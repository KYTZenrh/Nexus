#include "gfx.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>

#include "../../third_party/stb/stb_image.h"
#include "../../third_party/stb/stb_truetype.h"

namespace nexus {

static const float kPi = 3.14159265f;
// Float-only trig: with the console toolchain's headers std::sin/std::cos on a float return double,
// which is a narrowing error inside the brace-initialised vertex structs below.
static inline float cosF(float x) { return (float)std::cos((double)x); }
static inline float sinF(float x) { return (float)std::sin((double)x); }

Col lerp(Col a, Col b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [&](uint8_t x, uint8_t y) { return (uint8_t)(x + (y - x) * t + 0.5f); };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

Col withA(Col c, float mul) {
    c.a = (uint8_t)std::clamp(c.a * mul, 0.0f, 255.0f);
    return c;
}

// A vertex: position, colour channels (0..255, alpha already includes the global alpha) and
// texture coordinates (0..1).
struct Gfx::V {
    float x, y, r, g, b, a, u, v;
};

struct Gfx::FontFace {
    std::vector<unsigned char> data;
    stbtt_fontinfo info;
    bool ok = false;
};

Gfx::Gfx() = default;
Gfx::~Gfx() {
    if (worker_.joinable()) worker_.join();
}

// ---------------------------------------------------------------- setup

static bool readFile(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)n);
    size_t got = std::fread(out.data(), 1, (size_t)n, f);
    std::fclose(f);
    return got == (size_t)n;
}

bool Gfx::init(const std::string& dir) {
    static const char* files[4] = {"Barlow-Regular.ttf", "Barlow-Medium.ttf", "Barlow-SemiBold.ttf",
                                   "Barlow-Bold.ttf"};
    for (int i = 0; i < 4; i++) {
        auto face = std::make_unique<FontFace>();
        std::string path = dir + "/fonts/" + files[i];
        if (readFile(path, face->data) &&
            stbtt_InitFont(&face->info, face->data.data(),
                           stbtt_GetFontOffsetForIndex(face->data.data(), 0)))
            face->ok = true;
        else
            std::fprintf(stderr, "Font load failed: %s\n", path.c_str());
        faces_[i] = std::move(face);
    }
    return faces_[0]->ok;
}

void Gfx::setTarget(uint32_t* pixels, int w, int h) {
    target_ = pixels;
    tw_ = w;
    th_ = h;
    unclip();
}

