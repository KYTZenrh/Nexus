#include "app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>

namespace nexus {

static const float W = 1920.0f, H = 1080.0f;

// ------------------------------------------------------------------ theme
static const Col kBg0 = hex(0x0c0f16), kBg1 = hex(0x06080b);
static const Col kPanel = hex(0x12161e), kPanel2 = hex(0x1a1f2a);
static const Col kText = hex(0xeef1f6), kSub = hex(0x9aa4b4), kDim = hex(0x5d6878);
static const Col kAccent = hex(0x5ab8ff);
static const Col kLine = rgba(255, 255, 255, 24);
static const Col kBlack = rgba(0, 0, 0, 255);

static Col gradeCol(Grade g) {
    switch (g) {
        case Grade::Bronze: return hex(0xd9955a);
        case Grade::Silver: return hex(0xc3cbd6);
        case Grade::Gold: return hex(0xe8c14f);
        default: return hex(0x8fd6ff);
    }
}
static const char* gradeName(Grade g) {
    switch (g) {
        case Grade::Bronze: return "BRONZE";
        case Grade::Silver: return "SILVER";
        case Grade::Gold: return "GOLD";
        default: return "PLATINUM";
    }
}
static const char* gradeIcon(Grade g) {
    switch (g) {
        case Grade::Bronze: return "trophies/Bronze_136.png";
        case Grade::Silver: return "trophies/Silver_136.png";
        case Grade::Gold: return "trophies/Gold_136.png";
        default: return "trophies/Platinum_154.png";
    }
}

// Visible area of each trophy PNG (the files have large transparent margins).
static const Rect kTrophyCrop[4] = {
    {91, 160, 120, 156},   // bronze
    {85, 152, 132, 164},   // silver
    {80, 142, 142, 174},   // gold
    {95, 138, 112, 178}};  // platinum

// Draws a trophy fitted (aspect-preserving, bottom-centred) into the given box.
static void drawTrophyIcon(Gfx& g, const std::string& assets, Grade gr, float x, float y, float w,
                           float h, Col tint) {
    Rect src = kTrophyCrop[(int)gr];
    float k = std::min(w / src.w, h / src.h);
    float dw = src.w * k, dh = src.h * k;
    g.imageSrc(g.texture(assets + "/" + gradeIcon(gr)), src, x + (w - dw) / 2, y + h - dh, dw, dh, tint);
}

struct VibLevel {
    const char* name;
    float strength;
};
static const int kVibCount = 6;
static const VibLevel kVib[kVibCount] = {{"Off", 0.0f},         {"Low", 0.20f},
                                         {"Low-Medium", 0.35f}, {"Medium", 0.55f},
                                         {"Medium-High", 0.72f}, {"High", 0.90f}};
static const int kVibDefault = 2;

static const int kSfxCount = 4;
static const VibLevel kSfxLevels[kSfxCount] = {
    {"Off", 0.0f}, {"Low", 0.35f}, {"Medium", 0.65f}, {"High", 1.0f}};
static const int kSfxDefault = 2;

// Background music is a simple on/off switch, on by default, at the volume that was chosen
// by ear (the quieter steps were hard to hear).
static const float kMusicVolume = 0.62f;

static Sfx gradeSfx(Grade g) {
    switch (g) {
        case Grade::Bronze: return Sfx::Bronze;
        case Grade::Silver: return Sfx::Silver;
        case Grade::Gold: return Sfx::Gold;
        default: return Sfx::Platinum;
    }
}

// Controller motors barely move below roughly a quarter of full power, so levels are mapped
// into the range the motors actually respond to.
static float motorPower(float level) { return level <= 0.0f ? 0.0f : 0.25f + 0.75f * level; }

struct CushionPal {
    Col top, bot, edge, edgeDark;
    bool icy;
};
static CushionPal cushionPal(Grade g, bool earned) {
    CushionPal p;
    p.icy = false;
    switch (g) {
        case Grade::Silver:
            p = {hex(0x6f7f98), hex(0x2c3647), hex(0xdde3ec), hex(0x7d8798), false};
            break;
        case Grade::Platinum:
            p = {hex(0x2f78c4), hex(0x0e2650), hex(0xd6efff), hex(0x6fa6cf), true};
            break;
        case Grade::Bronze:  // warm copper pillow with a bronze edge
            p = {hex(0xb8723c), hex(0x4d2713), hex(0xe0aa72), hex(0x7f4d24), false};
            break;
        default:  // gold: red pillow, gold edge
            p = {hex(0xa8283c), hex(0x5a0f1e), hex(0xdcb860), hex(0x7d5d22), false};
    }
    if (!earned) {
        Col dark = hex(0x1f232b);
        float k = g == Grade::Gold ? 0.38f : 0.5f;
        p.top = lerp(p.top, dark, k);
        p.bot = lerp(p.bot, dark, k);
        p.edge = lerp(p.edge, hex(0x3a3f4a), 0.45f);
        p.edgeDark = lerp(p.edgeDark, dark, 0.5f);
    }
    return p;
}

// std::fabs on a float resolves to the double overload with the console toolchain's headers,
// which breaks std::min/std::max with float arguments, so use a float-only version.
static inline float fabsF(float x) { return x < 0 ? -x : x; }
static float clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }
static float smooth(float x) {
    x = clamp01(x);
    return x * x * (3 - 2 * x);
}
static float approach(float cur, float target, float dt, float speed) {
    return cur + (target - cur) * (1.0f - std::exp(-speed * dt));
}

// ------------------------------------------------------------------ setup
void App::preloadAssets(Gfx& g, const std::string& assetsDir) {
    for (int i = 0; i < 4; i++) g.texture(assetsDir + "/" + gradeIcon((Grade)i));
}

App::App(Gfx& g, Platform& p, const AppOptions& o, const std::string& assetsDir)
    : gfx_(g), plat_(p), assets_(assetsDir) {
    std::ifstream f(plat_.settingsPath());
    std::string line;
    vib_ = kVibDefault;
    while (std::getline(f, line))
    {
        if (line.rfind("vib_level=", 0) == 0)
            vib_ = std::clamp(std::atoi(line.c_str() + 10), 0, kVibCount - 1);
        if (line.rfind("sfx_level=", 0) == 0)
            sfx_ = std::clamp(std::atoi(line.c_str() + 10), 0, kSfxCount - 1);
        if (line.rfind("music_on=", 0) == 0) musicOn_ = std::atoi(line.c_str() + 9) != 0;
    }

    games_ = plat_.listGames(emptyReason_);
    prefetchCovers();
    sel_ = games_.empty() ? 0 : std::clamp(o.sel, 0, (int)games_.size() - 1);
    selAnim_ = (float)sel_;
    tab_ = std::clamp(o.tab, 0, TabCount - 1);
    filter_ = std::clamp(o.filter, 0, 4);
    trophySel_ = o.trophySel;
    settingsRow_ = std::clamp(o.settingsRow, 0, 3);
    contentT_ = 1.0f;
    forceLaunchT_ = o.freezeLaunch;
    showStats_ = o.showStats;
    reloadTrophies();
    sysRows_ = plat_.systemInfo((int)games_.size());
    trophyScroll_ = 0;
}

