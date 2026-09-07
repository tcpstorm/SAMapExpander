// Don't let one bad entity take the frame down, and say what it was.
//
// The render crash outlived a guard on the fading list because guarding that list was the wrong
// level. CVisibilityPlugins::RenderEntity (0x732B40) - the callback the fading list, the sorted
// list and the plain render loop all reach - begins by reading the entity's RwObject at +0x18 and
// its vtable, so a bad entity faults there whichever list it came from. And the entity is bad even
// though the fading list was clean when checked a moment earlier: rendering one entity can free or
// overwrite another later in the same pass, so the only place the check holds is immediately before
// the dereference. At the last crash the entity's vtable read 176F8AC8, a heap pointer - a live
// entity whose memory had been overwritten, not merely a dangling node.
//
// So hook RenderEntity, check the entity it is handed, and if it is not an entity return without
// drawing it - and log its model id the first few times, which names what is being corrupted. This
// is a net that keeps the game up while the corruption itself is chased; it is not the cure.
#include <plugin.h>
#include <safetyhook.hpp>
#include <cstdint>
#include "RenderGuard.h"
#include "ModelLimits.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr uintptr_t kRenderEntity = 0x732B40;   // void __cdecl CVisibilityPlugins::RenderEntity(CEntity*, float)

typedef void(__cdecl* RenderEntityFn)(void* entity, float distance);
static SafetyHookInline& Hook() { static SafetyHookInline h; return h; }
int gReports = 0;
long gSkipped = 0;

// A CEntity: readable, a vtable somewhere in the executable's .rdata, a model index in range, and a
// type in the valid set. The vtable range has to be wide enough to hold every entity kind, not just
// the map ones: an earlier version bounded it to 0x858000..0x867000, which is where CBuilding,
// CTreadable, CDummy and CObject sit, and skipped every vehicle - their vtables are higher
// (0x871120, 0x871528 were seen), so planes and cars rendered invisible. The band below covers all
// of them and still excludes the heap, where an overwritten object's vtable points (176F8AC8, ~390
// MB up); the type and model checks are the second and third gates.
bool LooksLikeEntity(const void* p) {
    if (!p || IsBadReadPtr(p, 0x40)) return false;
    const uint8_t* e = (const uint8_t*)p;
    const uintptr_t vtable = *(const uintptr_t*)e;
    if (vtable < 0x840000 || vtable >= 0x8B0000) return false;   // entity vtables live in .rdata
    if (*(const uint16_t*)(e + 0x22) >= (uint16_t)ModelLimits::TotalCount()) return false;
    const unsigned type = e[0x36] & 7;   // CEntity::m_nType: building/vehicle/ped/object/dummy
    return type >= 1 && type <= 5;
}

void __cdecl Hooked_RenderEntity(void* entity, float distance) {
    if (!LooksLikeEntity(entity)) {
        gSkipped++;
        if (gReports < 16) {
            gReports++;
            unsigned model = 0, vtable = 0;
            const bool readable = entity && !IsBadReadPtr(entity, 0x40);
            if (readable) {
                model = *(const uint16_t*)((const uint8_t*)entity + 0x22);
                vtable = *(const unsigned*)entity;
            }
            Log::Write("render guard: skipped a non-entity handed to RenderEntity (%08X, %s "
                       "vtable %08X, model %u); one entity has been corrupted or freed while live",
                       (unsigned)(uintptr_t)entity, readable ? "readable," : "unreadable,",
                       vtable, model);
            if (gReports == 16)
                Log::Write("render guard: (further skips counted but not logged)");
        }
        return;
    }
    Hook().ccall<void>(entity, distance);
}

}  // namespace

namespace RenderGuard {

void Install() {
    if (!Settings::world.debug) return;
    Hook() = safetyhook::create_inline(reinterpret_cast<void*>(kRenderEntity),
                                       reinterpret_cast<void*>(&Hooked_RenderEntity));
    if (!Hook()) Log::Write("render guard: could not hook CVisibilityPlugins::RenderEntity");
    else Log::Write("render guard: checking each entity before it is drawn");
}

}  // namespace RenderGuard
