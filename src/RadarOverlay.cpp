#include <plugin.h>
#include <CRadar.h>
#include <CMenuManager.h>
#include <CSprite2d.h>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include "RadarOverlay.h"
#include "Settings.h"
#include "TileCache.h"
#include "Log.h"
#include "Gxt.h"

using namespace plugin;

struct TileRect {
    float west, east, north, south;
};

static float RectPointDistance(const TileRect& t, float x, float y) {
    float dx = std::fmax(std::fmax(t.west - x, 0.0f), x - t.east);
    float dy = std::fmax(std::fmax(t.south - y, 0.0f), y - t.north);
    return std::sqrt(dx * dx + dy * dy);
}

static void DrawTileHud(const TileRect& t, RwTexture* tex) {
    CVector2D world[4] = {
        { t.west, t.south }, { t.east, t.south }, { t.east, t.north }, { t.west, t.north }
    };
    CVector2D radar[4];
    for (int i = 0; i < 4; i++)
        CRadar::TransformRealWorldPointToRadarSpace(radar[i], world[i]);

    CVector2D clipped[8];
    int n = CRadar::ClipRadarPoly(clipped, radar);
    if (n <= 2) return;

    float posn[16], uv[16];
    float size = t.east - t.west;
    for (int i = 0; i < n; i++) {
        CVector2D w, s;
        CRadar::TransformRadarPointToRealWorldSpace(w, clipped[i]);
        CRadar::TransformRadarPointToScreenSpace(s, clipped[i]);
        posn[i * 2] = s.x;
        posn[i * 2 + 1] = s.y;
        uv[i * 2] = (w.x - t.west) / size;
        uv[i * 2 + 1] = (t.north - w.y) / size;
    }

    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, tex->raster);
    CSprite2d::SetVertices(n, posn, uv, CRGBA(255, 255, 255, 255));
    RwIm2DRenderPrimitive(rwPRIMTYPETRIFAN, CSprite2d::maVertices, n);
}

// PrintMap's tile loop scales with raw RsGlobal arithmetic, not CMenuManager::StretchX (which other
// mods hook for aspect correction), so do exactly what the loop does to stay glued to the vanilla tiles.
static float ScreenW() { return (float)*reinterpret_cast<const int*>(0xC17044); }
static float ScreenH() { return (float)*reinterpret_cast<const int*>(0xC17048); }
static float StretchRawX(float x) { float w = ScreenW(); return w == 640.0f ? x : x * w * (1.0f / 640.0f); }
static float StretchRawY(float y) { float h = ScreenH(); return h == 448.0f ? y : y * h * (1.0f / 448.0f); }

static void DrawTileMenu(const TileRect& t, RwTexture* tex) {
    CMenuManager& menu = FrontEndMenuManager;
    float tilePx = menu.m_fMapZoom / 6.0f;                 // 500 world units
    float offX = menu.m_fMapBaseX - menu.m_fMapZoom;       // world x = -3000
    float offY = menu.m_fMapBaseY - menu.m_fMapZoom;       // world y = +3000
    float x1 = offX + (t.west + 3000.0f) / 500.0f * tilePx;
    float x2 = offX + (t.east + 3000.0f) / 500.0f * tilePx;
    float y1 = offY + (3000.0f - t.north) / 500.0f * tilePx;
    float y2 = offY + (3000.0f - t.south) / 500.0f * tilePx;

    // Same memory order as PrintMap's rect: (x1, larger y, x2, smaller y).
    CRect rect;
    rect.left = StretchRawX(x1);
    rect.bottom = StretchRawY(y2);
    rect.right = StretchRawX(x2);
    rect.top = StretchRawY(y1);
    float screenW = ScreenW();
    float screenH = ScreenH();
    if (rect.right < 0.0f || rect.left > screenW || rect.bottom < 0.0f || rect.top > screenH) return;

    CRGBA white(255, 255, 255, 255);
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, tex->raster);
    CSprite2d::SetVertices(rect, white, white, white, white);
    RwIm2DRenderPrimitive(rwPRIMTYPETRIFAN, CSprite2d::maVertices, 4);
}

static void DrawCustomTiles(bool inMenu) {
    float cullRange = CRadar::m_radarRange * 1.5f;
    CVector2D origin = CRadar::vec2DRadarOrigin;

    for (size_t ri = 0; ri < Settings::regions.size(); ri++) {
        const MapRegion& region = Settings::regions[ri];
        for (int row = 0; row < region.tilesY; row++) {
            for (int col = 0; col < region.tilesX; col++) {
                TileRect t;
                t.west = region.TileWest(col);
                t.east = t.west + region.tileSize;
                t.north = region.TileNorth(row);
                t.south = t.north - region.tileSize;

                if (!inMenu && RectPointDistance(t, origin.x, origin.y) > cullRange) continue;

                RwTexture* tex = TileCache::Get((int)ri, region, col, row);
                if (!tex) continue;

                if (inMenu) DrawTileMenu(t, tex);
                else        DrawTileHud(t, tex);
            }
        }
    }
}