// Home layout: where the big cover sits when nothing is moving (and its glow), the shelf cover
// size at rest, and how much the focused shelf cover grows.
static const float kCoverX = 130, kCoverY = 190, kCoverS = 560;
static const float kShelfT = 270, kShelfFocus = 1.07f;

// Every cover is decoded, with copies at the sizes it is shown at when still, in the
// background as soon as the library is known, so browsing never waits on an image.
void App::prefetchCovers() {
    std::vector<std::string> paths;
    for (auto& g : games_) paths.push_back(g.iconPath);
    gfx_.prefetch(paths, {(int)std::round(kCoverS), (int)std::round(kShelfT),
                          (int)std::round(kShelfT * kShelfFocus)});
    textWarm_ = 0;
}

// The game name under ON DECK has to fit between the shelf's left edge and the trophies panel
// divider: one line at the largest size that fits, else two lines at a smaller size, and only
// if even that is too long is the second line cut short with "...".
const App::FitTitle& App::fitTitle(const Game& g) {
    auto it = fitCache_.find(g.titleId);
    if (it != fitCache_.end()) return it->second;
    const float maxW = 480;
    for (int sz : {76, 66, 58, 50})
        if (gfx_.textWidth(g.title, SemiBold, sz) <= maxW) return fitCache_[g.titleId] = FitTitle{g.title, "", sz};

    const int sz = 46;
    FitTitle f{"", "", sz};
    // First line: as many whole words as fit; the second line gets the rest.
    std::vector<std::string> words;
    for (size_t p = 0; p < g.title.size();) {
        size_t q = g.title.find(' ', p);
        if (q == std::string::npos) q = g.title.size();
        if (q > p) words.push_back(g.title.substr(p, q - p));
        p = q + 1;
    }
    size_t w = 0;
    while (w < words.size()) {
        std::string trial = f.text.empty() ? words[w] : f.text + " " + words[w];
        if (!f.text.empty() && gfx_.textWidth(trial, SemiBold, sz) > maxW) break;
        f.text = trial;
        w++;
    }
    for (; w < words.size(); w++) f.line2 += (f.line2.empty() ? "" : " ") + words[w];
    auto shorten = [&](std::string& s) {  // drop whole UTF-8 characters until "s..." fits
        if (gfx_.textWidth(s, SemiBold, sz) <= maxW) return;
        while (!s.empty()) {
            size_t cut = s.size() - 1;
            while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
            s.resize(cut);
            while (!s.empty() && s.back() == ' ') s.pop_back();
            if (gfx_.textWidth(s + "...", SemiBold, sz) <= maxW) break;
        }
        s += "...";
    };
    shorten(f.text);  // a single word wider than the space
    shorten(f.line2);
    return fitCache_[g.titleId] = f;
}

void App::saveSettings() {
    std::ofstream f(plat_.settingsPath());
    if (f) f << "vib_level=" << vib_ << "\nsfx_level=" << sfx_ << "\nmusic_on=" << (musicOn_ ? 1 : 0) << "\n";
}

float App::vibStrength() const { return kVib[vib_].strength; }

void App::snd(Sfx s, float pitch) {
    float v = kSfxLevels[sfx_].strength;
    if (v > 0.0f) plat_.playSfx(s, v, pitch);
}

// All vibration goes through here so every cue respects the chosen level.
void App::buzz(float scale, int ms) {
    float level = vibStrength();
    if (level <= 0.0f) return;
    plat_.rumble(std::min(1.0f, motorPower(level) * scale), ms);
}

void App::tick(float) { buzz(0.85f, 60); }

void App::toast(const std::string& s) {
    toast_ = s;
    toastT_ = 3.2f;
}

void App::reloadTrophies() {
    trophies_ = games_.empty() ? TrophyList{} : plat_.trophiesFor(games_[sel_]);
    if (games_.empty()) trophies_.reason = "No game selected.";
    trophySel_ = 0;
}

std::vector<int> App::filteredTrophies() const {
    std::vector<int> v;
    for (int i = 0; i < (int)trophies_.items.size(); i++) {
        if (filter_ == 0 || (int)trophies_.items[i].grade == filter_ - 1) v.push_back(i);
    }
    return v;
}

void App::setTab(int t) {
    t = std::clamp(t, 0, TabCount - 1);
    if (t == tab_) return;
    tab_ = t;
    contentT_ = 0;
    sysRows_ = plat_.systemInfo((int)games_.size());
    tick(0.5f);
    snd(Sfx::Tab);
}

void App::moveSel(int d) {
    int n = (int)games_.size();
    if (n == 0) return;
    int ns = std::clamp(sel_ + d, 0, n - 1);
    if (ns == sel_) return;
    sel_ = ns;
    reloadTrophies();
    tick(0.6f);
    snd(Sfx::Move);
}

void App::startLaunch() {
    if (games_.empty() || launch_ != Launch::None) return;
    launching_ = games_[sel_];
    launch_ = Launch::Out;
    launchT_ = 0;
    buzz(1.0f, 180);
    snd(Sfx::Launch);
}

// Plays the chime for the trophy under the cursor (a dull thud when it is still locked).
void App::trophyCue() {
    auto idx = filteredTrophies();
    if (trophySel_ < 0 || trophySel_ >= (int)idx.size()) return;
    const Trophy& t = trophies_.items[idx[trophySel_]];
    snd(t.earned ? gradeSfx(t.grade) : Sfx::Locked);
}

