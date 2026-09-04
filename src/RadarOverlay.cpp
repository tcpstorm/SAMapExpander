#include <plugin.h>
#include <CRadar.h>
#include <CMenuManager.h>
#include <CSprite2d.h>
#include <safetyhook.hpp>
#include <cmath>
#include "RadarOverlay.h"
#include "Settings.h"
#include "TileCache.h"
#include "Log.h"

using namespace plugin;

static SafetyHookInline gGangOverlayHook;

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

static void __cdecl Hooked_DrawRadarGangOverlay(bool inMenu) {
    static bool logged[2] = { false, false };
    if (!logged[inMenu]) {
        logged[inMenu] = true;
        Log::Write("overlay hook fired (inMenu=%d) origin=(%.1f, %.1f) range=%.1f",
                   inMenu, CRadar::vec2DRadarOrigin.x, CRadar::vec2DRadarOrigin.y, CRadar::m_radarRange);
    }
    DrawCustomTiles(inMenu);
    gGangOverlayHook.ccall<void>(inMenu);
}

namespace RadarOverlay {
    void Install() {
        gGangOverlayHook = safetyhook::create_inline(reinterpret_cast<void*>(0x586650),
                                                     reinterpret_cast<void*>(&Hooked_DrawRadarGangOverlay));
    }
}