void Gfx::clear(Col c) {
    uint32_t v = 0xFF000000u | (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | c.b;
    std::fill(target_, target_ + (size_t)tw_ * th_, v);
}

void Gfx::clip(float x, float y, float w, float h) {
    cx0_ = std::max(0, (int)x);
    cy0_ = std::max(0, (int)y);
    cx1_ = std::min(tw_, (int)(x + w));
    cy1_ = std::min(th_, (int)(y + h));
}

void Gfx::unclip() {
    cx0_ = cy0_ = 0;
    cx1_ = tw_;
    cy1_ = th_;
}

// ---------------------------------------------------------------- cached layers

bool Gfx::blitCache(const std::string& name) {
    auto it = layers_.find(name);
    if (it == layers_.end() || (int)it->second.size() != tw_ * th_) return false;
    std::memcpy(target_, it->second.data(), it->second.size() * sizeof(uint32_t));
    return true;
}

void Gfx::beginCache(const std::string& name, int w, int h) {
    saved_ = {target_, tw_, th_};
    auto& v = layers_[name];
    v.assign((size_t)w * h, 0xFF000000u);
    target_ = v.data();
    tw_ = w;
    th_ = h;
    unclip();
}

void Gfx::endCache() {
    target_ = saved_.target;
    tw_ = saved_.tw;
    th_ = saved_.th;
    unclip();
}

// ---------------------------------------------------------------- rasterizer

static inline uint32_t div255(uint32_t t) { return (t + 128 + ((t + 128) >> 8)) >> 8; }

// Bilinear sample with premultiplied weighting, so transparent texels never bleed their
// (meaningless) colour into the edge of an image.
static uint32_t sampleBilinear(const Tex::Level& L, float u, float v) {
    float fx = u * L.w - 0.5f, fy = v * L.h - 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    int wx = (int)((fx - x0) * 256.0f), wy = (int)((fy - y0) * 256.0f);
    int x1 = std::clamp(x0 + 1, 0, L.w - 1), y1 = std::clamp(y0 + 1, 0, L.h - 1);
    x0 = std::clamp(x0, 0, L.w - 1);
    y0 = std::clamp(y0, 0, L.h - 1);
    uint32_t c00 = L.px[(size_t)y0 * L.w + x0], c10 = L.px[(size_t)y0 * L.w + x1];
    uint32_t c01 = L.px[(size_t)y1 * L.w + x0], c11 = L.px[(size_t)y1 * L.w + x1];
    int w00 = (256 - wx) * (256 - wy), w10 = wx * (256 - wy), w01 = (256 - wx) * wy, w11 = wx * wy;
    if ((c00 & c10 & c01 & c11) >> 24 == 255) {  // all opaque (most of a cover): plain bilinear
        auto op = [&](int sh) {
            return (uint32_t)((((c00 >> sh) & 255) * w00 + ((c10 >> sh) & 255) * w10 +
                               ((c01 >> sh) & 255) * w01 + ((c11 >> sh) & 255) * w11) >> 16);
        };
        return 0xFF000000u | op(16) << 16 | op(8) << 8 | op(0);
    }
    uint64_t a00 = (c00 >> 24) * (uint64_t)w00, a10 = (c10 >> 24) * (uint64_t)w10;
    uint64_t a01 = (c01 >> 24) * (uint64_t)w01, a11 = (c11 >> 24) * (uint64_t)w11;
    uint64_t asum = a00 + a10 + a01 + a11;
    if (asum == 0) return 0;
    auto ch = [&](int sh) {
        uint64_t s = ((c00 >> sh) & 255) * a00 + ((c10 >> sh) & 255) * a10 +
                     ((c01 >> sh) & 255) * a01 + ((c11 >> sh) & 255) * a11;
        return (uint32_t)(s / asum);
    };
    uint32_t a = (uint32_t)(asum >> 16);
    return std::min(a, 255u) << 24 | ch(16) << 16 | ch(8) << 8 | ch(0);
}

// One row of a tinted image: (u, v) at the first pixel and its step per pixel, in 0..1 texture
// space. Texture coordinates run in 16.16 fixed point and blending is premultiplied integer
// maths. A pixel landing exactly on a texel centre (text drawn at its own size) skips filtering.
static void texSpan(const Tex::Level& L, uint32_t* row, int xs, int xe, float u, float v, float du,
                    float dv, float tr, float tg, float tb, float ta) {
    const int ia = std::clamp((int)(ta + 0.5f), 0, 255);
    if (ia == 0) return;
    const uint32_t r8 = (uint32_t)std::clamp((int)(tr + 0.5f), 0, 255);
    const uint32_t g8 = (uint32_t)std::clamp((int)(tg + 0.5f), 0, 255);
    const uint32_t b8 = (uint32_t)std::clamp((int)(tb + 0.5f), 0, 255);
    const bool plain = r8 == 255 && g8 == 255 && b8 == 255;
    const bool grey = r8 == g8 && g8 == b8;
    const uint32_t tw = r8 + (r8 >> 7);  // grey tint as a 0..256 weight
    int32_t U = (int32_t)((u * L.w - 0.5f) * 65536.0f), V = (int32_t)((v * L.h - 0.5f) * 65536.0f);
    int32_t dU = (int32_t)(du * L.w * 65536.0f);
    const int32_t dV = (int32_t)(dv * L.h * 65536.0f);
    const int maxX = L.w - 1, maxY = L.h - 1;
    const uint32_t* px = L.px.data();

    // Tints, fades and blends one premultiplied source pixel into row[x].
    auto put = [&](int x, uint32_t sa, uint32_t sr, uint32_t sg, uint32_t sb) {
        if (grey && sa >= 255) {  // opaque texel, grey tint (dimmed covers) and/or faded: packed maths
            uint32_t c = sr << 16 | sg << 8 | sb;
            if (!plain)
                c = ((((c & 0xFF00FFu) * tw) >> 8) & 0xFF00FFu) | ((((c & 0x00FF00u) * tw) >> 8) & 0x00FF00u);
            if (ia == 255) {
                row[x] = 0xFF000000u | c;
                return;
            }
            const uint32_t w = (uint32_t)ia + ((uint32_t)ia >> 7);  // 0..256
            const uint32_t d = row[x];
            const uint32_t rb = (((c & 0xFF00FFu) * w + (d & 0xFF00FFu) * (256 - w)) >> 8) & 0xFF00FFu;
            const uint32_t g = (((c & 0x00FF00u) * w + (d & 0x00FF00u) * (256 - w)) >> 8) & 0x00FF00u;
            row[x] = 0xFF000000u | rb | g;
            return;
        }
        if (!plain) {
            sr = div255(sr * r8);
            sg = div255(sg * g8);
            sb = div255(sb * b8);
        }
        if (ia != 255) {
            sa = div255(sa * (uint32_t)ia);
            sr = div255(sr * (uint32_t)ia);
            sg = div255(sg * (uint32_t)ia);
            sb = div255(sb * (uint32_t)ia);
        }
        if (sa == 0) return;
        if (sa >= 255) {
            row[x] = 0xFF000000u | std::min(sr, 255u) << 16 | std::min(sg, 255u) << 8 | std::min(sb, 255u);
            return;
        }
        const uint32_t d = row[x], inv = 255 - sa;
        const uint32_t rr = std::min(255u, sr + div255(((d >> 16) & 255) * inv));
        const uint32_t gg = std::min(255u, sg + div255(((d >> 8) & 255) * inv));
        const uint32_t bb = std::min(255u, sb + div255((d & 255) * inv));
        row[x] = 0xFF000000u | rr << 16 | gg << 8 | bb;
    };

    // Drawn at its own size on whole pixels (all text): copy texels, no filtering. Float error
    // can leave the coordinates a hair off a texel centre, so they are snapped first.
    auto snap = [](int32_t& t) {
        int32_t f = t & 0xFFFF;
        if (f < 64) t -= f;
        else if (f > 65536 - 64) t += 65536 - f;
    };
    if (dU >= 65534 && dU <= 65538 && dV == 0) {
        dU = 65536;
        snap(U);
        snap(V);
    }
    if (dU == 65536 && dV == 0 && (U & 0xFFFF) == 0 && (V & 0xFFFF) == 0) {
        const int ty = std::clamp(V >> 16, 0, maxY);
        const uint32_t* src = px + (size_t)ty * L.w;
        const int tx0 = (U >> 16) - xs;
        for (int x = xs; x < xe; x++) {
            const int sx = tx0 + x;
            if (sx < 0 || sx > maxX) continue;
            const uint32_t c = src[sx], a = c >> 24;
            if (a == 0) continue;
            if (a == 255 && plain && ia == 255) {
                row[x] = c;
                continue;
            }
            put(x, a, div255(((c >> 16) & 255) * a), div255(((c >> 8) & 255) * a), div255((c & 255) * a));
        }
        return;
    }

    // Any pixel: bilinear, handling transparency (premultiplied weighting).
    auto general = [&](int x, int32_t U, int32_t V) {
        int x0 = U >> 16, y0 = V >> 16;
        const uint32_t wx = (uint32_t)(U >> 8) & 255, wy = (uint32_t)(V >> 8) & 255;
        int x1 = x0 + 1, y1 = y0 + 1;
        x0 = x0 < 0 ? 0 : x0 > maxX ? maxX : x0;
        y0 = y0 < 0 ? 0 : y0 > maxY ? maxY : y0;
        x1 = x1 < 0 ? 0 : x1 > maxX ? maxX : x1;
        y1 = y1 < 0 ? 0 : y1 > maxY ? maxY : y1;
        // Premultiplied source colour (0..255 each) and alpha (0..255).
        uint32_t sa, sr, sg, sb;
        const uint32_t c00 = px[(size_t)y0 * L.w + x0];
        if (wx == 0 && wy == 0) {
            sa = c00 >> 24;
            if (sa == 0) return;
            sr = div255(((c00 >> 16) & 255) * sa);
            sg = div255(((c00 >> 8) & 255) * sa);
            sb = div255((c00 & 255) * sa);
        } else {
            const uint32_t c10 = px[(size_t)y0 * L.w + x1], c01 = px[(size_t)y1 * L.w + x0],
                           c11 = px[(size_t)y1 * L.w + x1];
            const uint32_t w00 = (256 - wx) * (256 - wy), w10 = wx * (256 - wy), w01 = (256 - wx) * wy,
                           w11 = wx * wy;
            if ((c00 & c10 & c01 & c11) >> 24 == 255) {
                sa = 255;
                sr = (((c00 >> 16) & 255) * w00 + ((c10 >> 16) & 255) * w10 + ((c01 >> 16) & 255) * w01 +
                      ((c11 >> 16) & 255) * w11) >> 16;
                sg = (((c00 >> 8) & 255) * w00 + ((c10 >> 8) & 255) * w10 + ((c01 >> 8) & 255) * w01 +
                      ((c11 >> 8) & 255) * w11) >> 16;
                sb = ((c00 & 255) * w00 + (c10 & 255) * w10 + (c01 & 255) * w01 + (c11 & 255) * w11) >> 16;
            } else {
                const uint32_t a00 = c00 >> 24, a10 = c10 >> 24, a01 = c01 >> 24, a11 = c11 >> 24;
                sa = (a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11) >> 16;
                if (sa == 0) return;
                auto pm = [&](int sh) {
                    uint64_t s = (uint64_t)((c00 >> sh) & 255) * a00 * w00 + (uint64_t)((c10 >> sh) & 255) * a10 * w10 +
                                 (uint64_t)((c01 >> sh) & 255) * a01 * w01 + (uint64_t)((c11 >> sh) & 255) * a11 * w11;
                    return div255((uint32_t)(s >> 16));
                };
                sr = pm(16);
                sg = pm(8);
                sb = pm(0);
            }
        }
        put(x, sa, sr, sg, sb);
    };

    if (dV == 0) {
        // Not rotated (every cover): both source rows and the vertical weight are fixed for the
        // whole span, and opaque texels are blended with the packed trick (red and blue together
        // in one word, green on its own), two multiplies per blend step.
        int y0 = V >> 16, y1 = y0 + 1;
        const uint32_t wy = (uint32_t)(V >> 8) & 255;
        y0 = y0 < 0 ? 0 : y0 > maxY ? maxY : y0;
        y1 = y1 < 0 ? 0 : y1 > maxY ? maxY : y1;
        const uint32_t* r0 = px + (size_t)y0 * L.w;
        const uint32_t* r1 = px + (size_t)y1 * L.w;
        auto lerp2 = [](uint32_t a, uint32_t b, uint32_t w) {
            const uint32_t rb = (((a & 0xFF00FFu) * (256 - w) + (b & 0xFF00FFu) * w) >> 8) & 0xFF00FFu;
            const uint32_t g = (((a & 0x00FF00u) * (256 - w) + (b & 0x00FF00u) * w) >> 8) & 0x00FF00u;
            return rb | g;
        };
        for (int x = xs; x < xe; x++, U += dU) {
            int x0 = U >> 16, x1 = x0 + 1;
            const uint32_t wx = (uint32_t)(U >> 8) & 255;
            x0 = x0 < 0 ? 0 : x0 > maxX ? maxX : x0;
            x1 = x1 < 0 ? 0 : x1 > maxX ? maxX : x1;
            const uint32_t c00 = r0[x0], c10 = r0[x1], c01 = r1[x0], c11 = r1[x1];
            if ((c00 & c10 & c01 & c11) >> 24 != 255) {
                general(x, U, V);
                continue;
            }
            const uint32_t c = lerp2(lerp2(c00, c10, wx), lerp2(c01, c11, wx), wy);
            if (plain && ia == 255) row[x] = 0xFF000000u | c;
            else put(x, 255, (c >> 16) & 255, (c >> 8) & 255, c & 255);
        }
        return;
    }
    for (int x = xs; x < xe; x++, U += dU, V += dV) general(x, U, V);
}

// A few helper threads that fill the rows of one big shape together with the calling thread.
// Rows are handed out in small chunks from a shared counter, so uneven rows balance out.
class RowPool {
public:
    RowPool() {
        for (int i = 0; i < kHelpers; i++) threads_.emplace_back([this] { loop(); });
    }
    ~RowPool() {
        {
            std::lock_guard<std::mutex> g(m_);
            stop_ = true;
            gen_++;
        }
        wake_.notify_all();
        for (auto& t : threads_) t.join();
    }
    void run(int ys, int ye, const std::function<void(int, int)>& fn) {
        {
            std::lock_guard<std::mutex> g(m_);
            fn_ = &fn;
            next_.store(ys);
            end_ = ye;
            busy_ = kHelpers;
            gen_++;
        }
        wake_.notify_all();
        work();
        std::unique_lock<std::mutex> g(m_);
        done_.wait(g, [this] { return busy_ == 0; });
    }

private:
    static constexpr int kHelpers = 3, kChunk = 8;
    void work() {
        for (;;) {
            int y = next_.fetch_add(kChunk);
            if (y >= end_) return;
            (*fn_)(y, std::min(end_, y + kChunk));
        }
    }
    void loop() {
        unsigned seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> g(m_);
                wake_.wait(g, [&] { return gen_ != seen; });
                seen = gen_;
                if (stop_) return;
            }
            work();
            std::lock_guard<std::mutex> g(m_);
            if (--busy_ == 0) done_.notify_all();
        }
    }
    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable wake_, done_;
    const std::function<void(int, int)>* fn_ = nullptr;
    std::atomic<int> next_{0};
    int end_ = 0, busy_ = 0;
    unsigned gen_ = 0;
    bool stop_ = false;
};