static void OnOverlayPoint(bool inMenu) {
    static bool logged[2] = { false, false };
    if (!logged[inMenu]) {
        logged[inMenu] = true;
        Log::Write("overlay hook fired (inMenu=%d) origin=(%.1f, %.1f) range=%.1f",
                   inMenu, CRadar::vec2DRadarOrigin.x, CRadar::vec2DRadarOrigin.y, CRadar::m_radarRange);
    }
    DrawCustomTiles(inMenu);
}

// Our tiles belong at the two `call CRadar::DrawRadarGangOverlay` sites (CRadar::DrawRadarMap for
// the HUD, CMenuManager::PrintMap for the pause map). SA-MP rewrites those sites for its own gang
// zones during its init, so we retarget the call (to a thunk that draws, then calls whatever was
// there) on the first game tick instead of at load, and only touch bytes we recognise.
typedef void(__cdecl* OverlayFn)(bool);
static OverlayFn gOriginalHud = nullptr;
static OverlayFn gOriginalMenu = nullptr;

static void __cdecl HudThunk(bool inMenu) {
    OnOverlayPoint(false);
    if (gOriginalHud) gOriginalHud(inMenu);
}

static void __cdecl MenuThunk(bool inMenu) {
    OnOverlayPoint(true);
    if (gOriginalMenu) gOriginalMenu(inMenu);
}

// `site` is the address of the `call rel32`. Returns the previous target (nullptr if the site was NOPed).
static bool RetargetCall(uintptr_t site, const char* name, OverlayFn thunk, OverlayFn& original) {
    unsigned char bytes[5];
    patch::GetRaw(site, bytes, sizeof(bytes));
    Log::Write("%s call site 0x%X: %02X %02X %02X %02X %02X", name, (unsigned)site, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

    bool allNops = true;
    for (unsigned char b : bytes) allNops &= (b == 0x90);

    if (bytes[0] == 0xE8) {
        int32_t rel;
        memcpy(&rel, bytes + 1, sizeof(rel));
        original = reinterpret_cast<OverlayFn>(site + 5 + rel);
    } else if (allNops) {
        original = nullptr;
    } else {
        Log::Write("%s call site has unexpected code; overlay not installed there", name);
        return false;
    }

    int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(thunk) - (site + 5));
    unsigned char call[5] = { 0xE8 };
    memcpy(call + 1, &rel, sizeof(rel));
    patch::SetRaw(site, call, sizeof(call));
    return true;
}

static bool gInstalled = false;

static void DumpBytes(const char* label, uintptr_t address, unsigned count) {
    unsigned char b[16];
    if (count > sizeof(b)) count = sizeof(b);
    patch::GetRaw(address, b, count);
    char text[64] = { 0 };
    for (unsigned i = 0; i < count; i++) snprintf(text + i * 3, sizeof(text) - i * 3, "%02X ", b[i]);
    Log::Write("%s @0x%X: %s", label, (unsigned)address, text);
}

static void InstallCallSites() {
    if (gInstalled) return;
    gInstalled = true;
    Log::Write("installing overlay call sites");
    DumpBytes("  DrawRadarGangOverlay", 0x586650, 8);
    // DrawRadarMap: `push 0` at 0x5869BD, `call` at 0x5869BF. If the site was NOPed, restore the push too.
    unsigned char push[2];
    patch::GetRaw(0x5869BD, push, sizeof(push));
    if (push[0] == 0x90 && push[1] == 0x90) patch::SetRaw(0x5869BD, (void*)"\x6A\x00", 2);
    RetargetCall(0x5869BF, "HUD", HudThunk, gOriginalHud);
    RetargetCall(0x5759E4, "menu", MenuThunk, gOriginalMenu);
}

namespace RadarOverlay {
    void Install() {
        DumpBytes("at load: DrawRadarGangOverlay", 0x586650, 8);
        DumpBytes("at load: HUD site", 0x5869BD, 7);
        DumpBytes("at load: menu site", 0x5759E4, 5);
        Events::gameProcessEvent += InstallCallSites;
    }

    void EnsureInstalled() {
        if (!gInstalled) {
            InstallCallSites();
            Gxt::EnsureHookIntact();
        }
    }
}
