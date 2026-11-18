#include "ObjectNodePools.h"
#include "Log.h"
#include "MemPatch.h"
#include <cstdint>

// CPhysical::Add (0x544A30) files an object, vehicle or ped into the 16 x 16 grid of repeat
// sectors. It walks every 50-unit cell its bound rect covers - the rect being the collision
// model's bounding sphere, not its box - and takes one CEntryInfoNode and one CPtrNodeDoubleLink
// for each. Neither allocation is checked: a full entry-info pool is a null store at 0x544BC8, a
// full double-link pool one at 0x544BA4.
//
// San Andreas sizes these at 500 and 3200 for its own small props. SA-MP raises them, the
// entry-info pool to 50000, which is enough for objects of ordinary size and not for a city
// placed as objects: a land or seabed LOD with a radius over a kilometre covers thousands of
// cells by itself. Liberty City's LODsbed takes 11664 and lod_landne_3 another 2352, and the
// city's custom objects come to about 55000 between them, against Vice City's 42000.
//
// The sizes are raised where the pools are constructed rather than at the `push` operands that
// SA-MP and CityLoader write, so it does not matter which of them gets there first or last: the
// constructor is handed whichever size is larger.

namespace {

constexpr uintptr_t kDoubleLinkPoolCtorCall = 0x550F88;   // in CPools::Initialise
constexpr uintptr_t kDoubleLinkPoolCtor = 0x550250;
constexpr uintptr_t kEntryInfoPoolCtorCall = 0x550FC0;
constexpr uintptr_t kEntryInfoPoolCtor = 0x550320;

// 20 and 12 bytes a node, so about 6.5 MB for the pair. Both cities' objects streamed in at once
// would need half of this.
constexpr int kMinimumNodes = 200000;

using PoolCtor = void* (__thiscall*)(void* pool, int size, const char* name);

void* Construct(uintptr_t ctor, void* pool, int size, const char* name) {
    if (size < kMinimumNodes) {
        Log::Write("pools: %s pool %d -> %d entries", name, size, kMinimumNodes);
        size = kMinimumNodes;
    }
    return reinterpret_cast<PoolCtor>(ctor)(pool, size, name);
}

void* __fastcall DoubleLinkPoolThunk(void* pool, void*, int size, const char* name) {
    return Construct(kDoubleLinkPoolCtor, pool, size, name);
}

void* __fastcall EntryInfoPoolThunk(void* pool, void*, int size, const char* name) {
    return Construct(kEntryInfoPoolCtor, pool, size, name);
}

}   // namespace

namespace ObjectNodePools {

void Install() {
    MemPatch::RetargetCall(kDoubleLinkPoolCtorCall, kDoubleLinkPoolCtor,
                           reinterpret_cast<const void*>(&DoubleLinkPoolThunk));
    MemPatch::RetargetCall(kEntryInfoPoolCtorCall, kEntryInfoPoolCtor,
                           reinterpret_cast<const void*>(&EntryInfoPoolThunk));
}

}   // namespace ObjectNodePools
