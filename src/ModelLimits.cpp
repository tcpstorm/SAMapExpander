// Raises the number of DFF model ids from 20000 to whatever [World] ModelIdLimit asks for, so a
// converted map can bring its own objects along instead of fighting over SA's ~1370 free ids.
//
// SA numbers every streamable resource in one flat id space: DFF 0 (20000), TXD 20000 (5000),
// COL 25000 (255), IPL 25255 (256), DAT 25511 (64), IFP 25575 (180), RRR 25755 (475),
// SCM 26230 (82), then 4 list-head sentinels, 26316 entries in all. Adding N model ids therefore
// shifts every base above the DFF range by N, grows CStreaming::ms_aInfoForModel and
// CModelInfo::ms_modelInfoPtrs, and -- because CModelInfo::AddAtomicModel never checks its
// bounds -- has to grow the atomic model info store too. All three arrays are fixed globals, so
// they are reallocated and the 1407 instructions that name them or a shifted base are rewritten;
// tools/model_limit_sites.txt is that inventory and tools/scan_model_limit_sites.py rebuilds it.
//
// The ceiling is 26400 models: the streaming lists link entries through int16 indices, so the
// total entry count has to stay under 32768.
//
// Nothing is written unless every single site still holds the value the table expects, so another
// mod that moved one of these arrays first leaves the game vanilla instead of half-patched. Under
// SA-MP the whole thing is skipped: the server decides the id space there.
#include <plugin.h>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include "ModelLimits.h"
#include "ModelLimitSites.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr int kVanillaModelCount = 20000;
constexpr int kVanillaTotal = 26316;
constexpr int kMaxModelCount = 26400;

constexpr uintptr_t kInfoArray = 0x8E4CC0;     // CStreaming::ms_aInfoForModel[26316]
constexpr unsigned kInfoElem = 0x14;
constexpr uintptr_t kPtrArray = 0xA9B0C8;      // CModelInfo::ms_modelInfoPtrs[20000]
constexpr uintptr_t kArrayBasePtr = 0x9654B4;  // CStreamingInfo::ms_pArrayBase, filled by Init2

// CStore<CAtomicModelInfo, 14000>: an int32 count followed by the objects.
constexpr uintptr_t kAtomicStore = 0xAAE950;
constexpr unsigned kAtomicElem = 0x20;
constexpr int kVanillaAtomicCapacity = 14000;
constexpr int kAtomicHeadroom = 1000;
constexpr uint32_t kAtomicVTable = 0x85BBF0;   // written by CAtomicModelInfo's constructor, 0x4C5540

// Vanilla bases, in the order they follow the DFF range.
constexpr int kTxdBase = 20000, kColBase = 25000, kIplBase = 25255, kDatBase = 25511;
constexpr int kIfpBase = 25575, kRrrBase = 25755, kScmBase = 26230;

bool gActive = false;
int gModelCount = kVanillaModelCount;
uint8_t* gInfo = nullptr;
uint8_t* gPtrs = nullptr;
uint8_t* gStore = nullptr;

int Shift() { return gModelCount - kVanillaModelCount; }

uint32_t OperandAt(const ModelLimits::Site& s) {
    uintptr_t at = s.address + s.operandOffset;
    return s.operandSize == 2 ? patch::GetUShort(at) : (uint32_t)patch::GetUInt(at);
}

void WriteOperand(const ModelLimits::Site& s, uint32_t value) {
    uintptr_t at = s.address + s.operandOffset;
    if (s.operandSize == 2)
        patch::SetUShort(at, (unsigned short)value);
    else
        patch::SetUInt(at, value);
}

// An element of one of the shifted resource ranges keeps its position relative to the base it
// belongs to, so its index moves with the base; a DFF element keeps its index.
uint32_t ShiftedIndex(uint32_t index) {
    return index >= (uint32_t)kVanillaModelCount ? index + Shift() : index;
}

uint32_t NewValue(const ModelLimits::Site& s) {
    switch (s.kind) {
        case ModelLimits::kInfoArray: {
            uint32_t offset = s.oldValue - kInfoArray;
            uint32_t index = offset / kInfoElem, field = offset % kInfoElem;
            return (uint32_t)(uintptr_t)gInfo + ShiftedIndex(index) * kInfoElem + field;
        }
        case ModelLimits::kPtrArray:
            return (uint32_t)(uintptr_t)gPtrs + ShiftedIndex((s.oldValue - kPtrArray) / 4) * 4;
        case ModelLimits::kAtomicStore:
            return (uint32_t)(uintptr_t)gStore + (s.oldValue - kAtomicStore);
        case ModelLimits::kResourceBase:
        case ModelLimits::kSentinelIndex:
            return s.oldValue + Shift();
        case ModelLimits::kResourceBaseNeg:
            return s.oldValue - Shift() * s.scale;
        case ModelLimits::kCountTimes4:
            return (uint32_t)gModelCount * 4;
    }
    return s.oldValue;
}

bool AllSitesVanilla() {
    for (const ModelLimits::Site& s : ModelLimits::kSites) {
        uint32_t current = OperandAt(s);
        if (current != s.oldValue) {
            Log::Write("ERROR: model id limit not raised: 0x%X holds 0x%X, expected 0x%X "
                       "(another mod patched it); the game stays vanilla",
                       (unsigned)s.address, current, s.oldValue);
            return false;
        }
    }
    return true;
}