static RowPool& rowPool() {
    static RowPool pool;
    return pool;
}

void Gfx::raster(const V& A, const V& B, const V& C, const Tex* tex, int lvl) {
    if (A.a < 0.5f && B.a < 0.5f && C.a < 0.5f) return;  // invisible (faded out)
    const V *p0 = &A, *p1 = &B, *p2 = &C;
    if (p0->y > p1->y) std::swap(p0, p1);
    if (p1->y > p2->y) std::swap(p1, p2);
    if (p0->y > p1->y) std::swap(p0, p1);
    const float y0 = p0->y, y1 = p1->y, y2 = p2->y;
    if (!(y2 > y0)) return;
    // A row belongs to the triangle when its centre lies in [y0, y2): adjacent triangles never
    // overlap, so blended shapes made of several triangles have no double-blended seams.
    int ys = std::max(cy0_, (int)std::ceil(y0 - 0.5f));
    int ye = std::min(cy1_, (int)std::ceil(y2 - 0.5f));
    if (ys >= ye) return;
    float denom = (p1->x - p0->x) * (p2->y - p0->y) - (p2->x - p0->x) * (p1->y - p0->y);
    if (std::fabs(denom) < 1e-6f) return;
    float inv = 1.0f / denom;

    const float f0[6] = {p0->r, p0->g, p0->b, p0->a, p0->u, p0->v};
    const float f1[6] = {p1->r, p1->g, p1->b, p1->a, p1->u, p1->v};
    const float f2[6] = {p2->r, p2->g, p2->b, p2->a, p2->u, p2->v};
    float gx[6], gy[6];
    for (int k = 0; k < 6; k++) {
        gx[k] = ((f1[k] - f0[k]) * (p2->y - p0->y) - (f2[k] - f0[k]) * (p1->y - p0->y)) * inv;
        gy[k] = ((f2[k] - f0[k]) * (p1->x - p0->x) - (f1[k] - f0[k]) * (p2->x - p0->x)) * inv;
    }
    // Single colour with only the alpha varying (glows, shadows, anti-aliased edges): only the
    // alpha needs interpolating per pixel.
    const bool rgbFlat = !tex && f0[0] == f1[0] && f0[0] == f2[0] && f0[1] == f1[1] && f0[1] == f2[1] &&
                         f0[2] == f1[2] && f0[2] == f2[2];
    const bool flat = rgbFlat && f0[3] == f1[3] && f0[3] == f2[3];
    const Tex::Level* L = nullptr;
    if (tex && !tex->lv.empty()) L = &tex->lv[std::min<size_t>((size_t)lvl, tex->lv.size() - 1)];
    // An image with one tint (covers, icons, text): sampled in fixed point with integer maths.
    const bool texFlat = L && L->w > 0 && L->h > 0 && f0[0] == f1[0] && f0[0] == f2[0] && f0[1] == f1[1] &&
                         f0[1] == f2[1] && f0[2] == f1[2] && f0[2] == f2[2] && f0[3] == f1[3] && f0[3] == f2[3];

    auto rows = [&](int ya, int yb) {
    for (int y = ya; y < yb; y++) {
        const float sy = y + 0.5f;
        float xa = p0->x + (sy - y0) * (p2->x - p0->x) / (y2 - y0);
        float xb;
        if (sy < y1) {
            float d = y1 - y0;
            xb = d > 0 ? p0->x + (sy - y0) * (p1->x - p0->x) / d : p1->x;
        } else {
            float d = y2 - y1;
            xb = d > 0 ? p1->x + (sy - y1) * (p2->x - p1->x) / d : p1->x;
        }
        const float xl = std::min(xa, xb), xr = std::max(xa, xb);
        const int xs = std::max(cx0_, (int)std::ceil(xl - 0.5f));
        const int xe = std::min(cx1_, (int)std::ceil(xr - 0.5f));
        if (xs >= xe) continue;
        uint32_t* row = target_ + (size_t)y * tw_;

        if (flat) {
            int ia = std::clamp((int)(f0[3] + 0.5f), 0, 255);
            if (ia == 0) continue;
            uint32_t sr = (uint32_t)std::clamp((int)(f0[0] + 0.5f), 0, 255);
            uint32_t sg = (uint32_t)std::clamp((int)(f0[1] + 0.5f), 0, 255);
            uint32_t sb = (uint32_t)std::clamp((int)(f0[2] + 0.5f), 0, 255);
            if (ia == 255) {
                std::fill(row + xs, row + xe, 0xFF000000u | sr << 16 | sg << 8 | sb);
            } else {
                uint32_t ra = sr * ia, ga = sg * ia, ba = sb * ia, inva = 255 - ia;
                for (int x = xs; x < xe; x++) {
                    uint32_t d = row[x];
                    uint32_t r = div255(ra + ((d >> 16) & 255) * inva);
                    uint32_t g = div255(ga + ((d >> 8) & 255) * inva);
                    uint32_t b = div255(ba + (d & 255) * inva);
                    row[x] = 0xFF000000u | r << 16 | g << 8 | b;
                }
            }
            continue;
        }

        const float dxs = (xs + 0.5f) - p0->x, dys = sy - p0->y;
        if (texFlat) {
            texSpan(*L, row, xs, xe, f0[4] + gx[4] * dxs + gy[4] * dys, f0[5] + gx[5] * dxs + gy[5] * dys,
                    gx[4], gx[5], f0[0], f0[1], f0[2], f0[3]);
            continue;
        }
        if (rgbFlat) {
            const uint32_t sr = (uint32_t)std::clamp((int)(f0[0] + 0.5f), 0, 255);
            const uint32_t sg = (uint32_t)std::clamp((int)(f0[1] + 0.5f), 0, 255);
            const uint32_t sb = (uint32_t)std::clamp((int)(f0[2] + 0.5f), 0, 255);
            // alpha in 16.16 fixed point
            int32_t al = (int32_t)((f0[3] + gx[3] * dxs + gy[3] * dys) * 65536.0f + 32768.0f);
            const int32_t step = (int32_t)(gx[3] * 65536.0f);
            for (int x = xs; x < xe; x++, al += step) {
                int ia = al >> 16;
                if (ia <= 0) continue;
                if (ia >= 255) {
                    row[x] = 0xFF000000u | sr << 16 | sg << 8 | sb;
                    continue;
                }
                uint32_t d = row[x], inva = 255 - (uint32_t)ia;
                uint32_t rr = div255(sr * ia + ((d >> 16) & 255) * inva);
                uint32_t gg = div255(sg * ia + ((d >> 8) & 255) * inva);
                uint32_t bb = div255(sb * ia + (d & 255) * inva);
                row[x] = 0xFF000000u | rr << 16 | gg << 8 | bb;
            }
            continue;
        }
        float a[6];
        for (int k = 0; k < 6; k++) a[k] = f0[k] + gx[k] * dxs + gy[k] * dys;
        for (int x = xs; x < xe; x++) {
            float r = a[0], g = a[1], b = a[2], al = a[3];
            if (L) {
                uint32_t t = sampleBilinear(*L, a[4], a[5]);
                const float k = 1.0f / 255.0f;
                r *= ((t >> 16) & 255) * k;
                g *= ((t >> 8) & 255) * k;
                b *= (t & 255) * k;
                al *= (t >> 24) * k;
            }
            int ia = (int)(al + 0.5f);
            if (ia > 0) {
                if (ia > 255) ia = 255;
                uint32_t sr = (uint32_t)std::clamp((int)(r + 0.5f), 0, 255);
                uint32_t sg = (uint32_t)std::clamp((int)(g + 0.5f), 0, 255);
                uint32_t sb = (uint32_t)std::clamp((int)(b + 0.5f), 0, 255);
                if (ia == 255) {
                    row[x] = 0xFF000000u | sr << 16 | sg << 8 | sb;
                } else {
                    uint32_t d = row[x], inva = 255 - ia;
                    uint32_t rr = div255(sr * ia + ((d >> 16) & 255) * inva);
                    uint32_t gg = div255(sg * ia + ((d >> 8) & 255) * inva);
                    uint32_t bb = div255(sb * ia + (d & 255) * inva);
                    row[x] = 0xFF000000u | rr << 16 | gg << 8 | bb;
                }
            }
            for (int k = 0; k < 6; k++) a[k] += gx[k];
        }
    }
    };
    // Big shapes (full-screen fades, the zoomed launch cover) are split across cores by rows;
    // rows never overlap, so the threads never touch the same pixels.
    if (std::fabs(denom) * 0.5f > 16000.0f && ye - ys >= 32) rowPool().run(ys, ye, rows);
    else rows(ys, ye);
}