// ------------------------------------------------------------------ update
void App::update(float dt, const InputState& in) {
    lastIn_ = in;
    dt_ = dt;
    time_ += dt;
    contentT_ = std::min(1.0f, contentT_ + dt * 4.5f);
    selAnim_ = approach(selAnim_, (float)sel_, dt, 13.0f);
    toastT_ = std::max(0.0f, toastT_ - dt);

    float rowH = 164.0f;
    int count = (int)filteredTrophies().size();
    float maxScroll = std::max(0.0f, count * rowH - 620.0f);
    float target = std::clamp((trophySel_ - 1) * rowH, 0.0f, maxScroll);
    trophyScroll_ = approach(trophyScroll_, target, dt, 12.0f);

    // On the console the library may only become readable a few seconds after start-up (HyperHEN
    // lifts the app's sandbox shortly after launch), so an empty library is retried for a while.
    if (games_.empty() && time_ < 15.0f) {
        libRetry_ -= dt;
        if (libRetry_ <= 0) {
            libRetry_ = 1.0f;
            games_ = plat_.listGames(emptyReason_);
            prefetchCovers();
            if (!games_.empty()) {
                sel_ = 0;
                selAnim_ = 0;
                reloadTrophies();
                sysRows_ = plat_.systemInfo((int)games_.size());
            }
        }
    }

    // Render one game's title texts per frame ahead of time, so moving to a game for the first
    // time does not have to rasterise its name mid-animation.
    if (textWarm_ < (int)games_.size()) {
        const Game& g = games_[textWarm_++];
        fitTitle(g);
        gfx_.textWidth(g.title, SemiBold, 54);
        gfx_.textWidth(g.titleId, Medium, 26);
    }

    static float sysTimer = 0;
    sysTimer += dt;
    if (tab_ == System && sysTimer > 0.5f) {
        sysTimer = 0;
        sysRows_ = plat_.systemInfo((int)games_.size());
    }

    // The music fades out as a game launches, and comes back if the launch fails.
    plat_.setMusicVolume(musicOn_ ? kMusicVolume * (1.0f - smooth(launchT_)) : 0.0f);
    musicAnim_ = approach(musicAnim_, musicOn_ ? 1.0f : 0.0f, dt, 18.0f);

    if (forceLaunchT_ >= 0) return;  // frozen preview of the transition

    if (launch_ == Launch::Out) {
        launchT_ += dt / 0.62f;
        if (launchT_ >= 1.0f) {
            launchT_ = 1.0f;
            LaunchResult r = plat_.launch(launching_);
            if (r.ok) {
                launch_ = Launch::Gone;
                goneT_ = 0;
            } else {
                toast(r.message);
                snd(Sfx::Error);
                launch_ = Launch::Back;
            }
        }
        return;
    }
    if (launch_ == Launch::Back) {
        launchT_ -= dt / 0.45f;
        if (launchT_ <= 0) {
            launchT_ = 0;
            launch_ = Launch::None;
        }
        return;
    }
    if (launch_ == Launch::Gone) {
        // The game is in front now. Nexus keeps running behind it, so come back to the menu
        // shortly after, ready for when the user returns.
        goneT_ += dt;
        if (goneT_ > 4.0f) launch_ = Launch::Back;
        return;
    }

    if (in.l1) setTab(tab_ - 1);
    if (in.r1) setTab(tab_ + 1);
    if (in.back && tab_ != Home) {
        snd(Sfx::Back);
        setTab(Home);
    }

    switch (tab_) {
        case Home:
            if (in.left) moveSel(-1);
            if (in.right) moveSel(1);
            if (in.confirm) startLaunch();
            if (in.triangle) setTab(Trophies);
            break;
        case Trophies: {
            auto idx = filteredTrophies();
            // Switching to a grade filter selects its first trophy, so that trophy's chime plays too
            // (this is also the only way to land on a game's single platinum with its filter on).
            if (in.left && filter_ > 0) { filter_--; trophySel_ = 0; tick(0.5f); snd(Sfx::Filter, 0.9f + 0.05f * filter_); if (filter_ != 0) trophyCue(); }
            if (in.right && filter_ < 4) { filter_++; trophySel_ = 0; tick(0.5f); snd(Sfx::Filter, 0.9f + 0.05f * filter_); if (filter_ != 0) trophyCue(); }
            if (in.up && trophySel_ > 0) { trophySel_--; tick(0.4f); trophyCue(); }
            if (in.down && trophySel_ + 1 < (int)idx.size()) { trophySel_++; tick(0.4f); trophyCue(); }
            break;
        }
        case Settings:
            if (in.up && settingsRow_ > 0) { settingsRow_--; snd(Sfx::Move); }
            if (in.down && settingsRow_ < 3) { settingsRow_++; snd(Sfx::Move); }
            if (settingsRow_ == 0) {
                if (in.left && vib_ > 0) { vib_--; saveSettings(); buzz(1.0f, 140); snd(Sfx::Setting, 0.8f + 0.09f * vib_); }
                if (in.right && vib_ < kVibCount - 1) { vib_++; saveSettings(); buzz(1.0f, 140); snd(Sfx::Setting, 0.8f + 0.09f * vib_); }
                if (in.confirm) buzz(1.0f, 240);
            } else if (settingsRow_ == 1) {
                if (in.left && sfx_ > 0) { sfx_--; saveSettings(); snd(Sfx::Setting, 0.8f + 0.12f * sfx_); }
                if (in.right && sfx_ < kSfxCount - 1) { sfx_++; saveSettings(); snd(Sfx::Setting, 0.8f + 0.12f * sfx_); }
                if (in.confirm) snd(Sfx::Gold);
            } else if (settingsRow_ == 2) {
                bool want = musicOn_;
                if (in.left) want = false;
                if (in.right) want = true;
                if (in.confirm) want = !musicOn_;
                if (want != musicOn_) {
                    musicOn_ = want;
                    saveSettings();
                    snd(Sfx::Setting, musicOn_ ? 1.2f : 0.8f);
                }
            } else if (in.confirm) {
                vib_ = kVibDefault;
                sfx_ = kSfxDefault;
                musicOn_ = true;
                saveSettings();
                buzz(1.0f, 160);
                snd(Sfx::Confirm);
                toast("Settings reset to defaults.");
            }
            break;
        default: break;
    }
}

// ------------------------------------------------------------------ helpers
void App::drawBadge(const std::string& t, float x, float y, Col c, Align a) {
    float tw = gfx_.textWidthSpaced(t, SemiBold, 16, 2.0f);
    float w = tw + 28, h = 30;
    if (a == Align::Right) x -= w;
    if (a == Align::Center) x -= w * 0.5f;
    gfx_.rrect(x, y, w, h, 8, withA(c, 0.14f), withA(c, 0.14f));
    gfx_.rrectStroke(x, y, w, h, 8, 1.5f, withA(c, 0.55f));
    gfx_.textSpaced(t, SemiBold, 16, x + 14, y + 4, c, 2.0f);
}

static float hintWidthForLabel(Gfx& g, const std::string& l) { return g.textWidth(l, Medium, 22); }

