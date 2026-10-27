// Nexus - PS5 native entry point (ps5-payload-dev SDK), installed as an app and started from
// its home screen icon. Start-up is logged to /data/nexus_log.txt step by step, so a problem
// shows which step it reached.
#include <cstdio>
#include <vector>

extern "C" uint64_t sceKernelGetProcessTime(void);

#include "../../ui/app.h"
#include "../../ui/gfx.h"
#include "ps5_platform.h"

using namespace nexus;

static std::vector<uint32_t> g_frame;

static void fatalColour(Ps5Display& disp, uint32_t argb) {
    g_frame.assign((size_t)Ps5Display::kW * Ps5Display::kH, argb);
    for (;;) disp.present(g_frame.data());
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    logLine("---- Nexus starting");

    Ps5Display display;
    if (!display.init()) {
        // Give up and exit rather than sit here forever still holding whatever it grabbed.
        logLine("video init failed, exiting (see the line above for which call failed)");
        return 1;
    }
    g_frame.assign((size_t)Ps5Display::kW * Ps5Display::kH, 0xFF06080Bu);
    display.present(g_frame.data());

#if NEXUS_PS5_APP
    const std::string assets = "/app0/assets";
#else
    const std::string assets = "/data/nexus_assets";
#endif
    Gfx gfx;
    if (!gfx.init(assets)) {
        logLine("fonts: FAILED, could not load fonts from %s/fonts", assets.c_str());
        fatalColour(display, 0xFFB03030u);
    }

    // Everything needed from /app0 is read now: it is gone once the sandbox is lifted, which
    // the platform requests when it first reads the library (inside App's constructor).
    App::preloadAssets(gfx, assets);
    Ps5Platform platform;
    platform.loadLauncher(assets + "/launcher.elf");
    platform.initPad();  // also signs in the user, whose id picks the library order
    platform.initAudio();
    App app(gfx, platform, AppOptions{}, assets);
    logLine("app ready");

    uint64_t last = sceKernelGetProcessTime();
    float smoothMs = 0.0f;
    for (int n = 0;; n++) {
        uint64_t now = sceKernelGetProcessTime();
        float dt = (float)(now - last) / 1000000.0f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;

        InputState in;
        platform.poll(in);
        app.update(dt, in);

        uint64_t t0 = sceKernelGetProcessTime();
        gfx.setTarget(g_frame.data(), Ps5Display::kW, Ps5Display::kH);
        app.draw();
        float ms = (float)(sceKernelGetProcessTime() - t0) / 1000.0f;
        smoothMs = smoothMs == 0.0f ? ms : smoothMs * 0.9f + ms * 0.1f;

        display.present(g_frame.data());
        if (n == 300) logLine("loop: 300 frames, average draw %.1f ms", smoothMs);    }
    return 0;
}
