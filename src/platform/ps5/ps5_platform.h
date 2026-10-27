#pragma once
// Native PS5 (ps5-payload-dev SDK) implementation of the Platform interface, built as an
// installed app by tools/build-ps5-native.sh. Reading the title database, launching other
// titles and some system information need the app to be outside the sandbox, which HyperHEN
// does for title IDs listed under [app_jailbreak] in /data/HyperHEN/config.ini.
#include <cstdint>
#include <string>
#include <vector>

#include "../../core/mixer.h"
#include "../platform.h"
#include "ps5_tilemap.h"

struct kevent;  // opaque; only ever used as a pointer here (from <sys/event.h> on the console)

namespace nexus {

void logLine(const char* fmt, ...);

// 1920x1080, double-buffered, with the PS5 GPU tile swizzle applied on every flip.
class Ps5Display {
public:
    bool init();
    void present(const uint32_t* pixels);  // pixels: 0xAARRGGBB, row-major, kW*kH

    static constexpr int kW = 1920, kH = 1080;

private:
    int handle_ = -1;
    void* vbuf_[2] = {nullptr, nullptr};
    kevent* evtQueue_ = nullptr;
    intptr_t paddr_ = 0;
    size_t memsize_ = 0;
    Ps5Tilemap tmap_;
    int frameId_ = 0;
    int active_ = 0;
    bool ready_ = false;
};

class Ps5Platform : public Platform {
public:
    Ps5Platform();
    ~Ps5Platform() override;

    const char* name() const override { return "PS5"; }
    bool previewData() const override { return false; }

    void poll(InputState& in) override;
    void rumble(float strength01, int ms) override;
    void playSfx(Sfx s, float volume01, float pitch) override;
    void setMusicVolume(float v) override { mixer_.setMusicVolume(v); }

    std::vector<Game> listGames(std::string& emptyReason) override;
    LaunchResult launch(const Game& g) override;
    TrophyList trophiesFor(const Game& g) override;
    std::vector<SysRow> systemInfo(int gamesFound) override;

    std::string settingsPath() const override { return "/data/nexus_settings.ini"; }
    // Start-up, each step logged: the signed-in user and controller, then audio.
    bool initPad();
    void initAudio();
    void requestJailbreak();
    void loadLauncher(const std::string& path);  // the payload launch() sends; see launch()
    bool padReady() const { return pad_ >= 0; }

private:
    struct Repeat {
        bool held = false;
        float next = 0.0f;
    };
    bool repeatFire(Repeat& r, bool down, float dt);
    bool trophyTotals(int counts[4]);  // bronze, silver, gold, platinum earned across all games
    static void* audioThread(void* self);

    int pad_ = -1;
    int userId_ = -1;
    uint32_t prevButtons_ = 0;
    uint64_t lastUs_ = 0;
    uint64_t rumbleStopUs_ = 0;
    bool connected_ = false;
    Repeat rep_[4];  // up, down, left, right

    Mixer mixer_;
    int audioPort_ = -1;
    std::vector<char> launcher_;
};

}  // namespace nexus
