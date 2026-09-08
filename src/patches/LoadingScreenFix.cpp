// San Andreas' loading screen runs off the end of its own splash array after 40 seconds.
//
// The game keeps seven loading screens: `CSprite2d gSplash[7]` at 0x00BAB35C, built by the vector
// constructor call at 0x0084F2DA (`push dtor; push ctor; push 7; push 4; push 0x00BAB35C`) and torn
// down by the loop at 0x00590CB4..0x00590CD4, which walks from 0x00BAB35C while `esi < 0x00BAB378`.
// So the last valid entry is index 6 and 0x00BAB378 is one past the end.
//
// Which one is shown is `gSplashIndex` at 0x008D093C. Two places advance it:
//
//   0x005904D0  `cmp dword ptr [0x008D093C], 6 / je out` before its `inc` - correctly bounded.
//   0x00590ADE  a bare `inc dword ptr [0x008D093C]` with nothing in front of it.
//
// The second one is reached from the loading screen's own tick (0x00590D40): it reads the
// millisecond clock, scales it by 0.001 to seconds, and advances when the elapsed time since the
// last advance passes 5.5 seconds for the first screen (0x00859B4C) and 5.0 for every one after
// (0x00858C80). Seven screens is therefore a budget of 5.5 + 6 * 5.0 = 35.5 seconds, and the next
// advance - 40.5 seconds into a load - writes 7 into the index. The draw at 0x0059001B then does
// `lea ecx, [ecx*4 + 0x00BAB35C]` and calls CSprite2d::Draw on 0x00BAB378, four bytes past the
// array.
//
// Vanilla San Andreas never loads for 40 seconds, and when it does it gets away with it: the four
// bytes past the array read zero, and CSprite2d::Draw (0x00728350) checks the texture immediately
// after setting the vertices - `mov eax, [esi] / test eax, eax / jnz` at 0x00728365 - and takes the
// untextured path when it is null. Nothing is drawn and nothing faults.
//
// This plugin does load for longer than 40 seconds - two imported cities is 17803 inst lines, 7052
// models and several hundred loose collision files - so it crosses the budget every run, and it
// crosses it sooner with [World] PackRegions on, because packing shrinks the sector from 121 units
// to 85 and every entity added to the world then has to be filed into more of them.
//
// It only becomes a crash with another mod in the way. GTASA.WidescreenFix.asi patches the
// `call CSprite2d::SetVertices` at 0x00728360 - inside CSprite2d::Draw, immediately after
// `mov esi, ecx` puts `this` in ESI - and its replacement (WidescreenFix+0xE4E0) captures ESI as the
// CSprite2d. When the rectangle covers the whole screen, which the loading splash's
// (-5, height+5, width+5, -5) does, it reads `this->m_pTexture->name` at +0x10 and looks the name up
// in a 384-entry table, with no null check on m_pTexture. With m_pTexture null that is a strlen of
// 0x00000010, which is exactly the fault the user saw: 0xC0000005 reading 0x00000010 at
// WidescreenFix+0xE550, ecx = 0x10, edx = 0x11, and [ebp-4] = 0x00BAB378 in the register dump.
// WidescreenFix has the null check the game has, a hundred bytes further down its own function
// (0x00BAB35C -> `mov ecx, [eax] / test ecx, ecx / je` at WidescreenFix+0xE5F5), just not on the
// path that reads the name.
//
// The faulting instruction is theirs, but the out-of-bounds index is San Andreas' and the long load
// is ours, so this is the piece to fix: bound the unguarded increment the way the game bounds the
// other one. The last loading screen then stays up for the rest of the load, which is what
// 0x005904D0 already does when it reaches the end.
#include <plugin.h>
#include <cstdint>
#include <cstring>
#include "LoadingScreenFix.h"
#include "MemPatch.h"
#include "Log.h"

namespace {

// `inc dword ptr [0x008D093C]`, the unbounded one, inside the routine at 0x00590AC0 that shows the
// next loading screen for 20 frames.
constexpr uintptr_t kAdvanceSite = 0x590ADE;
constexpr uintptr_t kSplashIndex = 0x8D093C;
constexpr int kLastSplash = 6;   // gSplash[7] at 0x00BAB35C, so 6 is the last one that exists

const uint8_t ORIGINAL[6] = { 0xFF, 0x05, 0x3C, 0x09, 0x8D, 0x00 };

// Called in place of the `inc`. Every register the site cares about is dead across it: the game
// reloads eax at 0x00590AF0, ecx at 0x00590B0D and edx at 0x00590B3D, esi is written by the
// `mov esi, 14h` this returns into, and that `mov` reads no flags.
void __cdecl AdvanceSplashBounded() {
    int& index = *reinterpret_cast<int*>(kSplashIndex);
    if (index < kLastSplash) index++;
}

}  // namespace

namespace LoadingScreenFix {

void Install() {
    uint8_t replacement[6] = { 0xE8, 0x00, 0x00, 0x00, 0x00, 0x90 };   // call rel32; nop
    const int32_t rel = (int32_t)((intptr_t)&AdvanceSplashBounded - (intptr_t)(kAdvanceSite + 5));
    std::memcpy(replacement + 1, &rel, sizeof(rel));

    if (MemPatch::ReplaceBytes(kAdvanceSite, ORIGINAL, replacement, sizeof(replacement)))
        Log::Write("loading screen: the splash advance at 0x%X now stops at the last of San "
                   "Andreas' 7 screens. Unbounded it reaches gSplash[7] - four bytes past the "
                   "array at 0x00BAB35C - after 40.5 seconds of loading, which this plugin's "
                   "load always exceeds; the game survives that but GTASA.WidescreenFix.asi "
                   "reads the splash's texture name without checking it and faults",
                   (unsigned)kAdvanceSite);
    else
        Log::Write("loading screen: the splash advance at 0x%X was not what was expected and has "
                   "been left alone; a load longer than 40 seconds may still index past San "
                   "Andreas' 7 loading screens", (unsigned)kAdvanceSite);
}

}  // namespace LoadingScreenFix
