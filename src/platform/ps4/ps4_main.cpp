// Nexus - PS4 package entry point (OpenOrbis toolchain).
//
// Start-up is deliberately staged and visible, so that if anything goes wrong on the console the
// last thing on screen (and the last line in /data/nexus_log.txt) says which step it was:
//   1 video output up      2 fonts loaded      3 app created
//   4 first interface frame (the dashboard appears)
//   5 dashboard shown for two seconds, THEN the controller library is started
#include <orbis/libkernel.h>

#include <cstdio>
#include <vector>

#include "../../ui/app.h"
#include "../../ui/gfx.h"
#include "ps4_platform.h"

using namespace nexus;

static std::vector<uint32_t> g_frame;

// Draws `lit` of `total` progress squares (lit ones in the accent colour) and shows the frame.
static void progress(Ps4Display& disp, int lit, int total = 5) {
    g_frame.assign((size_t)Ps4Display::kW * Ps4Display::kH, 0xFF0B1220u);
    const int size = 70, gap = 26;
    const int x0 = (Ps4Display::kW - (total * size + (total - 1) * gap)) / 2, y0 = Ps4Display::kH / 2 - size / 2;
    for (int i = 0; i < total; i++) {
        uint32_t c = i < lit ? 0xFF5AB8FFu : 0xFF26303Fu;
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                g_frame[(size_t)(y0 + y) * Ps4Display::kW + x0 + i * (size + gap) + x] = c;
    }
    disp.present(g_frame.data());
}

// Paints the whole screen one colour and stops (used when fonts are missing).
static void fatalColour(Ps4Display& disp, uint32_t argb) {
    g_frame.assign((size_t)Ps4Display::kW * Ps4Display::kH, argb);
    for (;;) disp.present(g_frame.data());
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    logLine("---- Nexus 0.2.1 console test starting");

    Ps4Display display;
    if (!display.init()) for (;;) sceKernelSleep(1);  // nothing can be shown without video
    progress(display, 1);
    logLine("step 1: video output up, first frame shown");

    const std::string assets = "/app0/assets";
    Gfx gfx;
    if (!gfx.init(assets)) {
        logLine("step 2: FAILED, could not load fonts from %s/fonts", assets.c_str());
        fatalColour(display, 0xFFB03030u);  // red screen = fonts missing from the package
    }
    progress(display, 2);
    logLine("step 2: fonts loaded");

    Ps4Platform platform;  // does not touch the controller yet
    AppOptions opt;
    opt.showStats = true;  // test build: show the frame time
    App app(gfx, platform, opt, assets);
    progress(display, 3);
    logLine("step 3: app created");

    g_frame.assign((size_t)Ps4Display::kW * Ps4Display::kH, 0xFF000000u);
    uint64_t last = sceKernelGetProcessTime();
    float smoothMs = 0.0f;
    for (int n = 0;; n++) {
        uint64_t now = sceKernelGetProcessTime();
        float dt = (float)(now - last) / 1000000.0f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;

        InputState in;
        platform.poll(in);  // returns "no input" until the controller is started
        app.update(dt, in);

        uint64_t t0 = sceKernelGetProcessTime();
        gfx.setTarget(g_frame.data(), Ps4Display::kW, Ps4Display::kH);
        app.draw();
        float ms = (float)(sceKernelGetProcessTime() - t0) / 1000.0f;
        smoothMs = smoothMs == 0.0f ? ms : smoothMs * 0.9f + ms * 0.1f;
        app.setFrameMs(smoothMs);

        display.present(g_frame.data());
        if (n == 0) logLine("step 4: first interface frame presented (draw %.1f ms)", ms);
        if (n == 119) logLine("step 5: interface has been on screen for 2 seconds, starting the controller");
        if (n == 120) {
            platform.initPad();
            logLine("step 5: controller started (handle ok: %d)", platform.padReady() ? 1 : 0);
        }
        if (n == 300) logLine("loop: 300 frames, average draw %.1f ms", smoothMs);
    }
    return 0;
}
