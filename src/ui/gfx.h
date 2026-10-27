#pragma once
// Nexus 2D drawing layer: a small software renderer that draws into a plain 32-bit pixel buffer
// (0xAARRGGBB). It has no dependency on SDL or any GPU API, so the exact same code and pixels are
// produced by the PC preview and by the console build.
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace nexus {

struct Col {
    uint8_t r, g, b, a;
};
constexpr Col rgba(int r, int g, int b, int a = 255) {
    return {(uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a};
}
constexpr Col hex(unsigned v, int a = 255) {
    return rgba((v >> 16) & 255, (v >> 8) & 255, v & 255, a);
}
Col lerp(Col a, Col b, float t);
Col withA(Col c, float mul);  // multiplies alpha

enum Font { Regular = 0, Medium = 1, SemiBold = 2, Bold = 3 };
enum class Align { Left, Center, Right };

struct Rect {
    int x, y, w, h;
};
struct Pt {
    float x, y;
};

// An image with a chain of half-size copies used when it is drawn smaller than its real size.
struct Tex {
    struct Level {
        int w = 0, h = 0;
        std::vector<uint32_t> px;  // 0xAARRGGBB, straight (non-premultiplied) alpha
    };
    std::vector<Level> lv;
    int w() const { return lv.empty() ? 0 : lv[0].w; }
    int h() const { return lv.empty() ? 0 : lv[0].h; }
};

class Gfx {
public:
    Gfx();
    ~Gfx();

    bool init(const std::string& assetsDir);  // loads the fonts
    void setTarget(uint32_t* pixels, int w, int h);

    float alpha = 1.0f;  // multiplies the alpha of everything drawn
    float edge = 1.0f;   // width in pixels of the soft anti-aliasing fringe on shape edges

    void clear(Col c);
    void rect(float x, float y, float w, float h, Col top, Col bottom);
    void rrect(float x, float y, float w, float h, float rad, Col top, Col bottom);
    void rrectTex(const Tex* t, float x, float y, float w, float h, float rad, Col tint);
    void rrectStroke(float x, float y, float w, float h, float rad, float thick, Col c);
    void shadow(float x, float y, float w, float h, float rad, float spread, Col c);
    void radial(float cx, float cy, float radius, Col center);
    void circle(float cx, float cy, float rad, Col c);
    void ring(float cx, float cy, float rad, float thick, Col c);
    void line(float x1, float y1, float x2, float y2, float thick, Col c);
    void triangle(float x1, float y1, float x2, float y2, float x3, float y3, Col c);
    void image(const Tex* t, float x, float y, float w, float h, Col tint);
    void imageSrc(const Tex* t, Rect src, float x, float y, float w, float h, Col tint);
    void stitch(float x, float y, float w, float h, float rad, float spacing, float dot, Col c);

    void text(const std::string& s, Font f, int size, float x, float y, Col c,
              Align a = Align::Left);
    void textSpaced(const std::string& s, Font f, int size, float x, float y, Col c,
                    float tracking, Align a = Align::Left);
    float textWidth(const std::string& s, Font f, int size);
    float textWidthSpaced(const std::string& s, Font f, int size, float tracking);
    // Draws up to maxLines wrapped lines; returns number of lines drawn.
    int textWrapped(const std::string& s, Font f, int size, float x, float y, float maxW, Col c,
                    float lineH, int maxLines);

    const Tex* texture(const std::string& path);  // cached; nullptr if missing
    // A copy of `t` resampled to exactly w x h pixels (cached). Drawn at that size on whole
    // pixels it is copied rather than filtered, which is much faster.
    const Tex* scaled(const Tex* t, int w, int h);
    // Decodes images, plus square copies at the given sizes, on a background thread so they
    // are ready before they are first drawn (decoding a cover takes tens of milliseconds).
    // Until then texture() returns nullptr for them. Results are taken in by pump().
    void prefetch(const std::vector<std::string>& paths, const std::vector<int>& sizes);
    void pump();  // once per frame
    void clip(float x, float y, float w, float h);
    void unclip();

    // Pre-rendered layers: draw something expensive once, then copy it each frame.
    bool blitCache(const std::string& name);
    void beginCache(const std::string& name, int w, int h);
    void endCache();

private:
    struct V;
    struct FontFace;

    Tex& textTex(const std::string& s, Font f, int size);
    void fillPath(const std::vector<Pt>& path, float x, float y, float w, float h, Col top,
                  Col bot, const Tex* tex, int lvl);
    void strokePath(const std::vector<Pt>& p, float thick, Col c);
    void drawTris(const std::vector<V>& v, const std::vector<int>& idx, const Tex* tex, int lvl);
    void raster(const V& a, const V& b, const V& c, const Tex* tex, int lvl);
    int levelFor(const Tex* t, float dstW, float srcW) const;

    uint32_t* target_ = nullptr;
    int tw_ = 0, th_ = 0;
    int cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;

    struct CacheSave {
        uint32_t* target;
        int tw, th;
    } saved_{};
    std::unordered_map<std::string, std::vector<uint32_t>> layers_;
    std::unordered_map<std::string, int> layerW_;

    std::unique_ptr<FontFace> faces_[4];
    std::unordered_map<std::string, Tex> textCache_;
    std::unordered_map<std::string, std::unique_ptr<Tex>> texCache_;
    std::map<std::tuple<const Tex*, int, int>, std::unique_ptr<Tex>> scaledCache_;

    struct Prefetched {
        std::string path;
        std::unique_ptr<Tex> tex;
        std::vector<std::pair<int, std::unique_ptr<Tex>>> scaled;
    };
    std::set<std::string> prefetching_;  // main thread only
    std::mutex readyLock_;
    std::vector<Prefetched> ready_;      // guarded by readyLock_
    std::thread worker_;
};

void rrectPath(std::vector<Pt>& pts, float x, float y, float w, float h, float rad, int seg = 10);

}  // namespace nexus
