#pragma once
#include <SDL.h>

#include <thread>

#include "../../core/mixer.h"
#include "../platform.h"

namespace nexus {

// Desktop stand-in used to design and test the UI. It cannot launch games or read
// console data; sample entries exist only when started with --preview.
class PcPlatform : public Platform {
public:
    PcPlatform(bool preview, const std::string& exeDir, bool audio, const std::string& iconDir = "");
    ~PcPlatform() override;

    const char* name() const override { return "PC PREVIEW"; }
    bool previewData() const override { return preview_; }

    void poll(InputState& in) override;
    void rumble(float strength01, int ms) override;
    void playSfx(Sfx s, float volume01, float pitch) override;
    void setMusicVolume(float volume01) override { mixer_.setMusicVolume(volume01); }

    std::vector<Game> listGames(std::string& emptyReason) override;
    LaunchResult launch(const Game& g) override;
    TrophyList trophiesFor(const Game& g) override;
    std::vector<SysRow> systemInfo(int gamesFound) override;

    std::string settingsPath() const override { return exeDir_ + "/nexus_settings.ini"; }

private:
    void openPad(int index);
    void closePad();
    void openAudio();
    static void audioCallback(void* self, Uint8* stream, int len);

    Mixer mixer_;
    // Background music is generated on a worker thread at startup (about a second).
    std::thread musicThread_;
    SDL_AudioDeviceID audioDev_ = 0;
    bool wantAudio_ = false;

    bool preview_;
    std::string exeDir_, iconDir_;
    SDL_GameController* pad_ = nullptr;
    SDL_JoystickID padId_ = -1;

    struct Repeat {
        int dir = 0;      // -1/0/+1
        float next = 0;   // seconds until next repeat
    } stickX_, stickY_;
    Uint32 lastTicks_ = 0;
};

}  // namespace nexus
