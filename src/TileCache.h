#pragma once
#include <RenderWare.h>

struct MapRegion;

namespace TileCache {
    // Returns nullptr if the tile image is missing or failed to load (remembered, not retried).
    RwTexture* Get(int regionIndex, const MapRegion& region, int col, int row);
    void Clear();
}
