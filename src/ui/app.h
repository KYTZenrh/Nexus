#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "../platform/platform.h"
#include "gfx.h"

namespace nexus {

struct AppOptions {
    int tab = 0;
    int sel = 0;
    int filter = 0;
    int trophySel = 0;
    float freezeLaunch = -1.0f;  // >=0 renders the launch transition frozen at that point
    int settingsRow = 0;
    bool showStats = false;      // draw a frame-time readout (test builds)
};

class App {
public:
    App(Gfx& g, Platform& p, const AppOptions& o, const std::string& assetsDir);
    // Loads every image the UI uses from the assets folder now rather than on first use. On the
    // console the assets folder (/app0) disappears once HyperHEN lifts the sandbox.
    static void preloadAssets(Gfx& g, const std::string& assetsDir);
    void update(float dt, const InputState& in);
    void draw();
    void saveSettings();
    void setFrameMs(float ms) { frameMs_ = ms; }

private:
    enum Tab { Home = 0, Trophies = 1, System = 2, Settings = 3, TabCount = 4 };
    enum class Launch { None, Out, Back, Gone };

    // layout / drawing
    void drawBackground();
    void drawHeader(float a);
    void drawFooter(float a);
    void drawHome();
    void drawTrophies();
    void drawSystem();
    void drawSettings();
    void drawCover(const Game& g, float x, float y, float s, float rad, float brightness, bool atRest = false);
    void drawCushion(float x, float y, float s, Grade g, bool earned);
    void drawBadge(const std::string& text, float x, float y, Col c, Align a = Align::Left);
    float drawHint(float x, float y, int kind, const std::string& label);
    void drawEmpty(const std::string& title, const std::string& body);

    // logic
    void setTab(int t);
    void moveSel(int d);
    void reloadTrophies();
    void prefetchCovers();
    struct FitTitle {
        std::string text, line2;  // line2 empty: the name fits on one line
        int size;
    };
    const FitTitle& fitTitle(const Game& g);
    std::vector<int> filteredTrophies() const;
    void startLaunch();
    void toast(const std::string& s);
    void tick(float strength);
    void buzz(float scale, int ms);
    void snd(Sfx s, float pitch = 1.0f);
    void trophyCue();
    float vibStrength() const;

    Gfx& gfx_;
    Platform& plat_;
    std::string assets_;

    // data
    std::vector<Game> games_;
    std::string emptyReason_;
    TrophyList trophies_;
    std::vector<SysRow> sysRows_;
    InputState lastIn_;

    // state
    int tab_ = 0;
    int sel_ = 0;
    float selAnim_ = 0;
    int filter_ = 0;       // 0 all, 1 bronze, 2 silver, 3 gold, 4 platinum
    int trophySel_ = 0;
    float trophyScroll_ = 0;
    int vib_ = 2;          // index into vibration levels
    int sfx_ = 2;          // index into sound effect volume levels
    bool musicOn_ = true;  // background music switch (on by default)
    float musicAnim_ = 1.0f;
    int settingsRow_ = 0;

    float time_ = 0;
    float libRetry_ = 1.0f;
    int textWarm_ = 0;  // games whose title texts are already rendered
    std::unordered_map<std::string, FitTitle> fitCache_;  // by title ID
    float dt_ = 1.0f / 60.0f;
    bool showStats_ = false;
    float frameMs_ = 0;
    float contentT_ = 0;
    float underlineX_ = 0, underlineW_ = 0;
    bool underlineInit_ = false;
    bool bakedGlow_ = false;   // this frame's backdrop already has the Home cover glow
    uint32_t glowKey_ = 0xFFFFFFFFu;

    Launch launch_ = Launch::None;
    float launchT_ = 0;
    float goneT_ = 0;
    float forceLaunchT_ = -1;
    Game launching_;

    std::string toast_;
    float toastT_ = 0;
};

}  // namespace nexus