bool Allocate() {
    int total = kVanillaTotal + Shift();
    int atomicCapacity = kVanillaAtomicCapacity + Shift() + kAtomicHeadroom;

    gInfo = (uint8_t*)calloc((size_t)total, kInfoElem);
    gPtrs = (uint8_t*)calloc((size_t)gModelCount, 4);
    gStore = (uint8_t*)calloc(1, 4 + (size_t)atomicCapacity * kAtomicElem);
    if (!gInfo || !gPtrs || !gStore) {
        free(gInfo); free(gPtrs); free(gStore);
        gInfo = gPtrs = gStore = nullptr;
        Log::Write("ERROR: model id limit not raised: out of memory");
        return false;
    }

    // The store's objects are normally constructed by a static initialiser, and AddAtomicModel
    // calls a virtual on the next one before initialising it, so every slot needs its vtable and
    // the two fields CBaseModelInfo's constructor sets.
    for (int i = 0; i < atomicCapacity; i++) {
        uint8_t* object = gStore + 4 + (size_t)i * kAtomicElem;
        *(uint32_t*)object = kAtomicVTable;
        *(uint16_t*)(object + 8) = 0;
        *(uint16_t*)(object + 0xA) = 0xFFFF;
    }

    Log::Write("model arrays moved to the heap: streaming info %d x %u at %p, "
               "model info pointers %d at %p, atomic model infos %d at %p",
               total, kInfoElem, (void*)gInfo, gModelCount, (void*)gPtrs,
               atomicCapacity, (void*)(gStore + 4));
    return true;
}

}

namespace ModelLimits {

bool Active() { return gActive; }
int ModelCount() { return gModelCount; }
int TxdBase() { return gModelCount; }
int ColBase() { return kColBase + Shift(); }
int IplBase() { return kIplBase + Shift(); }
int DatBase() { return kDatBase + Shift(); }
int IfpBase() { return kIfpBase + Shift(); }
int RrrBase() { return kRrrBase + Shift(); }
int ScmBase() { return kScmBase + Shift(); }
int TotalCount() { return kVanillaTotal + Shift(); }

uintptr_t StreamingInfo() { return gActive ? (uintptr_t)gInfo : kInfoArray; }
uintptr_t ModelInfoPtrs() { return gActive ? (uintptr_t)gPtrs : kPtrArray; }
uintptr_t AtomicStore() { return gActive ? (uintptr_t)gStore : kAtomicStore; }
int AtomicCapacity() { return gActive ? kVanillaAtomicCapacity + Shift() + kAtomicHeadroom : kVanillaAtomicCapacity; }

void Install() {
    int wanted = Settings::world.modelIdLimit;
    if (wanted <= kVanillaModelCount) {
        if (wanted < kVanillaModelCount)
            Log::Write("ModelIdLimit %d is below the vanilla %d; left alone", wanted, kVanillaModelCount);
        return;
    }
    if (wanted > kMaxModelCount) {
        Log::Write("ModelIdLimit %d clamped to %d (the streaming lists index entries with int16)",
                   wanted, kMaxModelCount);
        wanted = kMaxModelCount;
    }
    if (GetModuleHandleA("samp.dll")) {
        Log::Write("SA-MP loaded: the model id limit stays at %d, the server owns the id space",
                   kVanillaModelCount);
        return;
    }
    // CStreaming::Init2 fills this in; if it is already set we are loading too late to move the
    // arrays out from under the game.
    if (patch::GetUInt(kArrayBasePtr) != 0) {
        Log::Write("ERROR: model id limit not raised: streaming is already initialised");
        return;
    }

    gModelCount = wanted;
    if (!AllSitesVanilla() || !Allocate()) {
        gModelCount = kVanillaModelCount;
        return;
    }

    int patched[kSiteKindCount] = {};
    for (const Site& s : kSites) {
        WriteOperand(s, NewValue(s));
        patched[s.kind]++;
    }

    int wrong = 0;
    for (const Site& s : kSites)
        if (OperandAt(s) != NewValue(s)) wrong++;
    if (wrong) {
        for (const Site& s : kSites) WriteOperand(s, s.oldValue);
        Log::Write("ERROR: model id limit not raised: %d site(s) did not take the new value; "
                   "the original values have been put back", wrong);
        gModelCount = kVanillaModelCount;
        free(gInfo); free(gPtrs); free(gStore);
        gInfo = gPtrs = gStore = nullptr;
        return;
    }
    patch::SetUInt(kArrayBasePtr, (uintptr_t)gInfo);

    gActive = true;
    Log::Write("model id limit %d -> %d: txd %d, col %d, ipl %d, dat %d, ifp %d, rrr %d, scm %d, "
               "%d streaming entries", kVanillaModelCount, gModelCount, TxdBase(), ColBase(),
               IplBase(), DatBase(), IfpBase(), RrrBase(), ScmBase(), TotalCount());
    Log::Write("model id limit: %d site(s) patched (array %d + %d + %d, resource base %d + %d, "
               "count %d, sentinel %d)", (int)std::size(kSites), patched[kInfoArray],
               patched[kPtrArray], patched[kAtomicStore], patched[kResourceBase],
               patched[kResourceBaseNeg], patched[kCountTimes4], patched[kSentinelIndex]);
}

}