void Gfx::drawTris(const std::vector<V>& v, const std::vector<int>& idx, const Tex* tex, int lvl) {
    for (size_t i = 0; i + 2 < idx.size(); i += 3) raster(v[idx[i]], v[idx[i + 1]], v[idx[i + 2]], tex, lvl);
}

// ---------------------------------------------------------------- shapes

void rrectPath(std::vector<Pt>& pts, float x, float y, float w, float h, float rad, int seg) {
    rad = std::max(0.0f, std::min(rad, std::min(w, h) * 0.5f));
    struct Corner {
        float cx, cy, a0;
    } cs[4] = {{x + rad, y + rad, kPi},
               {x + w - rad, y + rad, 1.5f * kPi},
               {x + w - rad, y + h - rad, 0.0f},
               {x + rad, y + h - rad, 0.5f * kPi}};
    for (auto& c : cs)
        for (int i = 0; i <= seg; i++) {
            float a = c.a0 + (0.5f * kPi) * i / seg;
            pts.push_back({c.cx + rad * cosF(a), c.cy + rad * sinF(a)});
        }
}

// Fills a convex clockwise outline. Optional vertical gradient (top to bottom), optional texture
// stretched over the box (x, y, w, h), and a one-pixel anti-aliasing fringe.
void Gfx::fillPath(const std::vector<Pt>& path, float x, float y, float w, float h, Col top, Col bot,
                   const Tex* tex, int lvl) {
    if (path.size() < 3) return;
    auto mk = [&](float px, float py) {
        float t = h > 0 ? std::clamp((py - y) / h, 0.0f, 1.0f) : 0.0f;
        Col c = lerp(top, bot, t);
        return V{px, py, (float)c.r, (float)c.g, (float)c.b, c.a * alpha,
                 w > 0 ? (px - x) / w : 0.0f, t};
    };
    std::vector<V> v;
    int n = (int)path.size();
    v.reserve(n * 2 + 1);
    v.push_back(mk(x + w * 0.5f, y + h * 0.5f));
    for (auto& p : path) v.push_back(mk(p.x, p.y));
    std::vector<int> idx;
    idx.reserve(n * 9);
    for (int i = 0; i < n; i++) {
        idx.push_back(0);
        idx.push_back(1 + i);
        idx.push_back(1 + (i + 1) % n);
    }
    if (edge > 0.0f) {
        for (int i = 0; i < n; i++) {
            Pt a = path[(i + n - 1) % n], b = path[(i + 1) % n];
            float dx = b.x - a.x, dy = b.y - a.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-4f) len = 1.0f;
            V o = mk(path[i].x + dy / len * edge, path[i].y - dx / len * edge);
            o.a = 0;
            o.u = std::clamp(o.u, 0.0f, 1.0f);
            v.push_back(o);
        }
        for (int i = 0; i < n; i++) {
            int a = 1 + i, b = 1 + (i + 1) % n, ao = 1 + n + i, bo = 1 + n + (i + 1) % n;
            idx.insert(idx.end(), {a, ao, b, b, ao, bo});
        }
    }
    drawTris(v, idx, tex, lvl);
}

