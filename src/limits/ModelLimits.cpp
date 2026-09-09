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
//
// Two model info stores are moved here that have nothing to do with the id space and everything to
// do with what a region's IDE lines become: the time store, which vanilla SA fills exactly, and the
// ped store, which it fills to 276 of 278. Both are CStore<T, N> at a fixed address with an adder
// that never checks its own count, so a bigger array is the whole fix; each has its own byte-checked
// site table so that a mismatch in one costs only what that store carries.
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

// CStore<CTimeModelInfo, 169>: what every tobj line becomes. Vanilla San Andreas fills it exactly,
// so a region's timed models - the lit night variants of its buildings - had nowhere to go and
// were dropped. Ten sites name it, all in CModelInfo, and AddTimeModel (0x4C66B0) never checks
// the count against 169: it multiplies by 0x24 and increments, so a bigger array is all it takes.
constexpr uintptr_t kTimeStore = 0xB1C960;
constexpr unsigned kTimeElem = 0x24;
constexpr int kVanillaTimeCapacity = 169;
constexpr int kTimeHeadroom = 1024;
constexpr uintptr_t kTimeModelInfoCtor = 0x4C5640;   // CTimeModelInfo::CTimeModelInfo, __thiscall

// CStore<CPedModelInfo, 278>: what every `peds` line becomes, and the reason a ped is not just an
// object with a skeleton as far as this file is concerned. CModelInfo::AddPedModel (0x4C67A0)
//
//     mov eax, [0xB478F8]      ; the count
//     imul esi, eax, 0x44      ; sizeof(CPedModelInfo)
//     add esi, 0xB478FC        ; the objects
//     inc eax / mov [0xB478F8], eax
//     ... mov [ecx*4 + 0xA9B0C8], esi
//
// multiplies and increments without ever comparing the count to 278 - the same shape as the time
// store and the weapon store - so a bigger array is all it takes. San Andreas' own data/peds.ide
// declares 276 of the 278, which leaves room for two imported peds and no more.
//
// The store lives at a fixed address rather than in the id space, so it is not in kSites and is
// moved by its own byte-verified table below; that also keeps a mismatch here from rolling back the
// id limit, which is the larger and more useful half.
constexpr uintptr_t kPedStoreCount = 0xB478F8;
constexpr uintptr_t kPedStoreObjects = 0xB478FC;
constexpr unsigned kPedElem = 0x44;
constexpr int kVanillaPedCapacity = 278;
constexpr int kPedHeadroom = 512;   // Vice City's 105 and Liberty City's 78, with room for a third
constexpr uintptr_t kPedModelInfoCtor = 0x4C57A0;   // __thiscall: base ctor, vtable 0x85BDC0, +0x34 = 0

struct StoreSite {
    uintptr_t address;
    uint32_t oldValue;
    uint8_t operandOffset;
};

// Every instruction in the first .text that names the ped store, from tools/xref.py. The two left
// alone - the CRT's vector constructor at 0x84BCF0 and its atexit destructor at 0x8562B0 - build
// and tear down the original static block, which simply goes unused.
const StoreSite kPedStoreSites[] = {
    { 0x4C6517, kPedStoreCount,   1 },   // Models::ShutDown      mov eax, [count]
    { 0x4C6522, kPedStoreObjects, 1 },   //                       mov edi, objects
    { 0x4C652E, kPedStoreCount,   1 },   //                       mov eax, [count]
    { 0x4C6608, kPedStoreCount,   2 },   //                       mov [count], ebp
    { 0x4C67A0, kPedStoreCount,   1 },   // Models::AddPedModel   mov eax, [count]
    { 0x4C67AB, kPedStoreObjects, 2 },   //                       add esi, objects
    { 0x4C67B2, kPedStoreCount,   1 },   //                       mov [count], eax
    { 0x4C6857, kPedStoreCount,   2 },   // Models::Initialise    mov [count], edi
};

// Vanilla bases, in the order they follow the DFF range.
constexpr int kTxdBase = 20000, kColBase = 25000, kIplBase = 25255, kDatBase = 25511;
constexpr int kIfpBase = 25575, kRrrBase = 25755, kScmBase = 26230;

bool gActive = false;
int gModelCount = kVanillaModelCount;
uint8_t* gInfo = nullptr;
uint8_t* gPtrs = nullptr;
uint8_t* gStore = nullptr;
uint8_t* gTimeStore = nullptr;
uint8_t* gPedStore = nullptr;
int gPedCapacity = kVanillaPedCapacity;

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
        case ModelLimits::kTimeStore:
            return (uint32_t)(uintptr_t)gTimeStore + (s.oldValue - kTimeStore);
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

