// Nexus - PC preview entry point.
// Flags: --preview (sample data) | --stats (frame-time readout)
//        --icons DIR    give the sample games real covers (DIR/1.png, 2.png, ...), for testing
//        --tab N --sel N --filter N --tsel N --srow N --freeze-launch F
//        --shot FILE [--frames N] [--keys a,b,c]     offscreen screenshot, for testing
//        --make-icon DIR                              export icon0.png and pic1.png
//        --dump-sfx DIR                               export every sound as WAV
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../third_party/stb/stb_image_write.h"
#include "core/music_synth.h"
#include "core/sfx_synth.h"
#include "platform/pc/pc_platform.h"
#include "ui/app.h"
#include "ui/brand.h"
#include "ui/gfx.h"

using namespace nexus;

static const int kW = 1920, kH = 1080;

static void writeWav(const std::string& path, const std::vector<float>& d, int rateHz) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t n = (uint32_t)d.size(), rate = (uint32_t)rateHz, dataBytes = n * 2, riff = 36 + dataBytes;
    uint16_t fmt = 1, ch = 1, bits = 16, align = 2;
    uint32_t br = rate * 2, sixteen = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&sixteen, 4, 1, f); std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&br, 4, 1, f); std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
    for (float v : d) {
        int16_t q = (int16_t)(std::max(-1.0f, std::min(1.0f, v)) * 32767);
        std::fwrite(&q, 2, 1, f);
    }
    std::fclose(f);
}

// Saves a 0xAARRGGBB pixel buffer as an RGBA PNG.
static bool savePng(const std::string& path, const uint32_t* px, int w, int h) {
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t p = px[i];
        rgba[i * 4 + 0] = (p >> 16) & 255;
        rgba[i * 4 + 1] = (p >> 8) & 255;
        rgba[i * 4 + 2] = p & 255;
        rgba[i * 4 + 3] = 255;
    }
    return stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}

static int dumpSounds(const std::string& dir) {
    static const char* names[] = {"move", "tab", "confirm", "back", "launch", "error", "filter",
                                  "setting", "bronze", "silver", "gold", "platinum", "locked"};
    std::filesystem::create_directories(dir);
    for (int s = 0; s < (int)Sfx::Count; s++) {
        std::vector<float> d = synthSfx((Sfx)s);
        float peak = 0;
        bool bad = false;
        for (float v : d) {
            if (!(v == v)) bad = true;
            peak = std::max(peak, std::fabs(v));
        }
        writeWav(dir + "/" + names[s] + ".wav", d, kSfxRate);
        std::printf("%-9s %5.2fs  peak %.2f%s\n", names[s], d.size() / (float)kSfxRate, peak, bad ? "  NaN!" : "");
    }
    std::vector<float> m = synthMusic();
    float peak = 0, maxStep = 0;
    for (size_t k = 0; k < m.size(); k++) {
        peak = std::max(peak, std::fabs(m[k]));
        if (k) maxStep = std::max(maxStep, std::fabs(m[k] - m[k - 1]));
    }
    float seam = std::fabs(m.front() - m.back());  // loop wrap-around jump
    writeWav(dir + "/music.wav", m, kMusicRate);
    std::printf("music     %5.1fs  peak %.2f  loop jump %.4f (largest normal step %.4f)\n",
                m.size() / (float)kMusicRate, peak, seam, maxStep);
    return 0;
}