void Gfx::rect(float x, float y, float w, float h, Col top, Col bottom) {
    std::vector<Pt> p = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    float old = edge;
    edge = 0;  // axis-aligned rectangles are already crisp
    fillPath(p, x, y, w, h, top, bottom, nullptr, 0);
    edge = old;
}

void Gfx::rrect(float x, float y, float w, float h, float rad, Col top, Col bottom) {
    std::vector<Pt> p;
    rrectPath(p, x, y, w, h, rad, rad > 40 ? 14 : 10);
    fillPath(p, x, y, w, h, top, bottom, nullptr, 0);
}

void Gfx::rrectTex(const Tex* t, float x, float y, float w, float h, float rad, Col tint) {
    if (!t || t->lv.empty()) return;
    std::vector<Pt> p;
    rrectPath(p, x, y, w, h, rad, rad > 40 ? 14 : 10);
    fillPath(p, x, y, w, h, tint, tint, t, levelFor(t, w, (float)t->w()));
}

// Band of width `thick` just inside a closed clockwise outline, with soft anti-aliased
// fringes on both its outer and inner edge.
void Gfx::strokePath(const std::vector<Pt>& p, float thick, Col c) {
    int n = (int)p.size();
    if (n < 3) return;
    std::vector<V> v;
    v.reserve(n * 4);
    float f = edge;
    for (int i = 0; i < n; i++) {
        Pt a = p[(i + n - 1) % n], b = p[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f) len = 1.0f;
        float nx = dy / len, ny = -dx / len;  // outward for a clockwise path in y-down space
        float ca = c.a * alpha;
        v.push_back({p[i].x + nx * f, p[i].y + ny * f, (float)c.r, (float)c.g, (float)c.b, 0, 0, 0});
        v.push_back({p[i].x, p[i].y, (float)c.r, (float)c.g, (float)c.b, ca, 0, 0});
        v.push_back({p[i].x - nx * thick, p[i].y - ny * thick, (float)c.r, (float)c.g, (float)c.b, ca, 0, 0});
        v.push_back({p[i].x - nx * (thick + f), p[i].y - ny * (thick + f), (float)c.r, (float)c.g, (float)c.b, 0, 0, 0});
    }
    std::vector<int> idx;
    idx.reserve(n * 18);
    for (int i = 0; i < n; i++) {
        int a = i * 4, b = ((i + 1) % n) * 4;
        for (int k = 0; k < 3; k++)
            idx.insert(idx.end(), {a + k, a + k + 1, b + k, b + k, a + k + 1, b + k + 1});
    }
    drawTris(v, idx, nullptr, 0);
}

