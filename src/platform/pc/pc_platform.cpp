#include "pc_platform.h"

#include <cmath>

#include "../../core/music_synth.h"
#include "../../core/sfx_synth.h"
#include "../sample_data.h"

namespace nexus {

PcPlatform::PcPlatform(bool preview, const std::string& exeDir, bool audio, const std::string& iconDir)
    : wantAudio_(audio), preview_(preview), exeDir_(exeDir), iconDir_(iconDir) {
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) {
            openPad(i);
            break;
        }
    lastTicks_ = SDL_GetTicks();
    if (wantAudio_) openAudio();
}

PcPlatform::~PcPlatform() {
    if (musicThread_.joinable()) musicThread_.join();
    if (audioDev_) SDL_CloseAudioDevice(audioDev_);
    closePad();
}

// ---------------------------------------------------------------- audio

void PcPlatform::openAudio() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_Log("Audio unavailable: %s", SDL_GetError());
        return;
    }
    mixer_.loadSfx();
    SDL_AudioSpec want{}, have{};
    want.freq = kSfxRate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &PcPlatform::audioCallback;
    want.userdata = this;
    audioDev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);  // 0 = SDL converts to our format
    if (!audioDev_) {
        SDL_Log("Audio device failed: %s", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(audioDev_, 0);
    musicThread_ = std::thread([this] { mixer_.setMusic(synthMusic()); });
}

void PcPlatform::audioCallback(void* self, Uint8* stream, int len) {
    static_cast<PcPlatform*>(self)->mixer_.mix(reinterpret_cast<float*>(stream), len / (int)(sizeof(float) * 2));
}

void PcPlatform::playSfx(Sfx s, float volume, float pitch) {
    if (audioDev_) mixer_.play(s, volume, pitch);
}

void PcPlatform::openPad(int index) {
    if (pad_) return;
    pad_ = SDL_GameControllerOpen(index);
    if (pad_) padId_ = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad_));
}

void PcPlatform::closePad() {
    if (pad_) SDL_GameControllerClose(pad_);
    pad_ = nullptr;
    padId_ = -1;
}

void PcPlatform::poll(InputState& in) {
    in = InputState{};
    Uint32 now = SDL_GetTicks();
    float dt = (now - lastTicks_) / 1000.0f;
    lastTicks_ = now;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT: in.quit = true; break;
            case SDL_CONTROLLERDEVICEADDED: openPad(e.cdevice.which); break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (e.cdevice.which == padId_) closePad();
                break;
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym) {
                    case SDLK_LEFT: case SDLK_a: in.left = true; break;
                    case SDLK_RIGHT: case SDLK_d: in.right = true; break;
                    case SDLK_UP: case SDLK_w: in.up = true; break;
                    case SDLK_DOWN: case SDLK_s: in.down = true; break;
                    case SDLK_RETURN: case SDLK_SPACE: case SDLK_z:
                        if (!e.key.repeat) in.confirm = true;
                        break;
                    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_x:
                        if (!e.key.repeat) in.back = true;
                        break;
                    case SDLK_q: case SDLK_PAGEUP: if (!e.key.repeat) in.l1 = true; break;
                    case SDLK_e: case SDLK_PAGEDOWN: if (!e.key.repeat) in.r1 = true; break;
                    case SDLK_TAB: if (!e.key.repeat) in.options = true; break;
                    case SDLK_t: if (!e.key.repeat) in.triangle = true; break;
                    case SDLK_F11:
                        if (!e.key.repeat) {
                            SDL_Window* w = SDL_GetWindowFromID(e.key.windowID);
                            if (w)
                                SDL_SetWindowFullscreen(
                                    w, (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN_DESKTOP)
                                           ? 0
                                           : SDL_WINDOW_FULLSCREEN_DESKTOP);
                        }
                        break;
                    default: break;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                switch (e.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: in.left = true; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: in.right = true; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_UP: in.up = true; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: in.down = true; break;
                    case SDL_CONTROLLER_BUTTON_A: in.confirm = true; break;  // Cross
                    case SDL_CONTROLLER_BUTTON_B: in.back = true; break;     // Circle
                    case SDL_CONTROLLER_BUTTON_Y: in.triangle = true; break; // Triangle
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: in.l1 = true; break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: in.r1 = true; break;
                    case SDL_CONTROLLER_BUTTON_START: in.options = true; break;
                    default: break;
                }
                break;
            default: break;
        }
    }

    // Left stick acts as a d-pad with auto-repeat.
    if (pad_) {
        auto stick = [&](SDL_GameControllerAxis ax, Repeat& rp, bool& neg, bool& pos) {
            float v = SDL_GameControllerGetAxis(pad_, ax) / 32767.0f;
            int dir = v > 0.6f ? 1 : v < -0.6f ? -1 : 0;
            if (dir == 0) {
                rp.dir = 0;
                return;
            }
            if (dir != rp.dir) {
                rp.dir = dir;
                rp.next = 0.38f;
                (dir > 0 ? pos : neg) = true;
            } else {
                rp.next -= dt;
                if (rp.next <= 0) {
                    rp.next = 0.14f;
                    (dir > 0 ? pos : neg) = true;
                }
            }
        };
        stick(SDL_CONTROLLER_AXIS_LEFTX, stickX_, in.left, in.right);
        stick(SDL_CONTROLLER_AXIS_LEFTY, stickY_, in.up, in.down);
        in.padConnected = true;
        const char* n = SDL_GameControllerName(pad_);
        in.padName = n ? n : "Controller";
    }
}

void PcPlatform::rumble(float s, int ms) {
    if (!pad_ || s <= 0.0f) return;
    Uint16 lo = (Uint16)(std::min(1.0f, s) * 65535.0f);
    Uint16 hi = (Uint16)(std::min(1.0f, s * 0.8f) * 65535.0f);
    SDL_GameControllerRumble(pad_, lo, hi, (Uint32)ms);
}

TrophyList PcPlatform::trophiesFor(const Game& g) { return sampleTrophies(g); }

std::vector<Game> PcPlatform::listGames(std::string& emptyReason) {
    if (preview_) {
        auto games = sampleGames();
        if (!iconDir_.empty())
            for (size_t i = 0; i < games.size(); i++) games[i].iconPath = iconDir_ + "/" + std::to_string(i + 1) + ".png";
        return games;
    }
    emptyReason = "The PC build cannot read a console library. Start it in preview mode to see clearly labelled sample entries.";
    return {};
}

LaunchResult PcPlatform::launch(const Game& g) {
    LaunchResult r;
    r.ok = false;
    r.message = "Launching is not available on the PC preview (" + g.title + ").";
    return r;
}

std::vector<SysRow> PcPlatform::systemInfo(int gamesFound) {
    std::vector<SysRow> rows;
    rows.push_back({"Platform", std::string("PC preview (") + SDL_GetPlatform() + ")", Source::Live});
    rows.push_back({"Nexus version", "1.0.0 PC preview", Source::Live});
    rows.push_back({"Controller", pad_ ? (SDL_GameControllerName(pad_) ? SDL_GameControllerName(pad_) : "Controller") : "None detected", Source::Live});
    rows.push_back({"Games found", std::to_string(gamesFound) + (preview_ ? " (sample entries)" : ""), preview_ ? Source::Sample : Source::Live});
    rows.push_back({"Console firmware", "Not detected on PC", Source::Unavailable});
    rows.push_back({"Console model", "Not detected on PC", Source::Unavailable});
    rows.push_back({"CPU temperature", "Not read on PC", Source::Unavailable});
    rows.push_back({"Storage", "Not read on PC", Source::Unavailable});
    return rows;
}

}  // namespace nexus

