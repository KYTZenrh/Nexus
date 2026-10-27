#include "ps4_platform.h"

#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../sample_data.h"

namespace nexus {

void logLine(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::printf("%s\n", buf);
    if (FILE* f = std::fopen("/data/nexus_log.txt", "a")) {
        std::fprintf(f, "%s\n", buf);
        std::fflush(f);
        fsync(fileno(f));  // force it to disk now, so the line survives a crash right after
        std::fclose(f);
    }
}

// ---------------------------------------------------------------- display

bool Ps4Display::init() {
    video_ = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (video_ < 0) {
        logLine("video: sceVideoOutOpen failed (%d)", video_);
        return false;
    }
    OrbisKernelEqueue queue;
    if (sceKernelCreateEqueue(&queue, "nexus flip queue") < 0) {
        logLine("video: could not create the flip queue");
        return false;
    }
    flipQueue_ = queue;
    sceVideoOutAddFlipEvent(queue, video_, 0);

    // Two frame buffers in direct memory, mapped for the CPU (same recipe as the OpenOrbis samples).
    // Same sizes as the official OpenOrbis graphics sample (a known-good configuration): one big
    // 192 MB direct-memory block, of which only the first two frames are used.
    const size_t frameBytes = (size_t)kW * kH * 4;
    const size_t alignment = 0x200000;
    const size_t total = 0xC000000;
    off_t directOff = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, alignment, 3, &directOff);
    if (rc < 0) {
        logLine("video: sceKernelAllocateDirectMemory failed (%d)", rc);
        return false;
    }
    void* mem = nullptr;
    rc = sceKernelMapDirectMemory(&mem, total, 0x33, 0, directOff, alignment);
    if (rc < 0 || !mem) {
        logLine("video: sceKernelMapDirectMemory failed (%d)", rc);
        return false;
    }
    buffers_[0] = (char*)mem;
    buffers_[1] = (char*)mem + frameBytes;

    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, kW, kH, kW);
    rc = sceVideoOutRegisterBuffers(video_, 0, (void**)buffers_, 2, &attr);
    if (rc != 0) {
        logLine("video: sceVideoOutRegisterBuffers failed (%d)", rc);
        return false;
    }
    sceVideoOutSetFlipRate(video_, 0);
    ready_ = true;
    logLine("video: ready (%dx%d, 2 buffers)", kW, kH);
    return true;
}

void Ps4Display::present(const uint32_t* pixels) {
    if (!ready_) return;
    // The renderer writes alpha 0xFF; the console's frame buffer format wants 0x80 there.
    uint32_t* dst = (uint32_t*)buffers_[active_];
    const size_t n = (size_t)kW * kH;
    for (size_t i = 0; i < n; i++) dst[i] = (pixels[i] & 0x00FFFFFFu) | 0x80000000u;

    sceVideoOutSubmitFlip(video_, active_, ORBIS_VIDEO_OUT_FLIP_VSYNC, frameId_);
    // Wait until the flip we just submitted has happened.
    for (;;) {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(video_, &st);
        if (st.flipArg == frameId_) break;
        OrbisKernelEvent evt;
        int count = 0;
        if (sceKernelWaitEqueue((OrbisKernelEqueue)flipQueue_, &evt, 1, &count, 0) != 0) break;
    }
    active_ = (active_ + 1) % 2;
    frameId_++;
}

// ---------------------------------------------------------------- platform

Ps4Platform::Ps4Platform() { lastUs_ = sceKernelGetProcessTime(); }

// Starts the controller library. Kept separate from the constructor so start-up can show the
// interface first and only then touch the pad libraries (each step is logged).
bool Ps4Platform::initPad() {
    logLine("pad: scePadInit...");
    if (scePadInit() != 0) {
        logLine("pad: scePadInit failed");
        return false;
    }
    logLine("pad: sceUserServiceInitialize...");
    OrbisUserServiceInitializeParams params;
    params.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&params);
    logLine("pad: sceUserServiceGetInitialUser...");
    if (sceUserServiceGetInitialUser(&userId_) != 0) {
        logLine("pad: could not get the initial user");
        return false;
    }
    logLine("pad: scePadOpen (user %d)...", userId_);
    pad_ = scePadOpen(userId_, 0, 0, nullptr);
    logLine("pad: handle %d", pad_);
    lastUs_ = sceKernelGetProcessTime();
    return pad_ >= 0;
}

Ps4Platform::~Ps4Platform() {
    if (pad_ >= 0) {
        OrbisPadVibeParam off = {0, 0};
        scePadSetVibration(pad_, &off);
        scePadClose(pad_);
    }
}

