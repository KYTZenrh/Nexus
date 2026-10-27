#pragma once
// Console (PS4 app, OpenOrbis toolchain) implementation of the Platform interface.
// Stage 1 "test build": video out, controller and rumble only. Audio, the real game list and
// launching are not implemented yet, and the capability list says so.
#include <cstdint>
#include <string>
#include <vector>

#include "../platform.h"

namespace nexus {

// Appends a line to /data/nexus_log.txt (best effort) so a failed first run can be diagnosed
// by fetching the file over FTP.
void logLine(const char* fmt, ...);

// 1920x1080 frame buffers in direct memory, flipped on vsync.
class Ps4Display {
public:
    bool init();
    // Copies a 1920x1080 0xAARRGGBB image to the screen and waits for the flip.
    void present(const uint32_t* pixels);

    static constexpr int kW = 1920, kH = 1080;

private:
    int video_ = -1;
    char* buffers_[2] = {nullptr, nullptr};
    int active_ = 0;
    int frameId_ = 0;
    unsigned long flipQueue_ = 0;  // OrbisKernelEqueue handle
    bool ready_ = false;
};

class Ps4Platform : public Platform {
public:
    Ps4Platform();
    ~Ps4Platform() override;

    const char* name() const override { return "PS4 TEST BUILD"; }
    bool previewData() const override { return true; }

    void poll(InputState& in) override;
    void rumble(float strength01, int ms) override;
    void playSfx(Sfx, float, float) override {}      // audio is not ported yet
    void setMusicVolume(float) override {}

    std::vector<Game> listGames(std::string& emptyReason) override;
    LaunchResult launch(const Game& g) override;
    TrophyList trophiesFor(const Game& g) override;
    std::vector<SysRow> systemInfo(int gamesFound) override;

    std::string settingsPath() const override { return "/data/nexus_settings.ini"; }

    bool initPad();
    bool padReady() const { return pad_ >= 0; }

private:
    struct Repeat {
        bool held = false;
        float next = 0.0f;
    };
    bool repeatFire(Repeat& r, bool down, float dt);

    int pad_ = -1;
    int userId_ = -1;
    uint32_t prevButtons_ = 0;
    uint64_t lastUs_ = 0;
    uint64_t rumbleStopUs_ = 0;
    bool connected_ = false;
    Repeat rep_[4];  // up, down, left, right
};

}  // namespace nexus