// The ped model info store, moved the same way the weapon store is (RegionWeapons::RelocateStore):
// its own bytes checked, its own failure. Nothing here is required for the id limit, so a mismatch
// only costs a region its pedestrians - PedCapacity() then answers the vanilla 278, of which San
// Andreas' own peds.ide takes 276, and CityLoader drops the rest of a region's `peds` lines with
// the same "store full" line any other store gives.
void RelocatePedStore() {
    for (const StoreSite& s : kPedStoreSites) {
        uint32_t have = patch::GetUInt(s.address + s.operandOffset);
        if (have != s.oldValue) {
            Log::Write("ERROR: the ped model store was not moved: 0x%X holds 0x%X, expected 0x%X "
                       "(another mod patched it); a region's pedestrian models are left out",
                       (unsigned)s.address, have, s.oldValue);
            return;
        }
    }

    const int capacity = kVanillaPedCapacity + kPedHeadroom;
    gPedStore = (uint8_t*)calloc(1, 4 + (size_t)capacity * kPedElem);
    if (!gPedStore) {
        Log::Write("ERROR: the ped model store was not moved: out of memory");
        return;
    }

    // The vanilla store's objects are built by a static initialiser that has not run yet - this
    // plugin loads first - so ours are built the way the game builds them. CPedModelInfo's
    // constructor calls CBaseModelInfo's, writes its own vtable (0x85BDC0) and clears the pointer
    // at +0x34 that its destructor frees; it touches nothing outside the object.
    typedef void(__thiscall* PedModelInfoCtor)(void*);
    const PedModelInfoCtor construct = reinterpret_cast<PedModelInfoCtor>(kPedModelInfoCtor);
    for (int i = 0; i < capacity; i++) construct(gPedStore + 4 + (size_t)i * kPedElem);

    const uint32_t newCount = (uint32_t)(uintptr_t)gPedStore;
    const uint32_t newObjects = newCount + 4;
    for (const StoreSite& s : kPedStoreSites)
        patch::SetUInt(s.address + s.operandOffset,
                       s.oldValue == kPedStoreCount ? newCount : newObjects);

    gPedCapacity = capacity;
    Log::Write("ped model store moved to the heap: %d entries of %u bytes at %p (vanilla %d, of "
               "which San Andreas' own data/peds.ide takes 276)",
               capacity, kPedElem, (void*)(gPedStore + 4), kVanillaPedCapacity);
}

bool Allocate() {
    int total = kVanillaTotal + Shift();
    int atomicCapacity = kVanillaAtomicCapacity + Shift() + kAtomicHeadroom;

    gInfo = (uint8_t*)calloc((size_t)total, kInfoElem);
    gPtrs = (uint8_t*)calloc((size_t)gModelCount, 4);
    gStore = (uint8_t*)calloc(1, 4 + (size_t)atomicCapacity * kAtomicElem);
    const int timeCapacity = kVanillaTimeCapacity + kTimeHeadroom;
    gTimeStore = (uint8_t*)calloc(1, 4 + (size_t)timeCapacity * kTimeElem);
    if (!gInfo || !gPtrs || !gStore || !gTimeStore) {
        free(gInfo); free(gPtrs); free(gStore); free(gTimeStore);
        gInfo = gPtrs = gStore = gTimeStore = nullptr;
        Log::Write("ERROR: model id limit not raised: out of memory");
        return false;
    }

    // The time store's objects are built by their own constructor, the way the CRT builds the
    // vanilla ones: CStore<CTimeModelInfo, 169>'s constructor (0x4C5DD0) hands the element
    // constructor to the runtime's array-construction helper, and that constructor - CBaseModelInfo's,
    // then the vtable 0x85BCB0, then m_nOtherTimeModel = -1 - touches nothing but the object. The
    // vanilla store cannot be copied from instead: this plugin loads before the executable's static
    // initialisers run, so at this point its objects are still zero.
    typedef void(__thiscall* TimeModelInfoCtor)(void*);
    const TimeModelInfoCtor constructTimeModelInfo = reinterpret_cast<TimeModelInfoCtor>(kTimeModelInfoCtor);
    for (int i = 0; i < timeCapacity; i++)
        constructTimeModelInfo(gTimeStore + 4 + (size_t)i * kTimeElem);

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
               "model info pointers %d at %p, atomic model infos %d at %p, time model infos "
               "%d at %p (vanilla %d, which SA's own map fills)",
               total, kInfoElem, (void*)gInfo, gModelCount, (void*)gPtrs,
               atomicCapacity, (void*)(gStore + 4), timeCapacity, (void*)(gTimeStore + 4),
               kVanillaTimeCapacity);
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
uintptr_t TimeStore() { return gActive ? (uintptr_t)gTimeStore : kTimeStore; }
int TimeCapacity() { return gActive ? kVanillaTimeCapacity + kTimeHeadroom : kVanillaTimeCapacity; }
uintptr_t PedStore() { return gPedStore ? (uintptr_t)gPedStore : kPedStoreCount; }
int PedCapacity() { return gPedCapacity; }
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
    if (Settings::RunningUnderSaMp()) {
        Log::Write("SA-MP: the model id limit stays at %d, the server owns the id space",
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

    // After the id limit is settled, not before: a rolled-back id limit must leave the ped store
    // where it was too, so the game is either wholly patched or wholly vanilla.
    RelocatePedStore();

    gActive = true;
    Log::Write("model id limit %d -> %d: txd %d, col %d, ipl %d, dat %d, ifp %d, rrr %d, scm %d, "
               "%d streaming entries", kVanillaModelCount, gModelCount, TxdBase(), ColBase(),
               IplBase(), DatBase(), IfpBase(), RrrBase(), ScmBase(), TotalCount());
    Log::Write("model id limit: %d site(s) patched (array %d + %d + %d, time store %d, resource "
               "base %d + %d, count %d, sentinel %d)", (int)std::size(kSites), patched[kInfoArray],
               patched[kPtrArray], patched[kAtomicStore], patched[kTimeStore], patched[kResourceBase],
               patched[kResourceBaseNeg], patched[kCountTimes4], patched[kSentinelIndex]);
}

}
