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
// The ceiling is 26452 models, and it is a ceiling on the whole id space rather than on the models:
// the streaming lists follow int16 indices into ms_aInfoForModel, so there can be at most 32768
// entries, and 6316 of them are the ranges above the DFF one. See kMaxModelCount below for the
// three instructions that fix it and what going past them would take.
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

// The ceiling, and it is not a number anybody chose. Every entry in CStreaming::ms_aInfoForModel
// carries three int16 links into that same array - m_nNextIndex at +0, m_nPrevIndex at +2 and
// m_nNextIndexOnCd at +4 - and three instructions read one back SIGN-EXTENDED before using it as
// an index: 0x408B22 and 0x408B3E in CStreaming::RemoveFromList (`movsx eax, word [edi + info+0]`,
// then `lea edx, [eax + eax*4]` and `[ecx + edx*4]`, which is entry eax of the array) and 0x40CEDB
// in CStreaming::RequestModelStream. So the largest index the game can follow is 32767, and the
// number of ENTRIES is what has to stay inside it - not the number of models.
//
// That is the part it is easy to get wrong. The DFF range is only the bottom of one flat id space:
// TXD, COL, IPL, DAT, IFP, RRR and SCM all sit above it and every one of them is an entry in the
// same array, 6316 of them plus the four list-head sentinels. So the DFF ceiling is 32768 - 6316 =
// 26452, and at the 26400 this ships with there are FIFTY-TWO ids left in San Andreas' whole
// resource id space. There is no headroom between 26400 and 32767 to go and get; the space between
// those two numbers is already full of texture dictionaries and collision files.
//
// Going past it means making those three reads unsigned, and that is a larger job than flipping
// 0F BF to 0F B7: -1 is the sentinel these fields carry when an entry is not in a list, and the
// tests that recognise it (`cmp esi, -1` on a sign-extended 32-bit register) would each have to
// become a 16-bit test. See docs/region-limits.md for what that would buy and what it would cost.
constexpr int kMaxTotal = 32768;
constexpr int kMaxModelCount = kMaxTotal - (kVanillaTotal - kVanillaModelCount);   // 26452

constexpr uintptr_t kInfoArray = 0x8E4CC0;     // CStreaming::ms_aInfoForModel[26316]
constexpr unsigned kInfoElem = 0x14;
constexpr uintptr_t kPtrArray = 0xA9B0C8;      // CModelInfo::ms_modelInfoPtrs[20000]
constexpr uintptr_t kArrayBasePtr = 0x9654B4;  // CStreamingInfo::ms_pArrayBase, filled by Init2

// CStore<CAtomicModelInfo, 14000>: an int32 count followed by the objects.
constexpr uintptr_t kAtomicStore = 0xAAE950;
constexpr unsigned kAtomicElem = 0x20;
constexpr int kVanillaAtomicCapacity = 14000;
// The store is sized at one entry per model id rather than at "vanilla plus the ids we added",
// because the ids a region gets are no longer only the ones above 20000: CityLoader also draws on
// the 3799 free ids San Andreas leaves inside its own range. Every model id has at most one model
// info and only some of those are atomic - peds, vehicles, timed and clump models have stores of
// their own - so a store with one entry per id can never be the thing that runs out. It costs
// 0x20 bytes an id, which is 845KB at the ceiling.
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
// Vice City's 105 and Liberty City's 78, and room for three more cities of that size. A slot is
// 0x44 bytes, so the whole store is 57KB.
constexpr int kPedHeadroom = 1024;
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

// THE TWO SITES THAT MUST NOT MOVE WITH THE ARRAY, and the reason PickupSlots.cpp exists.
//
// CStreaming::Save (0x5D29A0) and CStreaming::Load (0x5D29E0) walk ms_aInfoForModel writing and
// reading ONE BYTE PER SLOT straight into the savegame:
//
//     mov esi, 0x8E4CC6            ; &ms_aInfoForModel[0].m_nFlags
//     ...  write 1 byte  ...       ; 0xFF, or the flags if the slot is loaded
//     add esi, 0x14
//     cmp esi, 0x9654B6            ; &ms_aInfoForModel[26316].m_nFlags
//     jl  loop
//
// 26316 bytes, and San Andreas' savegame has no framing of any kind - no block tag, no length, no
// offset table (docs/save-compatibility.md). A block that changes size does not fail; it silently
// shifts every block after it, and there are nine of them behind this one.
//
// The end operand names element 26316, which is inside the shifted range, so the general rule
// would repoint it at element 26316 + Shift and the block would grow by exactly the number of ids
// added - 6400 bytes at ModelIdLimit 26400. A save written that way is unreadable by anything with
// a different limit, and a save written by vanilla San Andreas is misread from PedType onwards.
//
// So these two get the new BASE and keep the vanilla COUNT, exactly as CPickups::Save and
// CPickups::Load do. The block stays 26316 bytes, in the same order, from the same offsets, and
// every block after it lines up.
//
// What that costs: the block now carries the flags of the relocated array's first 26316 slots -
// DFF ids 0..26315 - rather than the vanilla mixture of 20000 models and 6316 dictionaries,
// collision files and IPLs. Under one configuration Save and Load are symmetric, so it round-trips
// exactly. Across configurations, and in vanilla San Andreas, the bytes land on slots that mean
// something else; but that is a flags byte whose worst effect is a model held in memory that need
// not have been, and it was already true of every id above 20000. The alternative - hooking both
// functions so the bytes keep their vanilla meaning as well as their vanilla count - is available
// and is written down in docs/region-limits.md, and is only worth doing if that flag loss ever
// shows.
bool IsSaveBlockEnd(uintptr_t address) {
    return address == 0x5D29D3 || address == 0x5D2A0A;
}

uint32_t NewValue(const ModelLimits::Site& s) {
    switch (s.kind) {
        case ModelLimits::kInfoArray: {
            uint32_t offset = s.oldValue - kInfoArray;
            uint32_t index = offset / kInfoElem, field = offset % kInfoElem;
            if (IsSaveBlockEnd(s.address))
                return (uint32_t)(uintptr_t)gInfo + index * kInfoElem + field;
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
    int atomicCapacity = gModelCount;   // one per model id: see kVanillaAtomicCapacity

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
int AtomicCapacity() { return gActive ? gModelCount : kVanillaAtomicCapacity; }

void Install() {
    int wanted = Settings::world.modelIdLimit;
    if (wanted <= kVanillaModelCount) {
        if (wanted < kVanillaModelCount)
            Log::Write("ModelIdLimit %d is below the vanilla %d; left alone", wanted, kVanillaModelCount);
        return;
    }
    if (wanted > kMaxModelCount) {
        Log::Write("ModelIdLimit %d clamped to %d: the streaming lists follow int16 indices into "
                   "ms_aInfoForModel, so the whole id space has to fit in %d entries, and %d of "
                   "them are the TXD, COL, IPL, DAT, IFP, RRR and SCM ranges above the models",
                   wanted, kMaxModelCount, kMaxTotal, kVanillaTotal - kVanillaModelCount);
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

    // Say it out loud, because it is the one thing here that a user's existing saves depend on and
    // the one thing a future change to the site table could quietly undo.
    Log::Write("model id limit: the savegame's Streaming block still spans exactly %d slot(s), one "
               "byte each, because CStreaming::Save and CStreaming::Load keep the vanilla count "
               "while taking the new base - the block is the same size vanilla San Andreas writes, "
               "so every block after it lines up", kVanillaTotal);

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
