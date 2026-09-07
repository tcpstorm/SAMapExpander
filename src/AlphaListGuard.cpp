// The one crash left standing, and the first one this project ever logged at Vice City's real
// offset: a wild call to FFFFFFFF out of CVisibilityPlugins::RenderFadingEntities.
//
// That function walks the alpha entity list - the entities being faded in or out at the edge of
// their draw distance - and for each node calls the render callback stored in it:
//
//     00733F11  mov  esi, [0xC88140]          ; first node
//     00733F20  mov  eax, [esi + 8]           ; distance
//     00733F23  mov  ecx, [esi]               ; entity
//     00733F27  call [esi + 4]                ; the callback - FFFFFFFF when it died
//     00733F2A  mov  esi, [esi + 0xC]         ; next
//     00733F30  cmp  esi, 0xC88120            ; the sentinel ends the walk
//
// At the crash the node held (B0F27BFF, FFFFFFFF, 192E0FD0): a float, minus one and a pointer,
// which is not a node. Either a next link led out of the list's pool, or a node inside it was
// overwritten by something else. Which of the two it is decides where to look, and this is what
// settles it: walk the same list the same way just before the game does, and the first node whose
// callback is not code inside gta_sa.exe is written down with its address, the address of the
// node that led to it, and the entity that node was fading - then the walk is cut off there, so
// the frame renders and the game stays up while the evidence is read.
//
// Debug only, and it changes nothing until a bad node is actually found.
#include <plugin.h>
#include <safetyhook.hpp>
#include <cstdint>
#include "AlphaListGuard.h"
#include "ModelLimits.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr uintptr_t kRenderFadingEntities = 0x733F10;
constexpr uintptr_t kListHead = 0xC88140;      // the word RenderFadingEntities starts from
constexpr uintptr_t kListSentinel = 0xC88120;  // the node address that ends its walk

struct AlphaNode {
    void* entity;
    void* callback;
    float distance;
    AlphaNode* next;
    AlphaNode* prev;
};

static SafetyHookInline& Hook() { static SafetyHookInline h; return h; }
int gReports = 0;

bool IsGameCode(const void* p) {
    const uintptr_t a = (uintptr_t)p;
    return (a >= 0x401000 && a < 0x860000) || (a >= 0x1556000 && a < 0x1577000);
}

// A CEntity's vtable is one of a handful in the .rdata band the entity classes live in - CBuilding,
// CTreadable, CDummy (0x8638C0), CObject and the rest cluster there. Recycled or freed memory whose
// "vtable" word happens to land in the code range still fails this, which is what let the second
// crash through: its vtable pointed at readable code whose +0x4C slot was FFFFFFFF, so the render
// callback jumped through junk. Combined with a model index in range, a false pass is very unlikely.
bool LooksLikeEntity(const void* p) {
    if (IsBadReadPtr(p, 0x40)) return false;
    const uint8_t* e = (const uint8_t*)p;
    const uintptr_t vtable = *(const uintptr_t*)e;
    if (vtable < 0x858000 || vtable >= 0x867000) return false;   // the entity vtable band
    const uint16_t model = *(const uint16_t*)(e + 0x22);
    return model < (uint16_t)ModelLimits::TotalCount();
}

// Walk the fading list ahead of the game and splice out ONLY the bad nodes, leaving the rest of
// the list joined up, then put every splice back the moment the render returns.
//
// The earlier version cut the list at the first bad node and dropped everything after it. That is
// why vehicles went invisible in a populated area: a car's alpha (its windows, and the fading car
// itself) sits later in the list than the one dangling foliage entity, so cutting at the foliage
// dropped the car's render too. Skipping just the bad node keeps every good entity, cars included.
//
// The list is transient - the game clears and rebuilds it every frame - so restoring the splices
// keeps the node pool's free list intact rather than orphaning nodes, which was its own corruption.
struct Splice { AlphaNode** loc; AlphaNode* original; };
const int kMaxSplices = 32;

void __cdecl Hooked_RenderFadingEntities() {
    Splice splices[kMaxSplices];
    int spliceCount = 0;

    AlphaNode** link = reinterpret_cast<AlphaNode**>(kListHead);  // the pointer feeding the walk
    int index = 0;
    while ((uintptr_t)*link != kListSentinel && spliceCount < kMaxSplices && index <= 4096) {
        AlphaNode* node = *link;
        const bool inRange = !IsBadReadPtr(node, sizeof(AlphaNode));
        const bool badNode = !inRange || !IsGameCode(node->callback);
        const bool badEntity = !badNode && !LooksLikeEntity(node->entity);
        if (!badNode && !badEntity) {          // good node - advance through its next pointer
            link = &node->next;
            index++;
            continue;
        }
        if (gReports < 12) {
            gReports++;
            Log::Write("alpha list: a fading node holds %s (node %08X, callback %08X, entity %08X); "
                       "spliced out for this frame, the rest of the list still renders",
                       inRange ? "something that is not an entity" : "unreadable memory",
                       (unsigned)(uintptr_t)node, inRange ? (unsigned)(uintptr_t)node->callback : 0u,
                       inRange ? (unsigned)(uintptr_t)node->entity : 0u);
        }
        splices[spliceCount].loc = link;
        splices[spliceCount].original = node;
        spliceCount++;
        // Skip past the bad node if we can read where it points; otherwise end the list here, since
        // its next pointer cannot be trusted.
        *link = inRange ? node->next : reinterpret_cast<AlphaNode*>(kListSentinel);
    }

    Hook().ccall<void>();

    for (int i = spliceCount - 1; i >= 0; i--) *splices[i].loc = splices[i].original;
}

}  // namespace

namespace AlphaListGuard {

void Install() {
    if (!Settings::world.debug) return;
    Hook() = safetyhook::create_inline(reinterpret_cast<void*>(kRenderFadingEntities),
                                       reinterpret_cast<void*>(&Hooked_RenderFadingEntities));
    if (!Hook()) Log::Write("alpha list guard: could not hook CVisibilityPlugins::RenderFadingEntities");
    else Log::Write("alpha list guard: watching the fading-entity list ahead of every render");
}

}  // namespace AlphaListGuard
