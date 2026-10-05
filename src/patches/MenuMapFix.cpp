// CMenuManager::AdditionalOptionInput (0x578785..0x578821) keeps the pause map covering the
// 60..580 x 60..388 frame by comparing (x - zoom, x + zoom, y - zoom, y + zoom) against fixed
// constants, which pins scrolling to the vanilla +/-3000 map. We redirect those constant operands
// to values recomputed just before the block runs, so the extended world (vanilla + all regions)
// is what has to cover the frame instead.
#include <plugin.h>
#include <CMenuManager.h>
#include <CRadar.h>
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

// CRadar::ClipRadarPoly(out, in) clips a 4-point radar-space poly to the +/-1 square, and
// CRadar::DrawAreaOnRadar -- what draws a gang zone (SA-MP's zones go through it too) -- runs
// every rect through it. Opening the map page re-inits the radar (CMenuManager, 0x57517C) to
// origin (0, 0) and range 2990, so on the pause map that square is exactly the vanilla map: a
// zone in a region beyond it has all four corners outside one edge and clips away to nothing,
// which is why the same zone still shows on the HUD radar, where the origin follows the player.
// The map screen position is that same radar point scaled by m_fMapZoom, so the space has to
// stay as it is -- only the clip box may grow. It grows to the extent's radius rather than its
// box because DrawAreaOnRadar rotates radar space by the radar's cos/sin first.
static SafetyHookInline gClipRadarPolyHook;

static int ClipToBox(CVector2D* out, const CVector2D* in, int count, int axis, float limit, bool keepBelow) {
    int n = 0;
    for (int i = 0; i < count && n < 14; i++) {
        const CVector2D& a = in[i];
        const CVector2D& b = in[(i + 1) % count];
        float av = axis == 0 ? a.x : a.y;
        float bv = axis == 0 ? b.x : b.y;
        bool aIn = keepBelow ? av <= limit : av >= limit;
        bool bIn = keepBelow ? bv <= limit : bv >= limit;
        if (aIn) out[n++] = a;
        if (aIn != bIn) {
            float t = (limit - av) / (bv - av);
            out[n].x = a.x + (b.x - a.x) * t;
            out[n].y = a.y + (b.y - a.y) * t;
            n++;
        }
    }
    return n;
}

static int __cdecl Hooked_ClipRadarPoly(CVector2D* out, CVector2D* in) {
    RadarOverlay::EnsureInstalled();
    const CMenuManager& menu = FrontEndMenuManager;
    float range = CRadar::m_radarRange;
    if (!menu.m_bMenuActive || menu.m_nCurrentMenuPage != MENUPAGE_MAP || range <= 0.0f)
        return gClipRadarPolyHook.ccall<int>(out, in);

    float minX, maxX, minY, maxY;
    Settings::WorldExtent(minX, maxX, minY, maxY);
    const CVector2D& origin = CRadar::vec2DRadarOrigin;
    float dx = std::fmax(std::fabs(minX - origin.x), std::fabs(maxX - origin.x));
    float dy = std::fmax(std::fabs(minY - origin.y), std::fabs(maxY - origin.y));
    float limit = std::sqrt(dx * dx + dy * dy) / range;
    if (limit < 1.0f) limit = 1.0f;

    CVector2D a[16], b[16];
    int n = ClipToBox(a, in, 4, 0, limit, true);
    n = ClipToBox(b, a, n, 0, -limit, false);
    n = ClipToBox(a, b, n, 1, limit, true);
    n = ClipToBox(b, a, n, 1, -limit, false);
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) out[i] = b[i];
    return n;
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
        gClipRadarPolyHook = safetyhook::create_inline(reinterpret_cast<void*>(0x585040),
                                                      reinterpret_cast<void*>(&Hooked_ClipRadarPoly));

        float minX, maxX, minY, maxY;
        Settings::WorldExtent(minX, maxX, minY, maxY);
        Log::Write("pause map scroll extent: x %.0f..%.0f, y %.0f..%.0f (%s)", minX, maxX, minY, maxY, ok ? "ok" : "PARTIAL");
    }
}
