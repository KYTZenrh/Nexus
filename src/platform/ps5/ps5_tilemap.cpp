// Adapted from ps5-payload-dev/SDL's SDL_ps5tilemap.c (zlib license, Copyright (C) 2026
// John Törnblom <john.tornblom@gmail.com>). See ps5_tilemap.h for why this exists.
#include "ps5_tilemap.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define NEXUS_HAVE_AVX2 1
#endif

namespace nexus {

static const uint32_t kTileWidth = 512, kTileHeight = 128, kTileBand = 8;
static const uint32_t kTileSize = kTileWidth * kTileHeight;

static uint32_t tileOffset(uint32_t x, uint32_t y) {
    return (((x) & 1u) << 0 | ((x >> 1) & 1u) << 1 | ((y >> 0) & 1u) << 2 | ((y >> 1) & 1u) << 3 |
            ((y >> 2) & 1u) << 4 | ((x >> 2) & 1u) << 5 | (((x >> 3) ^ (y >> 3)) & 1u) << 6 |
            (((x >> 4) ^ (y >> 4)) & 1u) << 7 | (((x >> 6) ^ (y >> 5)) & 1u) << 8 |
            (((x >> 5) ^ (y >> 6)) & 1u) << 9 | ((y >> 3) & 1u) << 10 | ((x >> 4) & 1u) << 11 |
            ((y >> 6) & 1u) << 12 | ((x >> 6) & 1u) << 13 | ((x >> 7) & 1u) << 14 |
            ((x >> 8) & 1u) << 15);
}

static uint32_t tilePixel(uint32_t x, uint32_t y, uint32_t width) {
    return (x / kTileWidth) * kTileSize + (y / kTileHeight) * (kTileHeight * width) +
           (tileOffset(x % kTileWidth, 0) ^ tileOffset(0, y % kTileHeight));
}

// The display buffer is A8B8G8R8 (red in the low byte) while Nexus draws 0xAARRGGBB, so red and
// blue are swapped on the way in. Without this every colour came out mirrored on the console
// (the ice-blue accent showed as orange, bronze trophies as blue).
static inline uint32_t swapRB(uint32_t p) {
    return (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
}

#ifdef NEXUS_HAVE_AVX2
__attribute__((target("avx2")))
#endif
static void tileArea(const uint32_t* colx, const uint32_t* yoffTab, uint32_t width,
                     const uint32_t* src, uint32_t pitch, uint32_t* dst, uint32_t x0, uint32_t x1,
                     uint32_t y0, uint32_t y1) {
    const uint32_t quads = x1 / 4;
    for (uint32_t y = y0; y < y1; y += kTileBand) {
        uint32_t base = (y / kTileHeight) * (kTileHeight * width);
        uint32_t yo = yoffTab[y % kTileHeight];
        const uint32_t* s = src + (size_t)y * pitch;
        uint32_t* d = dst + base;
        uint32_t rows = std::min<uint32_t>(kTileBand, y1 - y);

#ifdef NEXUS_HAVE_AVX2
        const __m256i rb = _mm256_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
                                            2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
        for (uint32_t g = x0 / 4; g < quads; g++) {
            const uint32_t* p = s + ((size_t)g << 2);
            uint32_t* o = d + (colx[g] ^ yo);
            uint32_t k = 0;
            for (; k + 1 < rows; k += 2) {
                __m256i v = _mm256_inserti128_si256(
                    _mm256_castsi128_si256(_mm_loadu_si128((const __m128i*)p)),
                    _mm_loadu_si128((const __m128i*)(p + pitch)), 1);
                _mm256_stream_si256((__m256i*)o, _mm256_shuffle_epi8(v, rb));
                p += (size_t)2 * pitch;
                o += 8;
            }
            if (rows & 1)
                _mm_stream_si128((__m128i*)o, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)p),
                                                               _mm256_castsi256_si128(rb)));
        }
#else
        for (uint32_t g = x0 / 4; g < quads; g++)
            for (uint32_t k = 0; k < rows; k++)
                for (uint32_t j = 0; j < 4; j++)
                    (d + (colx[g] ^ yo))[k * 4 + j] = swapRB((s + ((size_t)g << 2) + (size_t)k * pitch)[j]);
#endif
        for (uint32_t k = 0; k < rows; k++) {
            const uint32_t* p = s + (size_t)k * pitch;
            for (uint32_t x = quads * 4; x < x1; x++) dst[tilePixel(x, y + k, width)] = swapRB(p[x]);
        }
    }