void Gfx::rrectStroke(float x, float y, float w, float h, float rad, float thick, Col c) {
    std::vector<Pt> p;
    rrectPath(p, x, y, w, h, rad, rad > 40 ? 14 : 10);
    strokePath(p, thick, c);
}

// A soft drop shadow: a ring around the shape that fades from dark to transparent. The shape's
// interior is left untouched, since the shadow is always drawn under an opaque shape.
void Gfx::shadow(float x, float y, float w, float h, float rad, float spread, Col c) {
    std::vector<Pt> p;
    rrectPath(p, x, y, w, h, rad, rad > 40 ? 14 : 10);
    int n = (int)p.size();
    static const float off[4] = {0.0f, 0.33f, 0.66f, 1.0f};
    static const float fall[4] = {0.70f, 0.30f, 0.09f, 0.0f};
    std::vector<V> v;
    v.reserve(n * 4);
    for (int i = 0; i < n; i++) {
        Pt a = p[(i + n - 1) % n], b = p[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f) len = 1.0f;
        float nx = dy / len, ny = -dx / len;
        for (int k = 0; k < 4; k++)
            v.push_back({p[i].x + nx * spread * off[k] - nx * 1.5f, p[i].y + ny * spread * off[k] - ny * 1.5f,
                         (float)c.r, (float)c.g, (float)c.b, c.a * alpha * fall[k], 0, 0});
    }
    std::vector<int> idx;
    for (int i = 0; i < n; i++) {
        int a = i * 4, b = ((i + 1) % n) * 4;
        for (int k = 0; k < 3; k++)
            idx.insert(idx.end(), {a + k, a + k + 1, b + k, b + k, a + k + 1, b + k + 1});
    }
    drawTris(v, idx, nullptr, 0);
}

void Gfx::radial(float cx, float cy, float radius, Col center) {
    const int n = 64;
    std::vector<V> v;
    v.push_back({cx, cy, (float)center.r, (float)center.g, (float)center.b, center.a * alpha, 0, 0});
    for (int i = 0; i < n; i++) {
        float a = 2 * kPi * i / n;
        v.push_back({cx + radius * cosF(a), cy + radius * sinF(a), (float)center.r,
                     (float)center.g, (float)center.b, 0, 0, 0});
    }
    std::vector<int> idx;
    for (int i = 0; i < n; i++) idx.insert(idx.end(), {0, 1 + i, 1 + (i + 1) % n});
    drawTris(v, idx, nullptr, 0);
}

void Gfx::circle(float cx, float cy, float rad, Col c) {
    const int n = rad > 60 ? 96 : rad > 12 ? 48 : 24;
    std::vector<Pt> p;
    for (int i = 0; i < n; i++) {
        float a = 2 * kPi * i / n;
        p.push_back({cx + rad * cosF(a), cy + rad * sinF(a)});
    }
    fillPath(p, cx - rad, cy - rad, rad * 2, rad * 2, c, c, nullptr, 0);
}

void Gfx::ring(float cx, float cy, float rad, float thick, Col c) {
    int n = rad > 60 ? 96 : 48;
    std::vector<Pt> p;
    for (int i = 0; i < n; i++) {
        float a = 2 * kPi * i / n;
        p.push_back({cx + rad * cosF(a), cy + rad * sinF(a)});
    }
    strokePath(p, thick, c);
}

void Gfx::line(float x1, float y1, float x2, float y2, float thick, Col c) {
    float dx = x2 - x1, dy = y2 - y1;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float nx = -dy / len * thick * 0.5f, ny = dx / len * thick * 0.5f;
    std::vector<V> v;
    Pt pts[4] = {{x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x2 - nx, y2 - ny}, {x2 + nx, y2 + ny}};
    for (auto& p : pts) v.push_back({p.x, p.y, (float)c.r, (float)c.g, (float)c.b, c.a * alpha, 0, 0});
    drawTris(v, {0, 1, 2, 0, 2, 3}, nullptr, 0);
}

void Gfx::triangle(float x1, float y1, float x2, float y2, float x3, float y3, Col c) {
    // fillPath expects a clockwise outline (in y-down space), so flip if needed.
    float area = (x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1);
    std::vector<Pt> p = {{x1, y1}, {x2, y2}, {x3, y3}};
    if (area < 0) std::swap(p[1], p[2]);
    float minx = std::min({x1, x2, x3}), maxx = std::max({x1, x2, x3});
    float miny = std::min({y1, y2, y3}), maxy = std::max({y1, y2, y3});
    fillPath(p, minx, miny, maxx - minx, maxy - miny, c, c, nullptr, 0);
}

void Gfx::image(const Tex* t, float x, float y, float w, float h, Col tint) {
    if (!t || t->lv.empty()) return;
    imageSrc(t, {0, 0, t->w(), t->h()}, x, y, w, h, tint);
}

