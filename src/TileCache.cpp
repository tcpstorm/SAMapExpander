#include <plugin.h>
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <unordered_map>
#include "TileCache.h"
#include "Settings.h"
#include "Log.h"

static std::unordered_map<uint64_t, RwTexture*> gTiles;

static uint64_t Key(int region, int col, int row) {
    return ((uint64_t)(uint32_t)region << 32) | ((uint32_t)(uint16_t)row << 16) | (uint16_t)col;
}

static std::string TilePath(const MapRegion& region, int col, int row) {
    char name[512];
    int placeholders = 0;
    for (size_t i = 0; i + 1 < region.filePattern.size(); i++)
        if (region.filePattern[i] == '%' && region.filePattern[i + 1] != '%') placeholders++;

    if (placeholders >= 2)
        snprintf(name, sizeof(name), region.filePattern.c_str(), col, row);
    else
        snprintf(name, sizeof(name), region.filePattern.c_str(), row * region.tilesX + col);

    return region.tileFolder + "\\" + name;
}

static RwTexture* LoadPng(const std::string& path) {
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log::Write("  file not found");
        return nullptr;
    }

    RwImage* image = RtPNGImageRead(path.c_str());
    if (!image) {
        Log::Write("  RtPNGImageRead failed");
        return nullptr;
    }
    Log::Write("  image %dx%d depth %d", image->width, image->height, image->depth);

    RwInt32 width = 0, height = 0, depth = 0, format = 0;
    if (!RwImageFindRasterFormat(image, rwRASTERTYPETEXTURE, &width, &height, &depth, &format)) {
        Log::Write("  RwImageFindRasterFormat failed");
        RwImageDestroy(image);
        return nullptr;
    }
    Log::Write("  raster %dx%d depth %d format 0x%X", width, height, depth, format);

    RwRaster* raster = RwRasterCreate(width, height, depth, format);
    if (!raster) {
        Log::Write("  RwRasterCreate failed");
        RwImageDestroy(image);
        return nullptr;
    }
    if (!RwRasterSetFromImage(raster, image)) {
        Log::Write("  RwRasterSetFromImage failed");
        RwRasterDestroy(raster);
        RwImageDestroy(image);
        return nullptr;
    }
    RwImageDestroy(image);

    RwTexture* texture = RwTextureCreate(raster);
    if (!texture) {
        Log::Write("  RwTextureCreate failed");
        RwRasterDestroy(raster);
        return nullptr;
    }
    RwTextureSetFilterMode(texture, rwFILTERLINEAR);
    RwTextureSetAddressing(texture, rwTEXTUREADDRESSCLAMP);
    Log::Write("  ok");
    return texture;
}

namespace TileCache {
    RwTexture* Get(int regionIndex, const MapRegion& region, int col, int row) {
        uint64_t key = Key(regionIndex, col, row);
        auto it = gTiles.find(key);
        if (it != gTiles.end()) return it->second;

        std::string path = TilePath(region, col, row);
        Log::Write("[%s] loading tile (%d,%d): %s", region.name.c_str(), col, row, path.c_str());
        RwTexture* tex = LoadPng(path);
        gTiles[key] = tex;
        return tex;
    }

    void Clear() {
        for (auto& kv : gTiles)
            if (kv.second) RwTextureDestroy(kv.second);
        gTiles.clear();
    }
}