float App::drawHint(float x, float y, int kind, const std::string& label) {
    float cx = x + 16, cy = y + 16, w = 32;
    Col c = kSub;
    switch (kind) {
        case 0:  // cross
            gfx_.ring(cx, cy, 15, 2.5f, c);
            gfx_.line(cx - 6, cy - 6, cx + 6, cy + 6, 3, c);
            gfx_.line(cx - 6, cy + 6, cx + 6, cy - 6, 3, c);
            break;
        case 1:  // circle
            gfx_.ring(cx, cy, 15, 2.5f, c);
            gfx_.ring(cx, cy, 6.5f, 2.5f, c);
            break;
        case 2:  // d-pad horizontal
            gfx_.triangle(x, cy, x + 12, cy - 9, x + 12, cy + 9, c);
            gfx_.triangle(x + 34, cy, x + 22, cy - 9, x + 22, cy + 9, c);
            w = 34;
            break;
        case 3:  // d-pad vertical
            gfx_.triangle(cx, y, cx - 9, y + 12, cx + 9, y + 12, c);
            gfx_.triangle(cx, y + 34, cx - 9, y + 22, cx + 9, y + 22, c);
            w = 32;
            break;
        case 5: {  // triangle
            gfx_.ring(cx, cy, 15, 2.5f, c);
            gfx_.line(cx, cy - 7, cx + 7, cy + 5, 2.5f, c);
            gfx_.line(cx + 7, cy + 5, cx - 7, cy + 5, 2.5f, c);
            gfx_.line(cx - 7, cy + 5, cx, cy - 7, 2.5f, c);
            break;
        }
        default: {  // L1 R1
            for (int i = 0; i < 2; i++) {
                float bx = x + i * 52;
                gfx_.rrectStroke(bx, y + 3, 44, 26, 8, 2, c);
                gfx_.text(i == 0 ? "L1" : "R1", SemiBold, 17, bx + 22, y + 5, c, Align::Center);
            }
            w = 96;
        }
    }
    gfx_.text(label, Medium, 22, x + w + 12, y + 3, c);
    return w + 12 + hintWidthForLabel(gfx_, label) + 40;
}

void App::drawEmpty(const std::string& title, const std::string& body) {
    gfx_.text(title, SemiBold, 46, W / 2, 420, kText, Align::Center);
    gfx_.textWrapped(body, Regular, 26, W / 2 - 400, 500, 800, kSub, 38, 5);
}

void App::drawCover(const Game& g, float x, float y, float s, float rad, float br, bool atRest) {
    const Tex* t = g.iconPath.empty() ? nullptr : gfx_.texture(g.iconPath);
    Col tint = rgba((int)(255 * br), (int)(255 * br), (int)(255 * br));
    if (t && atRest) {  // draw a copy made at exactly this size, on whole pixels
        x = std::round(x);
        y = std::round(y);
        s = std::round(s);
        t = gfx_.scaled(t, (int)s, (int)s);
    }
    if (t) {
        gfx_.rrectTex(t, x, y, s, s, rad, tint);
    } else {
        Col a = lerp(kBlack, hex(g.colorA), br), b = lerp(kBlack, hex(g.colorB), br);
        gfx_.rrect(x, y, s, s, rad, a, b);
        gfx_.clip(x, y, s, s);
        gfx_.radial(x + s * 0.25f, y + s * 0.2f, s * 0.8f, rgba(255, 255, 255, (int)(38 * br)));
        gfx_.unclip();
        std::string mono;
        bool start = true;
        for (char c : g.title) {
            if (c == ' ') { start = true; continue; }
            if (start && mono.size() < 2) mono += (char)std::toupper((unsigned char)c);
            start = false;
        }
        gfx_.text(mono, Bold, (int)(s * 0.36f), x + s / 2, y + s * 0.5f - s * 0.25f,
                  rgba(255, 255, 255, (int)(235 * br)), Align::Center);
    }
    gfx_.rrectStroke(x, y, s, s, rad, 1.5f, rgba(255, 255, 255, 34));
}

// Red pillow with a gold edge and stitching; the trophy icon sits on it.
void App::drawCushion(float x, float y, float s, Grade grade, bool earned) {
    float rad = s * 0.24f;
    CushionPal pal = cushionPal(grade, earned);
    Col top = pal.top, bot = pal.bot, gold = pal.edge, goldDark = pal.edgeDark;

    gfx_.shadow(x, y + 6, s, s, rad, 16, kBlack);
    if (pal.icy && earned) gfx_.shadow(x, y, s, s, rad, 14, hex(0x4aa8ff));
    gfx_.rrect(x, y, s, s, rad, top, bot);
    if (pal.icy)  // soft cold glow behind the orb
        gfx_.rrect(x + s * 0.14f, y + s * 0.12f, s * 0.72f, s * 0.72f, rad * 0.9f,
                   rgba(150, 215, 255, earned ? 70 : 28), rgba(150, 215, 255, 0));
    // Puffy pillow shading, built from shapes that follow the outline (no spill past the corners).
    gfx_.rrect(x + s * 0.07f, y + s * 0.06f, s * 0.86f, s * 0.46f, rad * 0.8f,
               rgba(255, 255, 255, earned ? 52 : 26), rgba(255, 255, 255, 0));
    gfx_.rrect(x, y + s * 0.55f, s, s * 0.45f, rad, rgba(0, 0, 0, 0), rgba(0, 0, 0, 80));
    gfx_.rrectStroke(x, y, s, s, rad, s * 0.045f, gold);
    gfx_.rrectStroke(x + s * 0.045f, y + s * 0.045f, s - s * 0.09f, s - s * 0.09f, rad - s * 0.045f,
                     1.5f, goldDark);
    float in = s * 0.115f;
    gfx_.stitch(x + in, y + in, s - 2 * in, s - 2 * in, rad - in * 0.7f, s * 0.105f, 2.1f,
                withA(gold, earned ? 0.6f : 0.4f));

    drawTrophyIcon(gfx_, assets_, grade, x + s * 0.15f, y + s * 0.12f, s * 0.70f, s * 0.76f,
                   earned ? rgba(255, 255, 255) : rgba(150, 150, 158, 175));
}

// ------------------------------------------------------------------ screens
void App::drawBackground() {
    gfx_.alpha = 1;
    // The backdrop never changes, so it is drawn once and copied each frame. While Home is at
    // rest the glow behind the big cover is baked into a second copy (one layer, redrawn only
    // when the glow colour changes), which saves blending about a million pixels per frame.
    if (!gfx_.blitCache("backdrop")) {
        gfx_.beginCache("backdrop", (int)W, (int)H);
        gfx_.clear(kBg1);
        gfx_.rect(0, 0, W, H, kBg0, kBg1);
        gfx_.radial(260, -80, 1150, withA(kAccent, 0.085f));
        gfx_.radial(1750, 1180, 950, withA(kAccent, 0.05f));
        gfx_.endCache();
        gfx_.blitCache("backdrop");
    }
    bakedGlow_ = false;
    if (tab_ != Home || games_.empty() || launchT_ > 0 || forceLaunchT_ >= 0) return;
    const Game& g = games_[sel_];
    Col glow = g.iconPath.empty() ? hex(g.colorA) : kAccent;
    uint32_t key = (uint32_t)glow.r << 16 | (uint32_t)glow.g << 8 | glow.b;
    if (key != glowKey_ || !gfx_.blitCache("backdrop+glow")) {
        gfx_.beginCache("backdrop+glow", (int)W, (int)H);
        gfx_.blitCache("backdrop");
        gfx_.radial(kCoverX + kCoverS / 2, kCoverY + kCoverS / 2, kCoverS, withA(glow, 0.30f));
        gfx_.endCache();
        glowKey_ = key;
        gfx_.blitCache("backdrop+glow");
    }
    bakedGlow_ = true;
}