void Gfx::imageSrc(const Tex* t, Rect s, float x, float y, float w, float h, Col tint) {
    if (!t || t->lv.empty()) return;
    float tw = (float)t->w(), th = (float)t->h();
    float u0 = s.x / tw, v0 = s.y / th, u1 = (s.x + s.w) / tw, v1 = (s.y + s.h) / th;
    float a = tint.a * alpha;
    std::vector<V> v = {{x, y, (float)tint.r, (float)tint.g, (float)tint.b, a, u0, v0},
                        {x + w, y, (float)tint.r, (float)tint.g, (float)tint.b, a, u1, v0},
                        {x + w, y + h, (float)tint.r, (float)tint.g, (float)tint.b, a, u1, v1},
                        {x, y + h, (float)tint.r, (float)tint.g, (float)tint.b, a, u0, v1}};
    drawTris(v, {0, 1, 2, 0, 2, 3}, t, levelFor(t, w, (float)s.w));
}

// Dotted "stitching" that follows a rounded rectangle at even arc-length spacing.
void Gfx::stitch(float x, float y, float w, float h, float rad, float spacing, float dot, Col c) {
    std::vector<Pt> p;
    rrectPath(p, x, y, w, h, rad, 16);
    int n = (int)p.size();
    float carry = 0.0f;
    for (int i = 0; i < n; i++) {
        Pt a = p[i], b = p[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = std::sqrt(dx * dx + dy * dy);
        float pos = carry;
        while (pos < len) {
            float t = len > 0 ? pos / len : 0;
            circle(a.x + dx * t, a.y + dy * t, dot, c);
            pos += spacing;
        }
        carry = pos - len;
    }
}

// ---------------------------------------------------------------- text

static std::vector<uint32_t> decodeUtf8(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (size_t k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

// Renders a string to a white image whose alpha channel is the glyph coverage.
Tex& Gfx::textTex(const std::string& s, Font f, int size) {
    std::string key = std::to_string((int)f) + ":" + std::to_string(size) + ":" + s;
    auto it = textCache_.find(key);
    if (it != textCache_.end()) return it->second;
    if (textCache_.size() > 1500) textCache_.clear();
    Tex& out = textCache_[key];
    FontFace* F = faces_[(int)f].get();
    if (!F || !F->ok || s.empty()) return out;

    static uint8_t lut[256];
    static bool lutInit = false;
    if (!lutInit) {  // a touch of gamma keeps light text from looking thin on dark backgrounds
        for (int i = 0; i < 256; i++) lut[i] = (uint8_t)(255.0f * std::pow(i / 255.0f, 0.82f) + 0.5f);
        lutInit = true;
    }

    float sc = stbtt_ScaleForMappingEmToPixels(&F->info, (float)size);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&F->info, &asc, &desc, &gap);
    int ascPx = (int)std::ceil(asc * sc), descPx = (int)std::floor(desc * sc);
    int h = ascPx - descPx;

    std::vector<uint32_t> cps = decodeUtf8(s);
    std::vector<float> pos(cps.size());
    float x = 0;
    for (size_t i = 0; i < cps.size(); i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&F->info, (int)cps[i], &adv, &lsb);
        pos[i] = x;
        x += adv * sc;
        if (i + 1 < cps.size()) x += stbtt_GetCodepointKernAdvance(&F->info, (int)cps[i], (int)cps[i + 1]) * sc;
    }
    int w = (int)std::ceil(x) + 1;
    std::vector<uint8_t> cov((size_t)w * h, 0);
    for (size_t i = 0; i < cps.size(); i++) {
        float bx = std::floor(pos[i]);
        float fx = pos[i] - bx;
        int ix0, iy0, ix1, iy1;
        stbtt_GetCodepointBitmapBoxSubpixel(&F->info, (int)cps[i], sc, sc, fx, 0, &ix0, &iy0, &ix1, &iy1);
        int gw = ix1 - ix0, gh = iy1 - iy0;
        if (gw <= 0 || gh <= 0) continue;
        std::vector<uint8_t> g((size_t)gw * gh);
        stbtt_MakeCodepointBitmapSubpixel(&F->info, g.data(), gw, gh, gw, sc, sc, fx, 0, (int)cps[i]);
        for (int gy = 0; gy < gh; gy++)
            for (int gx2 = 0; gx2 < gw; gx2++) {
                int dx = (int)bx + ix0 + gx2, dy = ascPx + iy0 + gy;
                if (dx < 0 || dx >= w || dy < 0 || dy >= h) continue;
                uint8_t& d = cov[(size_t)dy * w + dx];
                int b = g[(size_t)gy * gw + gx2];
                d = (uint8_t)(d + b - d * b / 255);
            }
    }
    Tex::Level L;
    L.w = w;
    L.h = h;
    L.px.resize((size_t)w * h);
    for (size_t i = 0; i < L.px.size(); i++) L.px[i] = (uint32_t)lut[cov[i]] << 24 | 0x00FFFFFFu;
    out.lv.push_back(std::move(L));
    return out;
}

// Text images carry one spare pixel on the right so glyph edges are never clipped; layout uses
// the width without it.
float Gfx::textWidth(const std::string& s, Font f, int size) {
    int w = textTex(s, f, size).w();
    return (float)std::max(0, w - 1);
}

void Gfx::text(const std::string& s, Font f, int size, float x, float y, Col c, Align a) {
    Tex& e = textTex(s, f, size);
    if (e.lv.empty()) return;
    float layoutW = (float)(e.w() - 1);
    if (a == Align::Center) x -= layoutW * 0.5f;
    if (a == Align::Right) x -= layoutW;
    image(&e, std::round(x), std::round(y), (float)e.w(), (float)e.h(), c);
}

static std::vector<std::string> utf8Chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

float Gfx::textWidthSpaced(const std::string& s, Font f, int size, float tracking) {
    float w = 0;
    auto cs = utf8Chars(s);
    for (size_t i = 0; i < cs.size(); i++) {
        w += textWidth(cs[i], f, size);
        if (i + 1 < cs.size()) w += tracking;
    }
    return w;
}

void Gfx::textSpaced(const std::string& s, Font f, int size, float x, float y, Col c, float tracking,
                     Align a) {
    float w = textWidthSpaced(s, f, size, tracking);
    if (a == Align::Center) x -= w * 0.5f;
    if (a == Align::Right) x -= w;
    for (auto& ch : utf8Chars(s)) {
        text(ch, f, size, x, y, c);
        x += textWidth(ch, f, size) + tracking;
    }
}

int Gfx::textWrapped(const std::string& s, Font f, int size, float x, float y, float maxW, Col c,
                     float lineH, int maxLines) {
    std::vector<std::string> lines;
    std::string cur, word;
    auto flushWord = [&]() {
        if (word.empty()) return;
        std::string trial = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && textWidth(trial, f, size) > maxW) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = trial;
        }
        word.clear();
    };
    for (char ch : s) {
        if (ch == ' ')
            flushWord();
        else
            word += ch;
    }
    flushWord();
    if (!cur.empty()) lines.push_back(cur);
    int drawn = std::min((int)lines.size(), maxLines);
    for (int i = 0; i < drawn; i++) {
        std::string ln = lines[i];
        if (i == drawn - 1 && (int)lines.size() > drawn) {
            while (!ln.empty() && textWidth(ln + "...", f, size) > maxW) ln.pop_back();
            ln += "...";
        }
        text(ln, f, size, x, y + i * lineH, c);
    }
    return drawn;
}