// Fires once when a direction is first pressed, then again at a steady rate while it is held.
bool Ps4Platform::repeatFire(Repeat& r, bool down, float dt) {
    if (!down) {
        r.held = false;
        return false;
    }
    if (!r.held) {
        r.held = true;
        r.next = 0.38f;
        return true;
    }
    r.next -= dt;
    if (r.next <= 0.0f) {
        r.next = 0.14f;
        return true;
    }
    return false;
}

void Ps4Platform::poll(InputState& in) {
    in = InputState{};
    uint64_t now = sceKernelGetProcessTime();
    float dt = lastUs_ ? (float)(now - lastUs_) / 1000000.0f : 0.016f;
    lastUs_ = now;

    // Stop a finished rumble pulse.
    if (rumbleStopUs_ && now >= rumbleStopUs_ && pad_ >= 0) {
        OrbisPadVibeParam off = {0, 0};
        scePadSetVibration(pad_, &off);
        rumbleStopUs_ = 0;
    }
    if (pad_ < 0) return;

    OrbisPadData d;
    std::memset(&d, 0, sizeof d);
    if (scePadReadState(pad_, &d) != 0) return;
    connected_ = d.connected != 0;
    in.padConnected = connected_;
    in.padName = connected_ ? "Controller" : "";
    if (!connected_) {
        prevButtons_ = 0;
        return;
    }

    const uint32_t b = d.buttons, pressed = b & ~prevButtons_;
    prevButtons_ = b;
    if (pressed & ORBIS_PAD_BUTTON_CROSS) in.confirm = true;
    if (pressed & ORBIS_PAD_BUTTON_CIRCLE) in.back = true;
    if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) in.triangle = true;
    if (pressed & ORBIS_PAD_BUTTON_L1) in.l1 = true;
    if (pressed & ORBIS_PAD_BUTTON_R1) in.r1 = true;
    if (pressed & ORBIS_PAD_BUTTON_OPTIONS) in.options = true;

    // The d-pad and the left stick both act as direction keys, with auto-repeat.
    const bool up = (b & ORBIS_PAD_BUTTON_UP) || d.leftStick.y < 64;
    const bool down = (b & ORBIS_PAD_BUTTON_DOWN) || d.leftStick.y > 192;
    const bool left = (b & ORBIS_PAD_BUTTON_LEFT) || d.leftStick.x < 64;
    const bool right = (b & ORBIS_PAD_BUTTON_RIGHT) || d.leftStick.x > 192;
    in.up = repeatFire(rep_[0], up, dt);
    in.down = repeatFire(rep_[1], down, dt);
    in.left = repeatFire(rep_[2], left, dt);
    in.right = repeatFire(rep_[3], right, dt);
}

void Ps4Platform::rumble(float s, int ms) {
    if (pad_ < 0 || !connected_ || s <= 0.0f) return;
    if (s > 1.0f) s = 1.0f;
    OrbisPadVibeParam p;
    p.lgMotor = (uint8_t)(s * 255.0f);
    p.smMotor = (uint8_t)(s * 0.8f * 255.0f);
    scePadSetVibration(pad_, &p);
    rumbleStopUs_ = sceKernelGetProcessTime() + (uint64_t)ms * 1000ull;
}

std::vector<Game> Ps4Platform::listGames(std::string&) { return sampleGames(); }

LaunchResult Ps4Platform::launch(const Game& g) {
    LaunchResult r;
    r.ok = false;
    r.message = "Launching is not implemented in this test build yet (" + g.title + ").";
    return r;
}

TrophyList Ps4Platform::trophiesFor(const Game& g) { return sampleTrophies(g); }

std::vector<SysRow> Ps4Platform::systemInfo(int gamesFound) {
    std::vector<SysRow> rows;
    rows.push_back({"Platform", "PS4 app (test build)", Source::Live});
    rows.push_back({"Nexus build", "0.2.1 console test", Source::Live});
    rows.push_back({"Controller", connected_ ? "Connected" : "None detected", Source::Live});
    rows.push_back({"Games found", std::to_string(gamesFound) + " (sample entries)", Source::Sample});
    rows.push_back({"Console firmware", "Not read yet", Source::Unavailable});
    rows.push_back({"Console model", "Not read yet", Source::Unavailable});
    rows.push_back({"CPU temperature", "Not read yet", Source::Unavailable});
    rows.push_back({"Storage", "Not read yet", Source::Unavailable});
    return rows;
}

}  // namespace nexus