void App::drawHeader(float a) {
    gfx_.alpha = a;
    // wordmark (placeholder mark: replace with the KYT logo when available)
    float mx = 96, my = 60;
    gfx_.triangle(mx, my - 14, mx + 14, my, mx, my + 14, kAccent);
    gfx_.triangle(mx, my - 14, mx - 14, my, mx, my + 14, withA(kAccent, 0.55f));
    gfx_.textSpaced("NEXUS", SemiBold, 32, mx + 34, my - 21, kText, 7.0f);

    static const char* names[TabCount] = {"HOME", "TROPHIES", "SYSTEM", "SETTINGS"};
    const int sz = 25;
    const float track = 3.0f, gap = 66.0f;
    float widths[TabCount], total = 0;
    for (int i = 0; i < TabCount; i++) {
        widths[i] = gfx_.textWidthSpaced(names[i], Medium, sz, track);
        total += widths[i] + (i ? gap : 0);
    }
    float x = W / 2 - total / 2;
    float tx[TabCount];
    for (int i = 0; i < TabCount; i++) {
        tx[i] = x;
        x += widths[i] + gap;
    }
    float targetX = tx[tab_], targetW = widths[tab_];
    if (!underlineInit_) {
        underlineX_ = targetX;
        underlineW_ = targetW;
        underlineInit_ = true;
    }
    underlineX_ = approach(underlineX_, targetX, dt_, 16.0f);
    underlineW_ = approach(underlineW_, targetW, dt_, 16.0f);
    for (int i = 0; i < TabCount; i++)
        gfx_.textSpaced(names[i], Medium, sz, tx[i], 44, i == tab_ ? kText : kDim, track);
    gfx_.rrect(underlineX_, 88, underlineW_, 4, 2, kAccent, kAccent);

    // L1 / R1 chips
    auto chip = [&](const char* t, float cx) {
        gfx_.rrectStroke(cx - 28, 44, 56, 30, 8, 2, kDim);
        gfx_.text(t, SemiBold, 18, cx, 48, kDim, Align::Center);
    };
    chip("L1", tx[0] - 70);
    chip("R1", tx[TabCount - 1] + widths[TabCount - 1] + 70);

    // clock
    std::time_t tt = std::time(nullptr);
    std::tm* tm = std::localtime(&tt);
    char tb[32] = "", db[48] = "";
    if (tm) {
        std::strftime(tb, sizeof tb, "%I:%M %p", tm);
        std::strftime(db, sizeof db, "%B %d", tm);
    }
    std::string ts = tb[0] == '0' ? std::string(tb + 1) : std::string(tb);
    std::string ds = db;
    for (auto& c : ds) c = (char)std::toupper((unsigned char)c);
    gfx_.text(ts, Medium, 38, W - 96, 34, kText, Align::Right);
    gfx_.textSpaced(ds, Medium, 16, W - 96, 78, kDim, 3.0f, Align::Right);
}

void App::drawFooter(float a) {
    gfx_.alpha = a;
    gfx_.rect(96, 982, W - 192, 1.5f, kLine, kLine);
    float x = 96, y = 1004;
    switch (tab_) {
        case Home:
            if (!games_.empty()) {
                x += drawHint(x, y, 2, "Browse");
                x += drawHint(x, y, 0, "Launch");
                x += drawHint(x, y, 5, "Trophies");
            }
            break;
        case Trophies:
            if (trophies_.available) {
                x += drawHint(x, y, 2, "Filter");
                x += drawHint(x, y, 3, "Scroll");
            }
            x += drawHint(x, y, 1, "Back");
            break;
        case Settings:
            x += drawHint(x, y, 2, "Change");
            x += drawHint(x, y, 0, settingsRow_ == 0 ? "Test vibration" : settingsRow_ == 1 ? "Test sound" : settingsRow_ == 2 ? "Switch on / off" : "Reset");
            x += drawHint(x, y, 1, "Back");
            break;
        default: x += drawHint(x, y, 1, "Back");
    }
    drawHint(x, y, 4, "Switch tab");

    std::string label = plat_.name();
    if (plat_.previewData()) label += "  |  SAMPLE DATA";
    gfx_.textSpaced(label, SemiBold, 16, W - 96, 1010, plat_.previewData() ? hex(0xe0a84a) : kDim,
                    2.5f, Align::Right);
}

