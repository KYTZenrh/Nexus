#pragma once
// Everything the UI needs from the machine it runs on. The UI never calls
// console or OS APIs directly; each target (pc/, ps4/) implements this.
#include <cstdint>
#include <string>
#include <vector>

namespace nexus {

struct Game {
    std::string titleId;
    std::string title;
    std::string iconPath;          // empty -> placeholder cover is drawn
    bool sample = false;           // true only for preview-mode sample data
    uint32_t colorA = 0x2a3140;    // placeholder cover gradient (0xRRGGBB)
    uint32_t colorB = 0x151922;
};

struct LaunchResult {
    bool ok = false;
    std::string message;
};

enum class Source { Live, Sample, Unavailable };

struct SysRow {
    std::string label;
    std::string value;
    Source source;
};

enum class Grade { Bronze, Silver, Gold, Platinum };

struct Trophy {
    std::string name;
    std::string how;
    Grade grade = Grade::Bronze;
    bool earned = false;
};

struct TrophyList {
    bool available = false;
    bool sample = false;
    std::string reason;            // shown when !available
    std::vector<Trophy> items;
};

// Every sound effect the UI can ask for. The sounds themselves are synthesized in
// core/sfx_synth.cpp, so there are no audio files to ship or license.
enum class Sfx {
    Move, Tab, Confirm, Back, Launch, Error, Filter, Setting,
    Bronze, Silver, Gold, Platinum, Locked,
    Count
};

struct InputState {
    bool up = false, down = false, left = false, right = false;
    bool confirm = false, back = false, l1 = false, r1 = false, options = false;
    bool triangle = false;
    bool quit = false;
    bool padConnected = false;
    std::string padName;
};

class Platform {
public:
    virtual ~Platform() = default;
    virtual const char* name() const = 0;
    virtual bool previewData() const = 0;

    virtual void poll(InputState& in) = 0;   // one call per frame, edge-triggered
    virtual void rumble(float strength01, int ms) = 0;
    virtual void playSfx(Sfx s, float volume01, float pitch) = 0;
    virtual void setMusicVolume(float volume01) = 0;  // 0 = silent; called every frame

    virtual std::vector<Game> listGames(std::string& emptyReason) = 0;
    virtual LaunchResult launch(const Game& g) = 0;
    virtual TrophyList trophiesFor(const Game& g) = 0;
    virtual std::vector<SysRow> systemInfo(int gamesFound) = 0;

    virtual std::string settingsPath() const = 0;
};

}  // namespace nexus
