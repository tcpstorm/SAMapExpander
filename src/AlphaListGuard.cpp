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
uintptr_t gLowest = ~0u, gHighest = 0;   // the pool's extent, learned from nodes that were fine

bool IsGameCode(const void* p) {
    const uintptr_t a = (uintptr_t)p;
    return (a >= 0x401000 && a < 0x860000) || (a >= 0x1556000 && a < 0x1577000);
}

void __cdecl Hooked_RenderFadingEntities() {
    AlphaNode* node = *reinterpret_cast<AlphaNode**>(kListHead);
    AlphaNode* previous = nullptr;
    int index = 0;
    while ((uintptr_t)node != kListSentinel) {
        const bool inRange = !IsBadReadPtr(node, sizeof(AlphaNode));
        if (!inRange || !IsGameCode(node->callback)) {
            if (gReports < 8) {
                gReports++;
                const uintptr_t a = (uintptr_t)node;
                Log::Write("alpha list: node %d at %08X is not a node (%s callback %08X, entity %08X, "
                           "distance bits %08X); nodes seen so far lay in %08X..%08X, so this one is "
                           "%s the pool. Reached from node at %08X fading entity %08X%s. The walk is "
                           "cut here so the frame can render.",
                           index, (unsigned)a, inRange ? "readable," : "UNREADABLE,",
                           inRange ? (unsigned)(uintptr_t)node->callback : 0u,
                           inRange ? (unsigned)(uintptr_t)node->entity : 0u,
                           inRange ? *(unsigned*)&node->distance : 0u,
                           (unsigned)gLowest, (unsigned)gHighest,
                           (a >= gLowest && a <= gHighest) ? "INSIDE" : "outside",
                           previous ? (unsigned)(uintptr_t)previous : (unsigned)kListHead,
                           previous ? (unsigned)(uintptr_t)previous->entity : 0u,
                           previous && !IsBadReadPtr(previous->entity, 0x24)
                               ? "" : " (that entity is unreadable too)");
                if (previous && !IsBadReadPtr(previous->entity, 0x24)) {
                    const uint8_t* e = (const uint8_t*)previous->entity;
                    Log::Write("alpha list:   the entity before it: model %d, type/status byte %02X, "
                               "flags %08X, rwobject %08X", *(const int16_t*)(e + 0x22), e[0x36],
                               *(const uint32_t*)(e + 0x1C), *(const uint32_t*)(e + 0x18));
                }
            }
            // Cut the list at the last good node so RenderFadingEntities stops before the junk.
            if (previous) previous->next = reinterpret_cast<AlphaNode*>(kListSentinel);
            else *reinterpret_cast<uintptr_t*>(kListHead) = kListSentinel;
            break;
        }
        const uintptr_t a = (uintptr_t)node;
        if (a < gLowest) gLowest = a;
        if (a > gHighest) gHighest = a;
        previous = node;
        node = node->next;
        if (++index > 4096) {                     // a cycle; the same cure
            if (gReports < 8) { gReports++; Log::Write("alpha list: cycles after %d nodes; cut", index); }
            previous->next = reinterpret_cast<AlphaNode*>(kListSentinel);
            break;
        }
    }
    Hook().ccall<void>();
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