void App::drawHome() {
    if (games_.empty()) {
        drawEmpty("No games found", emptyReason_.empty() ? "Nothing is installed or the library could not be read." : emptyReason_);
        return;
    }
    const Game& g = games_[sel_];
    float lt = forceLaunchT_ >= 0 ? forceLaunchT_ : launchT_;

    // shelf ---------------------------------------------------------------
    const float x0 = 830, y0 = 250, T = kShelfT, gap = 30;
    gfx_.textSpaced("GAME LIBRARY", Medium, 20, x0, 172, kAccent, 4.0f);
    std::string cnt = std::to_string(sel_ + 1) + " / " + std::to_string(games_.size());
    gfx_.text(cnt, Medium, 20, W - 96, 171, kDim, Align::Right);
    float lw = gfx_.textWidthSpaced("GAME LIBRARY", Medium, 20, 4.0f);
    float cw = gfx_.textWidth(cnt, Medium, 20);
    gfx_.rect(x0 + lw + 24, 183, (W - 96 - cw - 24) - (x0 + lw + 24), 1.5f, kLine, kLine);

    gfx_.clip(x0 - 26, 200, W - x0 + 26, 420);
    for (int i = 0; i < (int)games_.size(); i++) {
        float d = i - selAnim_;
        float x = x0 + d * (T + gap);
        if (x + T < x0 - 30 || x > W) continue;
        float focus = clamp01(1.0f - fabsF(d));
        float s = T * (1.0f + (kShelfFocus - 1.0f) * focus);
        float cx = x + T / 2, cy = y0 + T / 2;
        float fade = d < 0 ? clamp01(1.0f + d * 1.2f) : 1.0f;
        float old = gfx_.alpha;
        gfx_.alpha = old * fade;
        gfx_.shadow(cx - s / 2, cy - s / 2, s, s, 18, 22 + 16 * focus, kBlack);
        // A cover at one of its two prefetched sizes (all but the one or two growing or
        // shrinking) is snapped to whole pixels and copied, even while the shelf slides.
        const float rs = std::round(s);
        const bool restSize = fabsF(s - rs) < 0.12f && (rs == std::round(T) || rs == std::round(T * kShelfFocus));
        drawCover(games_[i], cx - s / 2, cy - s / 2, s, 18, 0.62f + 0.38f * focus, restSize);
        if (focus > 0.01f)
            gfx_.rrectStroke(cx - s / 2 - 4, cy - s / 2 - 4, s + 8, s + 8, 22, 3, withA(kAccent, focus));
        gfx_.alpha = old;
    }
    gfx_.unclip();

    // info ----------------------------------------------------------------
    gfx_.textSpaced("ON DECK", Medium, 20, x0, 618, kAccent, 4.0f);
    const FitTitle& ft = fitTitle(g);
    if (ft.line2.empty()) {  // one line; smaller sizes sit lower, close to the title ID under it
        gfx_.text(ft.text, SemiBold, ft.size, x0 - 3, 646 + (76 - ft.size) * 0.6f, kText);
    } else {
        gfx_.text(ft.text, SemiBold, ft.size, x0 - 3, 640, kText);
        gfx_.text(ft.line2, SemiBold, ft.size, x0 - 3, 690, kText);
    }
    gfx_.text(g.titleId, Medium, 26, x0, 750, kSub);
    if (g.sample)
        drawBadge("SAMPLE", x0 + gfx_.textWidth(g.titleId, Medium, 26) + 22, 748, hex(0xe0a84a));

    float cx = 1390;
    gfx_.rect(cx - 50, 622, 1.5f, 160, kLine, kLine);
    gfx_.textSpaced("TROPHIES", Medium, 20, cx, 618, kAccent, 4.0f);
    if (trophies_.available) {
        int earned = 0, total = (int)trophies_.items.size();
        int cnts[4] = {0, 0, 0, 0}, ecn[4] = {0, 0, 0, 0};
        for (auto& t : trophies_.items) {
            cnts[(int)t.grade]++;
            if (t.earned) { earned++; ecn[(int)t.grade]++; }
        }
        gfx_.text(std::to_string(earned) + " / " + std::to_string(total), SemiBold, 46, cx, 646, kText);
        gfx_.rrect(cx, 714, 440, 8, 4, hex(0x242a36), hex(0x242a36));
        if (earned > 0) gfx_.rrect(cx, 714, 440.0f * earned / std::max(1, total), 8, 4, kAccent, kAccent);
        for (int i = 0; i < 4; i++) {
            float ix = cx + i * 112;
            drawTrophyIcon(gfx_, assets_, (Grade)i, ix, 732, 36, 44, rgba(255, 255, 255));
            gfx_.text(std::to_string(ecn[i]) + "/" + std::to_string(cnts[i]), Medium, 24, ix + 46, 742, kSub);
        }
        if (trophies_.sample) drawBadge("SAMPLE DATA", cx + 440, 618 - 4, hex(0xe0a84a), Align::Right);
    } else {
        gfx_.text("Not available", SemiBold, 34, cx, 650, kSub);
        gfx_.textWrapped(trophies_.reason, Regular, 22, cx, 700, 440, kDim, 30, 3);
    }

    // big cover -----------------------------------------------------------
    float bx = kCoverX, by = kCoverY, S = kCoverS;
    // The big cover keeps its size while browsing (an earlier 2.5% shrink while moving cost
    // ~300k filtered pixels a frame on the console), so it is copied 1:1 unless launching.
    const float pop = 1.0f;
    float zoom = 1.0f + smooth(lt) * 3.6f;
    float sz = S * pop * zoom;
    float drift = smooth(lt);  // the cover grows toward the middle of the screen
    float ccx = (bx + S / 2) + (W / 2 - (bx + S / 2)) * drift;
    float ccy = (by + S / 2) + (H / 2 - (by + S / 2)) * drift;
    float oldA = gfx_.alpha;
    // The cover stays opaque during the launch: the black overlay drawn last does the fade (the
    // backdrop is nearly black, so this looks the same as fading the cover out, and an opaque
    // cover is copied rather than blended).
    gfx_.alpha = smooth(contentT_);
    Col glow = g.iconPath.empty() ? hex(g.colorA) : kAccent;
    // The glow (about two million pixels once the cover zooms) fades out quickly as the launch starts.
    if (!bakedGlow_ && lt < 0.15f)
        gfx_.radial(ccx, ccy, S * 1.0f * zoom, withA(glow, 0.30f * (1.0f - lt / 0.15f)));
    gfx_.shadow(ccx - sz / 2, ccy - sz / 2, sz, sz, 24 * zoom, 46, kBlack);
    drawCover(g, ccx - sz / 2, ccy - sz / 2, sz, 24 * zoom, 1.0f, lt <= 0.0f);
    gfx_.alpha = oldA;

    // launch hint under the cover
    float la = 1.0f - clamp01(lt * 3);
    float old2 = gfx_.alpha;
    gfx_.alpha = old2 * la;
    float hx = bx + S / 2 - 62;
    gfx_.ring(hx + 16, by + S + 52, 15, 2.5f, kText);
    gfx_.line(hx + 10, by + S + 46, hx + 22, by + S + 58, 3, kText);
    gfx_.line(hx + 10, by + S + 58, hx + 22, by + S + 46, 3, kText);
    gfx_.textSpaced("LAUNCH", SemiBold, 20, hx + 44, by + S + 41, kText, 4.0f);
    gfx_.alpha = old2;
}

