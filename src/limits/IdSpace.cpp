// Counts what a region has spent of San Andreas' resource id space, and says so - because two of
// the three ranges it spends have no complaint of their own when they run out.
//
// THE SPACE. One flat numbering, in this order, with the vanilla sizes:
//
//     DFF 0 (20000)   TXD 20000 (5000)   COL 25000 (255)   IPL 25255 (256)   DAT 25511 (64)
//     IFP 25575 (180) RRR 25755 (475)    SCM 26230 (82)    + 4 list heads = 26316 entries
//
// ModelIdLimit grows the DFF range and shifts everything above it up by the same amount, but it
// grows nothing else: the TXD range is 5000 entries whatever the limit is, the COL range 255 and
// the IPL range 256. And the whole space has a hard ceiling of 32768 entries, because the streaming
// lists follow int16 indices into it (see ModelLimits.cpp), so those ranges cannot simply be made
// bigger either - there is nothing left to make them bigger with.
//
// WHY EACH RANGE MATTERS TO A REGION.
//
//   DFF - one id per model in the region's IDEs. CityLoader owns this one and already logs it.
//   TXD - one id per DISTINCT DICTIONARY NAME. CFileLoader::LoadObject creates a slot for the txd
//         column of every IDE line through CBaseModelInfo::SetTexDictionary, and
//         CStreaming::LoadCdDirectory (0x5B62A5) creates one for every .txd in every registered
//         archive that has no slot yet. Vice City brings 820 and Liberty City 422. San Andreas
//         itself takes 3687 of the 5000 - counted out of its own data/ tree and its three archives
//         - so two cities leave the range within a hundred of full.
//   COL - one id per .col FILE (not per model) inside an archive. San Andreas ships 251 of them
//         and the range is 255. A region's loose .col files go into slot 0, which the streamer
//         never touches, so they cost nothing here; a .col placed INSIDE a region's IMG archive
//         would cost one each and there are four to spend.
//   IPL - one id per streamed IPL section. San Andreas has 190 in its archives and the pool holds
//         256; a region's IPLs are loaded by CFileLoader::LoadScene rather than streamed, so they
//         cost none of these.
//
// WHAT RUNNING OUT LOOKS LIKE, and why this is worth logging rather than trusting. None of the
// three stores checks its range. CTxdStore::AddTxdSlot hands back whatever CPool::New gives it and
// LoadCdDirectory adds 20000 to it (`add eax, 0x4E20` at 0x5B62CE) with no comparison against the
// COL base; CColStore::AddColSlot is the same shape with 25000 (0x5B6313). So a 5001st texture
// dictionary is not refused - it is numbered 25000, which is collision slot 0, and the next one is
// collision slot 1, whose streaming entry San Andreas does use. That is a silent misbinding a long
// way from its cause, and CityLoader raising the CTxdStore POOL past 5000 (which it must, or
// AddTxdSlot writes through a null) makes it reachable without making it visible. Hence this.
#include <plugin.h>
#include <cstdint>
#include "IdSpace.h"
#include "ModelLimits.h"
#include "Log.h"

using namespace plugin;

namespace {

// The pool pointers, each read out of its own Initialise: CTxdStore::Initialise (0x731F79),
// CColStore::Initialise (0x41146F), CIplStore::Initialise (0x405F3D).
constexpr uintptr_t kTxdPool = 0xC8800C;
constexpr uintptr_t kColPool = 0x965560;
constexpr uintptr_t kIplPool = 0x8E3FB0;

// CPool: objects, byteMap, size, firstFree. A byteMap entry's top bit marks a free slot, so the
// highest slot in use is the last one without it - and it is the highest INDEX that matters here,
// not the count, because the index is what the id is built from.
struct PoolUse { int highest; int used; int size; bool read; };

PoolUse Use(uintptr_t poolPointer) {
    PoolUse out = { -1, 0, 0, false };
    const void** pool = *reinterpret_cast<const void***>(poolPointer);
    if (!pool) return out;
    const unsigned char* map = reinterpret_cast<const unsigned char*>(pool[1]);
    int size = reinterpret_cast<const int*>(pool)[2];
    if (!map || size <= 0 || size > 1 << 20) return out;
    out.read = true;
    out.size = size;
    for (int i = 0; i < size; i++)
        if (!(map[i] & 0x80)) { out.used++; out.highest = i; }
    return out;
}

// One line per range, and an error line for a range that has been walked out of.
void ReportRange(const char* what, const PoolUse& use, int ids, const char* over) {
    if (!use.read) {
        Log::Write("id space: %s - the store's pool could not be read; nothing to report", what);
        return;
    }
    int needed = use.highest + 1;
    Log::Write("id space: %s uses %d of %d id(s) (%d%%), highest slot %d, pool %d entries",
               what, needed, ids, ids ? needed * 100 / ids : 0, use.highest, use.size);
    if (needed > ids)
        Log::Write("ERROR: id space: %s has %d slot(s) past the end of its %d-id range. Nothing "
                   "refuses them: the slot number is turned into a streaming id by adding the "
                   "range's base, so slot %d and up take the entries of %s. Cut what the regions "
                   "need here, or take a region out",
                   what, needed - ids, ids, ids, over);
    else if (needed > ids - ids / 20)
        Log::Write("id space: %s has %d id(s) left of %d - the next region will not fit in it",
                   what, ids - needed, ids);
}

}   // namespace

namespace IdSpace {

void Report() {
    const int dffIds = ModelLimits::ModelCount();
    const int txdIds = ModelLimits::ColBase() - ModelLimits::TxdBase();
    const int colIds = ModelLimits::IplBase() - ModelLimits::ColBase();
    const int iplIds = ModelLimits::DatBase() - ModelLimits::IplBase();

    int models = 0;
    void** infos = reinterpret_cast<void**>(ModelLimits::ModelInfoPtrs());
    for (int i = 0; i < dffIds; i++)
        if (infos[i]) models++;

    Log::Write("id space: %d entries in all, of a hard ceiling of 32768 - DFF %d, TXD %d, COL %d, "
               "IPL %d, then DAT, IFP, RRR and SCM. Every one of them is an entry in "
               "CStreaming::ms_aInfoForModel and the streaming lists index it with an int16, so "
               "this is the whole budget and there are %d entries spare",
               ModelLimits::TotalCount(), dffIds, txdIds, colIds, iplIds,
               32768 - ModelLimits::TotalCount());
    Log::Write("id space: DFF uses %d of %d id(s) (%d%%), %d free",
               models, dffIds, dffIds ? models * 100 / dffIds : 0, dffIds - models);
    ReportRange("TXD", Use(kTxdPool), txdIds, "the collision files behind it");
    ReportRange("COL", Use(kColPool), colIds, "the IPL sections behind it");
    ReportRange("IPL", Use(kIplPool), iplIds, "the path data behind it");
}

}   // namespace IdSpace