#ifdef NEXUS_HAVE_AVX2
    _mm_sfence();
#endif
}

static void rectUnion(TileRect& acc, const TileRect& r) {
    if (r.w <= 0 || r.h <= 0) return;
    if (acc.w <= 0 || acc.h <= 0) {
        acc = r;
        return;
    }
    int x0 = std::min(acc.x, r.x), y0 = std::min(acc.y, r.y);
    int x1 = std::max(acc.x + acc.w, r.x + r.w), y1 = std::max(acc.y + acc.h, r.y + r.h);
    acc = {x0, y0, x1 - x0, y1 - y0};
}

static bool intersect(const TileRect& a, const TileRect& b, TileRect& out) {
    int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    int x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return false;
    out = {x0, y0, x1 - x0, y1 - y0};
    return true;
}

size_t Ps5Tilemap::bufferSize(uint32_t width, uint32_t height) {
    uint32_t lastBand = ((height - 1) / kTileHeight) * (kTileHeight * width);
    uint32_t lastTile = ((width - 1) / kTileWidth) * kTileSize;
    return (size_t)(lastBand + lastTile + kTileSize) * sizeof(uint32_t);
}

bool Ps5Tilemap::create(uint32_t width, uint32_t height) {
    colx_ = (uint32_t*)std::malloc(sizeof(uint32_t) * (width / 4 + 1));
    if (!colx_) return false;
    for (uint32_t x = 0; x < width; x += 4)
        colx_[x / 4] = (x / kTileWidth) * kTileSize + tileOffset(x % kTileWidth, 0);
    for (uint32_t y = 0; y < kTileHeight; y++) yoff_[y] = tileOffset(0, y);
    width_ = width;
    height_ = height;
    pending_[0] = pending_[1] = {};
    full_[0] = full_[1] = true;
    return true;
}

Ps5Tilemap::~Ps5Tilemap() { std::free(colx_); }

void Ps5Tilemap::blit(const uint32_t* pixels, int pitch, void* tiled, int bufIdx,
                      const TileRect* rects, int numRects, int offX, int offY) {
    const TileRect screen = {0, 0, (int)width_, (int)height_};
    TileRect damage = {};
    if (rects && numRects > 0) {
        for (int i = 0; i < numRects; i++) {
            TileRect r = rects[i];
            r.x += offX;
            r.y += offY;
            TileRect clipped;
            if (intersect(r, screen, clipped)) rectUnion(damage, clipped);
        }
    } else {
        damage = screen;
    }

    TileRect area;
    if (full_[bufIdx]) {
        area = screen;
    } else {
        area = pending_[bufIdx];
        rectUnion(area, damage);
    }
    full_[bufIdx] = false;
    pending_[bufIdx] = {};
    rectUnion(pending_[bufIdx ^ 1], damage);
    if (area.w <= 0 || area.h <= 0) return;

    area.w += area.x & 3;
    area.x &= ~3;
    area.w = std::min((area.w + 3) & ~3, (int)width_ - area.x);
    area.h += area.y & (kTileBand - 1);
    area.y &= ~(kTileBand - 1);
    area.h = std::min((area.h + kTileBand - 1) & ~(kTileBand - 1), (int)height_ - area.y);

    tileArea(colx_, yoff_, width_, pixels, (uint32_t)pitch, (uint32_t*)tiled, area.x,
             area.x + area.w, area.y, area.y + area.h);
}

}  // namespace nexus
