// CMenuManager::AdditionalOptionInput (0x578785..0x578821) keeps the pause map covering the
// 60..580 x 60..388 frame by comparing (x - zoom, x + zoom, y - zoom, y + zoom) against fixed
// constants, which pins scrolling to the vanilla +/-3000 map. We redirect those constant operands
// to values recomputed just before the block runs, so the extended world (vanilla + all regions)
// is what has to cover the frame instead.
#include <plugin.h>
#include <CMenuManager.h>
#include <safetyhook.hpp>
#include <cmath>
#include "MenuMapFix.h"
#include "RadarOverlay.h"
#include "MemPatch.h"
#include "Settings.h"
#include "Log.h"

static SafetyHookMid gClampHook;

static float gLeftLimit = 60.0f;
static float gRightLimit = 580.0f;
static float gTopLimit = 60.0f;
static float gBottomLimit = 388.0f;

static void UpdateLimits(const CMenuManager& menu) {
    float minX, maxX, minY, maxY;
    Settings::WorldExtent(minX, maxX, minY, maxY);

    float zoom = menu.m_fMapZoom;
    float scale = zoom / 3000.0f;
    gLeftLimit   = 60.0f  - zoom - minX * scale;
    gRightLimit  = 580.0f + zoom - maxX * scale;
    gTopLimit    = 60.0f  - zoom + maxY * scale;
    gBottomLimit = 388.0f + zoom + minY * scale;
}

static void OnBeforeClamp(safetyhook::Context& ctx) {
    UpdateLimits(*reinterpret_cast<CMenuManager*>(ctx.esi));
}

// AdditionalOptionInput+0x45 stores the map's screen bounds (y-zoom, y+zoom, x-zoom, x+zoom) in
// stack slots that the scroll gates read later. Rewrite them with the extended extent.
static SafetyHookMid gBoundsHook;

static void OnBoundsComputed(safetyhook::Context& ctx) {
    const CMenuManager& menu = *reinterpret_cast<CMenuManager*>(ctx.esi);
    float* frame = reinterpret_cast<float*>(ctx.esp);
    float minX, maxX, minY, maxY;
    Settings::WorldExtent(minX, maxX, minY, maxY);
    float scale = menu.m_fMapZoom / 3000.0f;
    frame[0x80 / 4] = menu.m_fMapBaseY - maxY * scale;   // top
    frame[0x30 / 4] = menu.m_fMapBaseY - minY * scale;   // bottom
    frame[0x78 / 4] = menu.m_fMapBaseX + minX * scale;   // left
    frame[0x34 / 4] = menu.m_fMapBaseX + maxX * scale;   // right
}

// CRadar::LimitToMap(float* x, float* y) clamps a screen point (cursor, blips) to the vanilla
// map box origin +/- zoom, scaled to the real resolution. Same thing, extended world.
static SafetyHookInline gLimitToMapHook;

static void __cdecl Hooked_LimitToMap(float* x, float* y) {
    RadarOverlay::EnsureInstalled();
    const CMenuManager& menu = FrontEndMenuManager;
    bool mapLoaded = *reinterpret_cast<const unsigned char*>(0xBA67C0) != 0;
    float zoom = mapLoaded ? menu.m_fMapZoom : 140.0f;
    float scale = zoom / 3000.0f;
    float sx = *reinterpret_cast<const int*>(0xC17044) / 640.0f;
    float sy = *reinterpret_cast<const int*>(0xC17048) / 448.0f;

    float minX, maxX, minY, maxY;
    Settings::WorldExtent(minX, maxX, minY, maxY);

    float left   = (menu.m_fMapBaseX + minX * scale) * sx;
    float right  = (menu.m_fMapBaseX + maxX * scale) * sx;
    float top    = (menu.m_fMapBaseY - maxY * scale) * sy;
    float bottom = (menu.m_fMapBaseY - minY * scale) * sy;

    if (*x < left) *x = left;
    if (*x > right) *x = right;
    if (*y < top) *y = top;
    if (*y > bottom) *y = bottom;
}

// CRadar::LimitRadarPoint(CVector2D* p) clamps a radar-space point to the unit circle unless
// m_bDrawingMap is set. Mouse cursor handling on the map screen runs before that flag is set,
// so the cursor gets pinned to a 2990-unit circle. Skip the clamp whenever the map page is open.
static SafetyHookInline gLimitRadarPointHook;