int main(int argc, char** argv) {
    bool preview = false, stats = false;
    std::string shot, keys, makeIcon, iconDir;
    int frames = 90;
    AppOptions opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](int def) { return i + 1 < argc ? std::atoi(argv[++i]) : def; };
        if (a == "--preview") preview = true;
        else if (a == "--stats") stats = true;
        else if (a == "--tab") opt.tab = next(0);
        else if (a == "--sel") opt.sel = next(0);
        else if (a == "--filter") opt.filter = next(0);
        else if (a == "--tsel") opt.trophySel = next(0);
        else if (a == "--srow") opt.settingsRow = next(0);
        else if (a == "--frames") frames = next(90);
        else if (a == "--freeze-launch") opt.freezeLaunch = i + 1 < argc ? (float)std::atof(argv[++i]) : 0.5f;
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--keys" && i + 1 < argc) keys = argv[++i];
        else if (a == "--make-icon" && i + 1 < argc) makeIcon = argv[++i];
        else if (a == "--icons" && i + 1 < argc) iconDir = argv[++i];
        else if (a == "--dump-sfx" && i + 1 < argc) return dumpSounds(argv[++i]);
    }
    opt.showStats = stats;
    const bool offscreen = !shot.empty() || !makeIcon.empty();

    // scripted input for offscreen tests: comma list of left,right,up,down,confirm,back,l1,r1,triangle
    std::vector<std::string> script;
    for (size_t p = 0; p < keys.size();) {
        size_t q = keys.find(',', p);
        if (q == std::string::npos) q = keys.size();
        script.push_back(keys.substr(p, q - p));
        p = q + 1;
    }

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    char* base = SDL_GetBasePath();
    std::string exeDir = base ? base : "./";
    SDL_free(base);
    while (!exeDir.empty() && (exeDir.back() == '/' || exeDir.back() == '\\')) exeDir.pop_back();
    std::string assets = exeDir + "/assets";
    if (!std::filesystem::exists(assets + "/fonts")) assets = exeDir + "/../assets";

    Gfx gfx;
    if (!gfx.init(assets)) return 1;
    std::vector<uint32_t> fb((size_t)kW * kH, 0xFF000000u);

    if (!makeIcon.empty()) {  // export the app's icon and home-screen art, then exit
        std::filesystem::create_directories(makeIcon);
        struct Out { const char* file; int w, h; bool icon; };
        for (Out o : {Out{"icon0.png", 512, 512, true}, Out{"pic1.png", 1920, 1080, false}}) {
            std::vector<uint32_t> img((size_t)o.w * o.h, 0xFF000000u);
            gfx.setTarget(img.data(), o.w, o.h);
            if (o.icon) drawAppIcon(gfx, (float)o.w); else drawBackdrop(gfx, (float)o.w, (float)o.h);
            bool ok = savePng(makeIcon + "/" + o.file, img.data(), o.w, o.h);
            std::printf("wrote %s/%s (%dx%d) %s\n", makeIcon.c_str(), o.file, o.w, o.h, ok ? "ok" : "FAILED");
        }
        return 0;
    }

    PcPlatform plat(preview, exeDir, /*audio=*/!offscreen, iconDir);
    App app(gfx, plat, opt, assets);

    if (offscreen) {
        double firstMs = 0, drawMs = 0, worstMs = 0;
        InputState in;
        for (int f = 0; f < frames; f++) {
            plat.poll(in);
            InputState fake;
            size_t k = (size_t)f / 12;
            if (f % 12 == 0 && k < script.size()) {
                const std::string& t = script[k];
                fake.left = t == "left"; fake.right = t == "right"; fake.up = t == "up"; fake.down = t == "down";
                fake.confirm = t == "confirm"; fake.back = t == "back"; fake.l1 = t == "l1"; fake.r1 = t == "r1";
                fake.triangle = t == "triangle";
            }
            app.update(1.0f / 60.0f, fake);
            Uint64 t0 = SDL_GetPerformanceCounter();
            gfx.setTarget(fb.data(), kW, kH);
            app.draw();
            double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
            if (f == 0) firstMs = ms;
            else { drawMs += ms; worstMs = std::max(worstMs, ms); }        }
        bool ok = savePng(shot, fb.data(), kW, kH);
        std::printf("screenshot %s (%s)  draw: first %.1f ms, average %.1f ms, worst %.1f ms\n", shot.c_str(),
                    ok ? "ok" : "FAILED", firstMs, frames > 1 ? drawMs / (frames - 1) : 0.0, worstMs);
        SDL_Quit();
        return ok ? 0 : 1;
    }

    SDL_Window* win = SDL_CreateWindow("Nexus (PC preview)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1280, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!ren) {
        std::fprintf(stderr, "Window/renderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(ren, kW, kH);
    // The window only shows the renderer's pixel buffer; all drawing is done by Gfx.
    SDL_Texture* screen = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kW, kH);

    Uint64 last = SDL_GetPerformanceCounter();
    const double freq = (double)SDL_GetPerformanceFrequency();
    InputState in;
    for (;;) {
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = std::min((float)((now - last) / freq), 0.05f);
        last = now;
        plat.poll(in);
        if (in.quit) break;
        app.update(dt, in);

        Uint64 t0 = SDL_GetPerformanceCounter();
        gfx.setTarget(fb.data(), kW, kH);
        app.draw();
        app.setFrameMs((float)((SDL_GetPerformanceCounter() - t0) * 1000.0 / freq));

        SDL_UpdateTexture(screen, nullptr, fb.data(), kW * (int)sizeof(uint32_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, screen, nullptr, nullptr);
        SDL_RenderPresent(ren);
    }

    app.saveSettings();
    SDL_DestroyTexture(screen);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