void App::drawTrophies() {
    if (games_.empty()) {
        drawEmpty("No game selected", "Pick a game on the Home tab to see its trophies.");
        return;
    }
    const Game& g = games_[sel_];
    gfx_.textSpaced("TROPHIES", Medium, 20, 130, 150, kAccent, 4.0f);
    gfx_.text(g.title, SemiBold, 54, 127, 176, kText);
    if (trophies_.sample) drawBadge("SAMPLE DATA", 130 + gfx_.textWidthSpaced("TROPHIES", Medium, 20, 4.0f) + 24, 144, hex(0xe0a84a));

    if (!trophies_.available) {
        drawEmpty("Trophy data unavailable", trophies_.reason);
        return;
    }

    // filter chips: earned / total per category
    static const char* names[5] = {"ALL", "BRONZE", "SILVER", "GOLD", "PLATINUM"};
    int counts[5] = {(int)trophies_.items.size(), 0, 0, 0, 0};
    int got[5] = {0, 0, 0, 0, 0};
    for (auto& t : trophies_.items) {
        counts[1 + (int)t.grade]++;
        if (t.earned) { got[0]++; got[1 + (int)t.grade]++; }
    }

    // whole-game summary
    {
        float rx = W - 130;
        std::string s1 = std::to_string(got[0]), s2 = " of " + std::to_string(counts[0]) + " earned";
        float w2 = gfx_.textWidth(s2, Medium, 28);
        gfx_.text(s2, Medium, 28, rx, 184, kSub, Align::Right);
        gfx_.text(s1, SemiBold, 54, rx - w2 - 8, 160, kText, Align::Right);
        gfx_.rrect(rx - 420, 232, 420, 8, 4, hex(0x242a36), hex(0x242a36));
        if (got[0] > 0)
            gfx_.rrect(rx - 420, 232, 420.0f * got[0] / std::max(1, counts[0]), 8, 4, kAccent, kAccent);
    }
    float cx = 130;
    for (int i = 0; i < 5; i++) {
        float w = i == 0 ? 170 : 236, h = 70, y = 262;
        bool on = i == filter_;
        gfx_.rrect(cx, y, w, h, 18, on ? kPanel2 : kPanel, on ? kPanel2 : kPanel);
        gfx_.rrectStroke(cx, y, w, h, 18, on ? 2.5f : 1.5f, on ? kAccent : kLine);
        float tx = cx + 24;
        if (i > 0) {
            drawTrophyIcon(gfx_, assets_, (Grade)(i - 1), cx + 16, y + 10, 40, 50, rgba(255, 255, 255));
            tx = cx + 68;
        }
        gfx_.textSpaced(names[i], SemiBold, 19, tx, y + 14, on ? kText : kSub, 2.5f);
        gfx_.text(std::to_string(got[i]) + " / " + std::to_string(counts[i]), Medium, 22, tx, y + 38,
                  on ? kAccent : kDim);
        cx += w + 18;
    }

    // list
    auto idx = filteredTrophies();
    const float lx = 130, ly = 360, lw = W - 260, rowH = 164, rh = 150;
    if (idx.empty()) {
        gfx_.text(trophies_.items.empty() ? "This app has no trophies." : "No trophies in this category.", Medium, 28,
                  W / 2, 600, kSub, Align::Center);
        return;
    }
    gfx_.clip(lx - 20, ly - 6, lw + 40, 626);
    for (int k = 0; k < (int)idx.size(); k++) {
        float y = ly + k * rowH - trophyScroll_;
        if (y + rh < ly - 10 || y > ly + 640) continue;
        const Trophy& t = trophies_.items[idx[k]];
        bool on = k == trophySel_;
        gfx_.rrect(lx, y, lw, rh, 24, on ? hex(0x1f2531) : kPanel, on ? hex(0x171c26) : kPanel);
        gfx_.rrectStroke(lx, y, lw, rh, 24, on ? 2.5f : 1.5f, on ? kAccent : kLine);
        drawCushion(lx + 18, y + 15, 120, t.grade, t.earned);
        float a = t.earned ? 1.0f : 0.55f;
        gfx_.text(t.name, SemiBold, 34, lx + 170, y + 24, withA(kText, a));
        gfx_.textWrapped(t.how, Regular, 24, lx + 170, y + 72, lw - 520, withA(kSub, a), 32, 2);
        gfx_.textSpaced(gradeName(t.grade), SemiBold, 18, lx + lw - 36, y + 30, gradeCol(t.grade), 3.0f, Align::Right);
        gfx_.textSpaced(t.earned ? "EARNED" : "LOCKED", SemiBold, 18, lx + lw - 36, y + 62,
                        t.earned ? hex(0x4fcf8a) : kDim, 3.0f, Align::Right);
    }
    gfx_.unclip();

    // soften the bottom edge where rows run under the footer
    gfx_.rect(0, 900, W, 80, rgba(8, 10, 15, 0), rgba(8, 10, 15, 255));

    // scrollbar
    float total = idx.size() * rowH, view = 626;
    if (total > view) {
        float bh = view * view / total, by = ly + (view - bh) * (trophyScroll_ / (total - 620.0f));
        gfx_.rrect(W - 112, ly, 4, view, 2, withA(kLine, 0.8f), withA(kLine, 0.8f));
        gfx_.rrect(W - 112, by, 4, bh, 2, kSub, kSub);
    }
}

void App::drawSystem() {
    // One centred panel. Values that could not be read are dimmed, never guessed.
    const float pw = 1160, px = (W - pw) / 2, py = 150, rowH = 76;
    const int n = std::min((int)sysRows_.size(), 9);
    gfx_.textSpaced("SYSTEM INFORMATION", Medium, 20, px, py, kAccent, 4.0f);
    gfx_.rrect(px, py + 44, pw, 36 + n * rowH, 24, kPanel, kPanel);
    gfx_.rrectStroke(px, py + 44, pw, 36 + n * rowH, 24, 1.5f, kLine);
    float y = py + 62;
    for (int i = 0; i < n; i++) {
        const SysRow& r = sysRows_[i];
        gfx_.text(r.label, Regular, 26, px + 48, y + 18, kSub);
        gfx_.text(r.value, Medium, 30, px + 430, y + 15, r.source == Source::Unavailable ? kDim : kText);
        if (r.source == Source::Sample)
            drawBadge("SAMPLE", px + pw - 40, y + 18, hex(0xe0a84a), Align::Right);
        y += rowH;
        if (i + 1 < n) gfx_.rect(px + 48, y - 2, pw - 96, 1.2f, kLine, kLine);
    }
}