static float __cdecl Hooked_LimitRadarPoint(CVector2D* point) {
    RadarOverlay::EnsureInstalled();
    const CMenuManager& menu = FrontEndMenuManager;
    if (menu.m_bMenuActive && menu.m_nCurrentMenuPage == MENUPAGE_MAP)
        return std::sqrt(point->x * point->x + point->y * point->y);
    return gLimitRadarPointHook.ccall<float>(point);
}

// AdditionalOptionInput+0x1451..+0x14A4 clamps the cursor's world position to +/-3000 with one
// register per bound shared by both axes. Disable those compares and clamp to the extent ourselves.
static SafetyHookMid gCursorClampHook;
static float gNeverAbove = 1.0e30f;
static float gNeverBelow = -1.0e30f;

static void OnAfterCursorClamp(safetyhook::Context& ctx) {
    float* cursor = reinterpret_cast<float*>(ctx.esi + 0x70);
    float minX, maxX, minY, maxY;
    Settings::WorldExtent(minX, maxX, minY, maxY);
    if (cursor[0] > maxX) cursor[0] = maxX;
    if (cursor[0] < minX) cursor[0] = minX;
    if (cursor[1] > maxY) cursor[1] = maxY;
    if (cursor[1] < minY) cursor[1] = minY;
}

namespace MenuMapFix {
    void Install() {
        bool ok = true;
        // fcom/fsub dword ptr [60.0]  (D8 15 / D8 25) -> left, then top
        ok &= MemPatch::RetargetDisp32(0x5787A5, 2, 0x858B34, &gLeftLimit);
        ok &= MemPatch::RetargetDisp32(0x5787B2, 2, 0x858B34, &gLeftLimit);
        ok &= MemPatch::RetargetDisp32(0x5787E3, 2, 0x858B34, &gTopLimit);
        ok &= MemPatch::RetargetDisp32(0x5787F0, 2, 0x858B34, &gTopLimit);
        // fcomp/fld dword ptr [580.0]  (D8 1D / D9 05) -> right
        ok &= MemPatch::RetargetDisp32(0x5787C6, 2, 0x865260, &gRightLimit);
        ok &= MemPatch::RetargetDisp32(0x5787D3, 2, 0x865260, &gRightLimit);
        // fcomp/fld dword ptr [388.0] -> bottom
        ok &= MemPatch::RetargetDisp32(0x578804, 2, 0x86525C, &gBottomLimit);
        ok &= MemPatch::RetargetDisp32(0x578811, 2, 0x86525C, &gBottomLimit);

        gClampHook = safetyhook::create_mid(reinterpret_cast<void*>(0x578785), OnBeforeClamp);
        gBoundsHook = safetyhook::create_mid(reinterpret_cast<void*>(0x577445), OnBoundsComputed);

        // fcomp dword ptr [3000.0] / [-3000.0] (D8 1D disp32) on cursor x/y
        ok &= MemPatch::RetargetDisp32(0x578829, 2, 0x859A94, &gNeverAbove);
        ok &= MemPatch::RetargetDisp32(0x578841, 2, 0x859A90, &gNeverBelow);
        ok &= MemPatch::RetargetDisp32(0x578854, 2, 0x859A94, &gNeverAbove);
        ok &= MemPatch::RetargetDisp32(0x578867, 2, 0x859A90, &gNeverBelow);
        gCursorClampHook = safetyhook::create_mid(reinterpret_cast<void*>(0x578877), OnAfterCursorClamp);
        gLimitToMapHook = safetyhook::create_inline(reinterpret_cast<void*>(0x583350),
                                                    reinterpret_cast<void*>(&Hooked_LimitToMap));
        gLimitRadarPointHook = safetyhook::create_inline(reinterpret_cast<void*>(0x5832F0),
                                                         reinterpret_cast<void*>(&Hooked_LimitRadarPoint));

        float minX, maxX, minY, maxY;
        Settings::WorldExtent(minX, maxX, minY, maxY);
        Log::Write("pause map scroll extent: x %.0f..%.0f, y %.0f..%.0f (%s)", minX, maxX, minY, maxY, ok ? "ok" : "PARTIAL");
    }
}
