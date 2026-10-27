#pragma once
// PS5 framebuffers are stored in a GPU-specific tiled/swizzled layout, not a plain row-by-row
// image, so a normal pixel buffer has to be rearranged ("tiled") before each flip or the screen
// comes out scrambled. This is the same algorithm used by the PS5 port of SDL
// (ps5-payload-dev/SDL, src/video/ps5/SDL_ps5tilemap.c, zlib license, Copyright (C) 2026
// John Törnblom <john.tornblom@gmail.com>), adapted here to plain types with no SDL dependency.
#include <cstddef>
#include <cstdint>

namespace nexus {

struct TileRect {
    int x = 0, y = 0, w = 0, h = 0;
};

class Ps5Tilemap {
public:
    static size_t bufferSize(uint32_t width, uint32_t height);

    bool create(uint32_t width, uint32_t height);
    ~Ps5Tilemap();

    // Copies `pixels` (row-major, `pitch` 32-bit words per row) into `tiled` (a destination
    // buffer sized by bufferSize()), re-tiling only what changed since the last call to this
    // buffer index. `bufIdx` selects which of the two flip buffers `tiled` is.
    void blit(const uint32_t* pixels, int pitch, void* tiled, int bufIdx, const TileRect* rects,
              int numRects, int offX, int offY);

private:
    static const int kTileWidth = 512, kTileHeight = 128, kTileBand = 8;
    static const int kTileSize = kTileWidth * kTileHeight;

    uint32_t width_ = 0, height_ = 0;
    uint32_t* colx_ = nullptr;
    uint32_t yoff_[kTileHeight] = {};
    TileRect pending_[2] = {};
    bool full_[2] = {true, true};
};

}  // namespace nexus