void App::drawSettings() {
    float px = 130, py = 150, pw = 1100;
    gfx_.textSpaced("SETTINGS", Medium, 20, px, py, kAccent, 4.0f);

    // row 0: vibration
    float y = py + 50, h = 170;
    bool on0 = settingsRow_ == 0;
    gfx_.rrect(px, y, pw, h, 24, on0 ? kPanel2 : kPanel, kPanel);
    gfx_.rrectStroke(px, y, pw, h, 24, on0 ? 2.5f : 1.5f, on0 ? kAccent : kLine);
    gfx_.text("Controller vibration", SemiBold, 32, px + 40, y + 26, kText);
    gfx_.text("Strength of haptic feedback when browsing and launching.", Regular, 22, px + 40, y + 70, kSub);
    // 5-step meter
    for (int i = 0; i < kVibCount; i++) {
        float bx = px + 40 + i * 74;
        bool lit = i <= vib_ && vib_ > 0;
        if (i == 0) {
            gfx_.rrect(bx, y + 112, 62, 30, 8, vib_ == 0 ? kAccent : kPanel2, vib_ == 0 ? kAccent : kPanel2);
            gfx_.text("OFF", SemiBold, 16, bx + 31, y + 118, vib_ == 0 ? hex(0x06080b) : kDim, Align::Center);
        } else {
            gfx_.rrect(bx, y + 112, 62, 30, 8, lit ? kAccent : kPanel2, lit ? kAccent : kPanel2);
        }
    }
    gfx_.text(kVib[vib_].name, SemiBold, 30, px + pw - 40, y + 30, kAccent, Align::Right);
    gfx_.text(vib_ == kVibDefault ? "Default" : "Custom", Regular, 20, px + pw - 40, y + 70, kDim, Align::Right);

    // row 1: sound effects volume
    y += h + 24;
    h = 170;
    bool on1 = settingsRow_ == 1;
    gfx_.rrect(px, y, pw, h, 24, on1 ? kPanel2 : kPanel, kPanel);
    gfx_.rrectStroke(px, y, pw, h, 24, on1 ? 2.5f : 1.5f, on1 ? kAccent : kLine);
    gfx_.text("Sound effects", SemiBold, 32, px + 40, y + 26, kText);
    gfx_.text("Volume of menu sounds, trophy chimes and the launch whoosh.", Regular, 22, px + 40, y + 70, kSub);
    for (int i = 0; i < kSfxCount; i++) {
        float bx = px + 40 + i * 74;
        bool lit = i <= sfx_ && sfx_ > 0;
        if (i == 0) {
            gfx_.rrect(bx, y + 112, 62, 30, 8, sfx_ == 0 ? kAccent : kPanel2, sfx_ == 0 ? kAccent : kPanel2);
            gfx_.text("OFF", SemiBold, 16, bx + 31, y + 118, sfx_ == 0 ? hex(0x06080b) : kDim, Align::Center);
        } else {
            gfx_.rrect(bx, y + 112, 62, 30, 8, lit ? kAccent : kPanel2, lit ? kAccent : kPanel2);
        }
    }
    gfx_.text(kSfxLevels[sfx_].name, SemiBold, 30, px + pw - 40, y + 30, kAccent, Align::Right);
    gfx_.text(sfx_ == kSfxDefault ? "Default" : "Custom", Regular, 20, px + pw - 40, y + 70, kDim, Align::Right);

    // row 2: background music
    y += h + 24;
    h = 158;
    bool on2 = settingsRow_ == 2;
    gfx_.rrect(px, y, pw, h, 24, on2 ? kPanel2 : kPanel, kPanel);
    gfx_.rrectStroke(px, y, pw, h, 24, on2 ? 2.5f : 1.5f, on2 ? kAccent : kLine);
    gfx_.text("Background music", SemiBold, 32, px + 40, y + 26, kText);
    gfx_.text("A quiet ambient loop under the menus. Fades out when a game launches.", Regular, 22, px + 40, y + 70, kSub);
    // On/off switch: a lighter track with the knob to the right when on, a darker one when off.
    {
        float sx = px + 40, sy = y + 108, sw = 88, sh = 38;
        float a = smooth(musicAnim_);
        Col off = hex(0x1c222d), on = hex(0x8fd0ff);
        Col track = lerp(off, on, a);
        gfx_.rrect(sx, sy, sw, sh, sh / 2, track, track);
        gfx_.rrectStroke(sx, sy, sw, sh, sh / 2, 1.5f, lerp(rgba(255, 255, 255, 30), rgba(255, 255, 255, 70), a));
        float kx = sx + sh / 2 + (sw - sh) * a;
        gfx_.shadow(kx - 14, sy + sh / 2 - 14, 28, 28, 14, 6, kBlack);
        gfx_.circle(kx, sy + sh / 2, 14, lerp(hex(0x7c8697), hex(0xffffff), a));
        gfx_.textSpaced(musicOn_ ? "ON" : "OFF", SemiBold, 20, sx + sw + 22, sy + 7, musicOn_ ? kAccent : kDim, 3.0f);
    }
    gfx_.text(musicOn_ ? "On" : "Off", SemiBold, 30, px + pw - 40, y + 30, musicOn_ ? kAccent : kDim, Align::Right);
    gfx_.text(musicOn_ ? "Default" : "Custom", Regular, 20, px + pw - 40, y + 70, kDim, Align::Right);

    // row 3: reset
    y += h + 24;
    h = 100;
    bool on3 = settingsRow_ == 3;
    gfx_.rrect(px, y, pw, h, 24, on3 ? kPanel2 : kPanel, kPanel);
    gfx_.rrectStroke(px, y, pw, h, 24, on3 ? 2.5f : 1.5f, on3 ? kAccent : kLine);
    gfx_.text("Reset to defaults", SemiBold, 30, px + 40, y + 30, kText);

    gfx_.textWrapped("Changes apply immediately and are saved. Vibration needs a connected controller that supports it. "
                     "PS4 controllers only vibrate in PS4 games, so they will not vibrate in Nexus.",
                     Regular, 20, px, y + h + 36, pw, kDim, 28, 3);
}

// ------------------------------------------------------------------ frame
void App::draw() {
    gfx_.pump();  // take in covers decoded in the background
    float lt = forceLaunchT_ >= 0 ? forceLaunchT_ : launchT_;
    // Faded (all but invisibly) to black: draw only the black. The last few percent of the fade
    // cannot be seen, and drawing the zoomed cover under a near-opaque black is two full-screen
    // passes for nothing.
    if (lt >= 1.0f || smooth((lt - 0.55f) / 0.45f) >= 0.97f) {
        gfx_.alpha = 1;
        gfx_.clear(kBlack);
        if (launch_ == Launch::Gone)
            gfx_.text("Launching " + launching_.title + "...", Medium, 30, W / 2, H / 2 - 18, kSub, Align::Center);
        return;
    }
    drawBackground();
    drawHeader(1.0f - clamp01(lt * 1.6f));

    gfx_.alpha = smooth(contentT_) * (1.0f - clamp01(lt * 2.0f));
    switch (tab_) {
        case Home: drawHome(); break;
        case Trophies: drawTrophies(); break;
        case System: drawSystem(); break;
        default: drawSettings(); break;
    }
    // the cover is drawn inside drawHome with its own fade
    gfx_.alpha = 1;
    drawFooter(1.0f - clamp01(lt * 1.6f));

    if (lt > 0.0f) {
        float a = smooth((lt - 0.55f) / 0.45f);
        gfx_.alpha = a;
        gfx_.rect(0, 0, W, H, kBlack, kBlack);
    }
    gfx_.alpha = 1;
    if (showStats_) {  // diagnostic overlay for test builds: time spent per frame
        char buf[64];
        std::snprintf(buf, sizeof buf, "frame %.1f ms   %.0f fps", frameMs_, frameMs_ > 0 ? 1000.0f / frameMs_ : 0.0f);
        gfx_.text(buf, Medium, 18, W / 2, 1018, kDim, Align::Center);
    }
    if (launch_ == Launch::Gone)
        gfx_.text("Launching " + launching_.title + "...", Medium, 30, W / 2, H / 2 - 18, kSub, Align::Center);

    if (toastT_ > 0) {
        float a = clamp01(std::min(toastT_ / 0.4f, (3.2f - toastT_) / 0.2f));
        gfx_.alpha = a;
        float tw = gfx_.textWidth(toast_, Medium, 26) + 80;
        float x = W / 2 - tw / 2, y = 900 - 12 * (1 - a);
        gfx_.shadow(x, y, tw, 66, 20, 24, kBlack);
        gfx_.rrect(x, y, tw, 66, 20, hex(0x202734), hex(0x171c26));
        gfx_.rrectStroke(x, y, tw, 66, 20, 1.5f, rgba(255, 255, 255, 40));
        gfx_.text(toast_, Medium, 26, W / 2, y + 17, kText, Align::Center);
        gfx_.alpha = 1;
    }
}

}  // namespace nexus