// ---------------------------------------------------------------- images

// Which mip level to sample when drawing `t` at dstW pixels wide (largest one not more than
// twice the needed size).
static int pickLevel(const Tex& t, float dstW) {
    if (t.lv.size() < 2 || t.w() <= 0) return 0;
    float scale = dstW / (float)t.w();
    int l = 0;
    while (scale <= 0.5f && l + 1 < (int)t.lv.size()) {
        l++;
        scale *= 2.0f;
    }
    return l;
}

int Gfx::levelFor(const Tex* t, float dstW, float srcW) const {
    if (!t || srcW <= 0) return 0;
    return pickLevel(*t, dstW * (float)t->w() / srcW);
}

// Decodes an image file and builds its half-size copies. Touches no shared state, so it can
// run on the prefetch thread.
static std::unique_ptr<Tex> decodeTex(const std::string& path) {
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data) {
        std::fprintf(stderr, "Texture load failed: %s\n", path.c_str());
        return nullptr;
    }
    auto t = std::make_unique<Tex>();
    Tex::Level L;
    L.w = w;
    L.h = h;
    L.px.resize((size_t)w * h);
    for (size_t i = 0; i < L.px.size(); i++) {
        const unsigned char* p = data + i * 4;
        L.px[i] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    stbi_image_free(data);
    t->lv.push_back(std::move(L));
    // Half-size copies (2x2 box filter, alpha-weighted) for smooth downscaling.
    while (t->lv.back().w > 16 && t->lv.back().h > 16) {
        const Tex::Level& s = t->lv.back();
        Tex::Level d;
        d.w = s.w / 2;
        d.h = s.h / 2;
        d.px.resize((size_t)d.w * d.h);
        for (int y = 0; y < d.h; y++)
            for (int x = 0; x < d.w; x++) {
                uint32_t c[4] = {s.px[(size_t)(2 * y) * s.w + 2 * x], s.px[(size_t)(2 * y) * s.w + 2 * x + 1],
                                 s.px[(size_t)(2 * y + 1) * s.w + 2 * x], s.px[(size_t)(2 * y + 1) * s.w + 2 * x + 1]};
                uint32_t as = 0, rs = 0, gs = 0, bs = 0;
                for (uint32_t q : c) {
                    uint32_t a = q >> 24;
                    as += a;
                    rs += ((q >> 16) & 255) * a;
                    gs += ((q >> 8) & 255) * a;
                    bs += (q & 255) * a;
                }
                uint32_t a = as / 4;
                if (as == 0) { d.px[(size_t)y * d.w + x] = 0; continue; }
                d.px[(size_t)y * d.w + x] = a << 24 | (rs / as) << 16 | (gs / as) << 8 | (bs / as);
            }
        t->lv.push_back(std::move(d));
    }
    return t;
}

// A copy of `t` resampled to exactly w x h, with the same filtering as drawing it live.
static std::unique_ptr<Tex> makeScaled(const Tex& t, int w, int h) {
    const Tex::Level& src = t.lv[std::min<size_t>((size_t)pickLevel(t, (float)w), t.lv.size() - 1)];
    auto out = std::make_unique<Tex>();
    Tex::Level L;
    L.w = w;
    L.h = h;
    L.px.resize((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            L.px[(size_t)y * w + x] = sampleBilinear(src, (x + 0.5f) / w, (y + 0.5f) / h);
    out->lv.push_back(std::move(L));
    return out;
}

const Tex* Gfx::texture(const std::string& path) {
    auto it = texCache_.find(path);
    if (it != texCache_.end()) return it->second.get();
    if (prefetching_.count(path)) return nullptr;  // still being decoded in the background
    auto t = decodeTex(path);
    const Tex* out = t.get();
    texCache_[path] = std::move(t);
    return out;
}

const Tex* Gfx::scaled(const Tex* t, int w, int h) {
    if (!t || t->lv.empty() || w <= 0 || h <= 0) return t;
    auto key = std::make_tuple(t, w, h);
    auto it = scaledCache_.find(key);
    if (it != scaledCache_.end()) return it->second.get();
    if (scaledCache_.size() > 512) scaledCache_.clear();
    auto out = makeScaled(*t, w, h);
    const Tex* p = out.get();
    scaledCache_[key] = std::move(out);
    return p;
}

void Gfx::prefetch(const std::vector<std::string>& paths, const std::vector<int>& sizes) {
    std::vector<std::string> todo;
    for (auto& p : paths)
        if (!p.empty() && !texCache_.count(p) && !prefetching_.count(p)) {
            todo.push_back(p);
            prefetching_.insert(p);
        }
    if (todo.empty()) return;
    if (worker_.joinable()) worker_.join();  // one batch at a time
    worker_ = std::thread([this, todo, sizes] {
        for (auto& p : todo) {
            Prefetched r;
            r.path = p;
            r.tex = decodeTex(p);
            if (r.tex)
                for (int s : sizes) r.scaled.emplace_back(s, makeScaled(*r.tex, s, s));
            std::lock_guard<std::mutex> g(readyLock_);
            ready_.push_back(std::move(r));
        }
    });
}

void Gfx::pump() {
    std::vector<Prefetched> got;
    {
        std::lock_guard<std::mutex> g(readyLock_);
        got.swap(ready_);
    }
    for (auto& r : got) {
        prefetching_.erase(r.path);
        const Tex* base = r.tex.get();
        for (auto& [s, t] : r.scaled) scaledCache_[std::make_tuple(base, s, s)] = std::move(t);
        texCache_[r.path] = std::move(r.tex);
    }
}

}  // namespace nexus
