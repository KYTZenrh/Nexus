#include "ps5_platform.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/mount.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../../core/music_synth.h"
#include "../../core/sfx_synth.h"
#include "../console_library.h"

#ifndef NEXUS_TITLE_ID
#define NEXUS_TITLE_ID "PPSA99784"
#endif
#define NEXUS_VERSION "1.0.0"

// Minimal prototypes/structs for the Sony libraries this SDK leaves undeclared (the same set
// used by ps5-payload-dev/SDL's PS5 video and joystick backends, which is where these exact
// signatures and struct layouts come from).
extern "C" {
int sceSystemServiceHideSplashScreen(void);

int sceKernelAllocateMainDirectMemory(size_t, size_t, int, intptr_t*);
int sceKernelMapDirectMemory(void**, size_t, int, int, intptr_t, size_t);
int sceKernelReleaseDirectMemory(intptr_t, size_t);

int sceKernelCreateEqueue(struct kevent**, const char*);
int sceKernelWaitEqueue(struct kevent*, struct kevent*, int, int*, unsigned int*);
int sceKernelDeleteEqueue(struct kevent*);
uint64_t sceKernelGetProcessTime(void);

int sceVideoOutOpen(int, int, int, const void*);
void sceVideoOutClose(int);
int sceVideoOutAddFlipEvent(struct kevent*, int, void*);
int sceVideoOutSetFlipRate(int, int);
int sceVideoOutSubmitFlip(int, int, uint32_t, int64_t);
int sceVideoOutDeleteFlipEvent(struct kevent*, int);

struct Ps5VideoBuf {
    void* data;
    uint64_t junk0[3];
};
struct Ps5VideoAttr {
    uint8_t junk0[80];
};
void sceVideoOutSetBufferAttribute2(Ps5VideoAttr*, uint64_t, uint32_t, uint32_t, uint32_t,
                                    uint64_t, uint32_t, uint64_t);
int sceVideoOutRegisterBuffers2(int, int, int, Ps5VideoBuf*, int, Ps5VideoAttr*, int, void*);
}

#define PS5_USER_ID_SYSTEM 0xff

#define PS5_PAD_PORT_TYPE_STANDARD 0

#define PS5_PAD_BUTTON_L3 0x0002
#define PS5_PAD_BUTTON_R3 0x0004
#define PS5_PAD_BUTTON_OPTIONS 0x0008
#define PS5_PAD_BUTTON_UP 0x0010
#define PS5_PAD_BUTTON_RIGHT 0x0020
#define PS5_PAD_BUTTON_DOWN 0x0040
#define PS5_PAD_BUTTON_LEFT 0x0080
#define PS5_PAD_BUTTON_L1 0x0400
#define PS5_PAD_BUTTON_R1 0x0800
#define PS5_PAD_BUTTON_TRIANGLE 0x1000
#define PS5_PAD_BUTTON_CIRCLE 0x2000
#define PS5_PAD_BUTTON_CROSS 0x4000
#define PS5_PAD_BUTTON_SQUARE 0x8000

struct Ps5PadTouch {
    uint16_t x, y;
    uint8_t finger;
    uint8_t pad[3];
};
struct Ps5PadTouchData {
    uint8_t fingers;
    uint8_t pad1[3];
    uint32_t pad2;
    Ps5PadTouch touch[2];
};
struct Ps5PadData {
    uint32_t buttons;
    struct { uint8_t x, y; } leftStick;
    struct { uint8_t x, y; } rightStick;
    struct { uint8_t l2, r2; } analogButtons;
    uint16_t padding;
    struct { float x, y, z, w; } quat;
    struct { float x, y, z; } vel;
    struct { float x, y, z; } acell;
    Ps5PadTouchData touch;
    uint8_t connected;
    uint64_t timestamp;
    uint8_t ext[16];
    uint8_t count;
    uint8_t unknown[15];
};
struct Ps5PadVibration {
    uint8_t large_motor;
    uint8_t small_motor;
};

extern "C" {
int scePadInit(void);
int scePadOpen(int userId, int type, int index, void* param);
int scePadReadState(int handle, Ps5PadData* data);
int scePadSetVibrationMode(int handle, int mode);
int scePadSetVibration(int handle, const Ps5PadVibration* vib);
int scePadClose(int handle);

int sceUserServiceInitialize(void*);
int sceUserServiceGetInitialUser(int*);
int sceUserServiceGetUserName(int, char*, size_t);

// Audio output: one port on the main output, 16-bit stereo at 48 kHz.
int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, uint32_t frames, uint32_t freq, uint32_t format);
int sceAudioOutOutput(int handle, const void* buf);
int sceAudioOutSetVolume(int handle, int flags, int* vol);

// System information. SwVersion is SceKernelSwVersion: size, printable version, packed version.
struct SwVersion {
    uint64_t size;
    char str[0x1c];
    uint32_t version;
};
int sceKernelGetProsperoSystemSwVersion(SwVersion*);
// Note: functions that only libkernel_sys exports (sceKernelGetCpuTemperature, statfs) are not
// open to apps: their imports resolve to address 0 and calling them crashes. So there is no
// temperature row on the console, and storage uses _fstatfs, which the app libkernel exports.
int _fstatfs(int fd, struct statfs* buf);
}

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
        std::fclose(f);
    }
}

// ---------------------------------------------------------------- display

bool Ps5Display::init() {
    sceSystemServiceHideSplashScreen();

    // On any failure below, release whatever was already acquired before returning, so a failed
    // attempt never sits there holding the display (or memory) hostage from future attempts.
    handle_ = sceVideoOutOpen(PS5_USER_ID_SYSTEM, 0, 0, nullptr);
    if (handle_ < 0) {
        logLine("video: sceVideoOutOpen failed (%d)", handle_);
        handle_ = -1;
        return false;
    }

    // Exactly what two tiled 1920x1080 buffers need (about 17 MiB total), rounded up to the
    // 128 KiB alignment the allocator requires - not the SDK sample's flat 64 MiB, which asks
    // for far more than this app needs and makes an allocation failure more likely on a console
    // already running other background homebrew.
    const size_t align = 0x20000;
    size_t bufBytes = ((Ps5Tilemap::bufferSize(kW, kH) + align - 1) / align) * align;
    memsize_ = bufBytes * 2;
    if (sceKernelAllocateMainDirectMemory(memsize_, 0x20000, 3, &paddr_)) {
        logLine("video: sceKernelAllocateMainDirectMemory failed");
        memsize_ = 0;
        sceVideoOutClose(handle_);
        handle_ = -1;
        return false;
    }
    void* vaddr = nullptr;
    if (sceKernelMapDirectMemory(&vaddr, memsize_, 0x33, 0, paddr_, 0x20000)) {
        logLine("video: sceKernelMapDirectMemory failed");
        sceKernelReleaseDirectMemory(paddr_, memsize_);
        paddr_ = 0;
        memsize_ = 0;
        sceVideoOutClose(handle_);
        handle_ = -1;
        return false;
    }
    vbuf_[0] = vaddr;
    vbuf_[1] = (char*)vaddr + memsize_ / 2;

    if (sceKernelCreateEqueue(&evtQueue_, "nexus flip queue")) {
        logLine("video: sceKernelCreateEqueue failed");
        sceKernelReleaseDirectMemory(paddr_, memsize_);
        paddr_ = 0;
        memsize_ = 0;
        sceVideoOutClose(handle_);
        handle_ = -1;
        return false;
    }
    if (sceVideoOutAddFlipEvent(evtQueue_, handle_, nullptr)) {
        logLine("video: sceVideoOutAddFlipEvent failed");
        sceKernelDeleteEqueue(evtQueue_);
        evtQueue_ = nullptr;
        sceKernelReleaseDirectMemory(paddr_, memsize_);
        paddr_ = 0;
        memsize_ = 0;
        sceVideoOutClose(handle_);
        handle_ = -1;
        return false;
    }
    sceVideoOutSetFlipRate(handle_, 0);

    Ps5VideoAttr vattr = {};
    sceVideoOutSetBufferAttribute2(&vattr, 0x8000000022000000ULL, 0, kW, kH, 0, 0, 0);
    Ps5VideoBuf buf2[2] = {{vbuf_[0], {}}, {vbuf_[1], {}}};
    if (sceVideoOutRegisterBuffers2(handle_, 0, 0, buf2, 2, &vattr, 0, nullptr)) {
        logLine("video: sceVideoOutRegisterBuffers2 failed");
        sceVideoOutDeleteFlipEvent(evtQueue_, handle_);
        sceKernelDeleteEqueue(evtQueue_);
        evtQueue_ = nullptr;
        sceKernelReleaseDirectMemory(paddr_, memsize_);
        paddr_ = 0;
        memsize_ = 0;
        sceVideoOutClose(handle_);
        handle_ = -1;
        return false;
    }

    if (!tmap_.create(kW, kH)) {
        logLine("video: tilemap allocation failed");
        sceVideoOutDeleteFlipEvent(evtQueue_, handle_);
        sceKernelDeleteEqueue(evtQueue_);
        evtQueue_ = nullptr;
        sceKernelReleaseDirectMemory(paddr_, memsize_);
        paddr_ = 0;
        memsize_ = 0;
        sceVideoOutClose(handle_);
        handle_ = -1;
        return false;
    }

    ready_ = true;
    logLine("video: ready (%dx%d, 2 buffers)", kW, kH);
    return true;
}

void Ps5Display::present(const uint32_t* pixels) {
    if (!ready_) return;
    tmap_.blit(pixels, kW, vbuf_[active_], active_, nullptr, 0, 0, 0);

    sceVideoOutSubmitFlip(handle_, active_, 1, frameId_);
    for (;;) {
        alignas(16) unsigned char evt[64];  // room for one kevent of either FreeBSD layout
        int count = 0;
        if (sceKernelWaitEqueue(evtQueue_, reinterpret_cast<kevent*>(evt), 1, &count, nullptr)) break;
        break;  // one flip event is enough to know the flip completed
    }
    active_ ^= 1;
    frameId_++;
}

// ---------------------------------------------------------------- platform

Ps5Platform::Ps5Platform() { lastUs_ = sceKernelGetProcessTime(); }

// Starts the user service and the controller library, each step logged.
bool Ps5Platform::initPad() {
    if (int rc = sceUserServiceInitialize(nullptr); rc != 0 && (unsigned)rc != 0x80960003) {
        logLine("pad: sceUserServiceInitialize failed (0x%x)", rc);
        return false;
    }
    if (scePadInit() != 0) {
        logLine("pad: scePadInit failed");
        return false;
    }
    if (sceUserServiceGetInitialUser(&userId_) != 0) {
        logLine("pad: could not get the initial user");
        return false;
    }
    pad_ = scePadOpen(userId_, PS5_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    int vm = scePadSetVibrationMode(pad_, 2);
    logLine("pad: user 0x%x, handle %d, vibration mode -> 0x%x", userId_, pad_, vm);
    lastUs_ = sceKernelGetProcessTime();
    return pad_ >= 0;
}

Ps5Platform::~Ps5Platform() {
    if (pad_ >= 0) {
        Ps5PadVibration off = {0, 0};
        scePadSetVibration(pad_, &off);
        scePadClose(pad_);
    }
}

// ---------------------------------------------------------------- audio

static const int kAudioFrames = 256;

void Ps5Platform::initAudio() {
    mixer_.loadSfx();
    // A non-zero result can just mean "already initialised"; opening the port is the real test.
    int rc = sceAudioOutInit();
    logLine("audio: sceAudioOutInit -> 0x%x", rc);
    // user = system, port type 0 = main output, format 1 = 16-bit stereo
    audioPort_ = sceAudioOutOpen(PS5_USER_ID_SYSTEM, 0, 0, kAudioFrames, kSfxRate, 1);
    if (audioPort_ < 0) {
        logLine("audio: sceAudioOutOpen failed (0x%x)", audioPort_);
        audioPort_ = -1;
        return;
    }
    int vol[2] = {32768, 32768};  // 0 dB on both channels
    sceAudioOutSetVolume(audioPort_, 0x3, vol);

    pthread_t t;
    if (pthread_create(&t, nullptr, &Ps5Platform::audioThread, this) != 0) {
        logLine("audio: could not start the audio thread");
        return;
    }
    pthread_detach(t);
    // The music loop takes about a second to generate; do it off the main thread.
    pthread_t m;
    if (pthread_create(&m, nullptr, [](void* self) -> void* {
            static_cast<Ps5Platform*>(self)->mixer_.setMusic(synthMusic());
            logLine("audio: music ready");
            return nullptr;
        }, this) == 0)
        pthread_detach(m);
    logLine("audio: ready (port %d)", audioPort_);
}

// Mixes and hands buffers to the audio port forever; sceAudioOutOutput blocks until the
// previous buffer has been taken, which paces this loop. Two buffers alternate so the one
// being played is never overwritten.
void* Ps5Platform::audioThread(void* arg) {
    auto* self = static_cast<Ps5Platform*>(arg);
    static float mix[kAudioFrames * 2];
    static int16_t out[2][kAudioFrames * 2];
    for (int n = 0;; n ^= 1) {
        self->mixer_.mix(mix, kAudioFrames);
        for (int i = 0; i < kAudioFrames * 2; i++) {
            float s = mix[i];
            s = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
            out[n][i] = (int16_t)(s * 32767.0f);
        }
        sceAudioOutOutput(self->audioPort_, out[n]);
    }
    return nullptr;
}

void Ps5Platform::playSfx(Sfx s, float volume, float pitch) {
    if (audioPort_ >= 0) mixer_.play(s, volume, pitch);
}

// ---------------------------------------------------------------- input

bool Ps5Platform::repeatFire(Repeat& r, bool down, float dt) {
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

void Ps5Platform::poll(InputState& in) {
    in = InputState{};
    uint64_t now = sceKernelGetProcessTime();
    float dt = lastUs_ ? (float)(now - lastUs_) / 1000000.0f : 0.016f;
    lastUs_ = now;

    if (rumbleStopUs_ && now >= rumbleStopUs_ && pad_ >= 0) {
        Ps5PadVibration off = {0, 0};
        scePadSetVibration(pad_, &off);
        rumbleStopUs_ = 0;
    }
    if (pad_ < 0) return;

    Ps5PadData d;
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
    if (pressed & PS5_PAD_BUTTON_CROSS) in.confirm = true;
    if (pressed & PS5_PAD_BUTTON_CIRCLE) in.back = true;
    if (pressed & PS5_PAD_BUTTON_TRIANGLE) in.triangle = true;
    if (pressed & PS5_PAD_BUTTON_L1) in.l1 = true;
    if (pressed & PS5_PAD_BUTTON_R1) in.r1 = true;
    if (pressed & PS5_PAD_BUTTON_OPTIONS) in.options = true;

    const bool up = (b & PS5_PAD_BUTTON_UP) || d.leftStick.y < 64;
    const bool down = (b & PS5_PAD_BUTTON_DOWN) || d.leftStick.y > 192;
    const bool left = (b & PS5_PAD_BUTTON_LEFT) || d.leftStick.x < 64;
    const bool right = (b & PS5_PAD_BUTTON_RIGHT) || d.leftStick.x > 192;
    in.up = repeatFire(rep_[0], up, dt);
    in.down = repeatFire(rep_[1], down, dt);
    in.left = repeatFire(rep_[2], left, dt);
    in.right = repeatFire(rep_[3], right, dt);
}

// The console drives the motors harder than the PC (SDL) path does for the same value, so
// levels are scaled down to feel closer to the PC preview. But the motors do not turn at all
// below roughly a fifth of full power, so anything switched on is kept above that floor
// (scaling by 0.45 alone silenced the Low setting entirely).
static const float kConsoleRumbleScale = 0.65f, kConsoleRumbleFloor = 0.22f;

void Ps5Platform::rumble(float s, int ms) {
    if (pad_ < 0 || !connected_ || s <= 0.0f) return;
    s *= kConsoleRumbleScale;
    if (s < kConsoleRumbleFloor) s = kConsoleRumbleFloor;
    if (s > 1.0f) s = 1.0f;
    Ps5PadVibration p;
    p.large_motor = (uint8_t)(s * 255.0f);
    p.small_motor = (uint8_t)(s * 0.8f * 255.0f);
    scePadSetVibration(pad_, &p);
    rumbleStopUs_ = sceKernelGetProcessTime() + (uint64_t)ms * 1000ull;
}

// ---------------------------------------------------------------- sandbox

// HyperHEN lifts the sandbox of allowed apps (title IDs under [app_jailbreak] in its config) on
// request: it watches each allowed app's /download0 folder for a small JSON file naming the
// process. It deletes the file once handled, and logs "[JB] OK ... fully jailbroken".
void Ps5Platform::requestJailbreak() {
    char json[96];
    std::snprintf(json, sizeof json, "{\"PID\":%d,\"pid\":%d}", (int)getpid(), (int)getpid());
    // HyperHEN only reacts to changes made after it starts watching, which can be a moment after
    // launch, so a stale request is removed and written again on each attempt.
    const char* path = "/download0/hyperhen_jailbreak";
    unlink("/download0/jailbreak_request.json");  // left by an earlier test build
    unlink(path);
    // Written under another name and renamed into place, so it is never read half-written.
    const char* tmp = "/download0/.hyperhen_jailbreak.tmp";
    FILE* f = std::fopen(tmp, "w");
    if (!f) {
        logLine("sandbox: could not write %s", tmp);
        return;
    }
    std::fputs(json, f);
    std::fclose(f);
    if (rename(tmp, path) != 0) logLine("sandbox: rename to %s failed", path);
    logLine("sandbox: jailbreak requested %s", json);
}

// ---------------------------------------------------------------- library and launching

static const char* kJailbreakHint =
    "Nexus needs HyperHEN's app jailbreak to read your library. Add " NEXUS_TITLE_ID
    " to exact_title_ids under [app_jailbreak] in /data/HyperHEN/config.ini.";

std::vector<Game> Ps5Platform::listGames(std::string& emptyReason) {
    std::string err;
    auto games = readConsoleLibrary("/system_data/priv/mms/app.db", (uint32_t)userId_, NEXUS_TITLE_ID, err);
    if (games.empty()) {
        logLine("library: %s", err.c_str());
        if (err.rfind("Could not open", 0) == 0) requestJailbreak();  // still sandboxed: ask again
        emptyReason = err.rfind("Could not open", 0) == 0 ? kJailbreakHint : err;
        return games;
    }
    int icons = 0;
    for (auto& g : games)
        if (FILE* f = std::fopen(g.iconPath.c_str(), "rb")) {
            icons++;
            std::fclose(f);
        }
    logLine("library: %d titles, %d icons readable", (int)games.size(), icons);
    return games;
}

// Overwrites a 32-byte placeholder slot in the launcher payload with a title ID.
static bool patchSlot(std::vector<char>& elf, const char* placeholder, const std::string& value) {
    const size_t n = std::strlen(placeholder);
    for (size_t i = 0; i + 32 <= elf.size(); i++) {
        if (std::memcmp(&elf[i], placeholder, n) == 0) {
            std::memset(&elf[i], 0, 32);
            std::memcpy(&elf[i], value.data(), std::min<size_t>(value.size(), 31));
            return true;
        }
    }
    return false;
}

// Apps may not call the system's launch functions (their imports resolve to address 0, and
// they are not exported to apps even when looked up by name), and only one big app can run at a
// time. So Nexus hands the job to a small payload (tools/ps5-launcher/launcher.c, shipped as
// assets/launcher.elf) through the ELF loader on this console: it closes Nexus, then opens the
// game's deep link so the home screen launches it and gives it the screen.
void Ps5Platform::loadLauncher(const std::string& path) {
    launcher_.clear();
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[16384];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) launcher_.insert(launcher_.end(), buf, buf + n);
        std::fclose(f);
    }
    logLine("launch: launcher payload %zu bytes", launcher_.size());
}

LaunchResult Ps5Platform::launch(const Game& g) {
    LaunchResult r;
    std::vector<char> elf = launcher_;
    if (elf.empty() || !patchSlot(elf, "NEXUS_SLOT_TARGET_TITLE_ID", g.titleId) ||
        !patchSlot(elf, "NEXUS_SLOT_CLOSE_TITLE_ID", NEXUS_TITLE_ID)) {
        logLine("launch: launcher payload missing or invalid");
        r.message = "Nexus is missing its launcher. Reinstall Nexus.";
        return r;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(9021);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || connect(s, (sockaddr*)&a, sizeof a) != 0) {
        if (s >= 0) close(s);
        logLine("launch: cannot reach the ELF loader on port 9021");
        r.message = "Can't launch: the ELF loader (port 9021) is not running. Run the exploit chain again.";
        return r;
    }
    size_t sent = 0;
    while (sent < elf.size()) {
        ssize_t k = send(s, elf.data() + sent, elf.size() - sent, 0);
        if (k <= 0) break;
        sent += (size_t)k;
    }
    close(s);
    logLine("launch: %s, sent %zu of %zu bytes to the ELF loader", g.titleId.c_str(), sent, elf.size());
    r.ok = sent == elf.size();
    if (!r.ok) r.message = "Could not start " + g.title + ".";
    return r;
}

// PS4 games (CUSA...) have trophy sets, but per-game progress is only in the console's
// encrypted trophy data, which Nexus does not read. Everything else on this console (homebrew,
// media apps) has no trophy set, so it honestly has 0 of 0.
TrophyList Ps5Platform::trophiesFor(const Game& g) {
    TrophyList t;
    if (g.titleId.rfind("CUSA", 0) == 0) {
        t.reason = "Progress for this game can't be read. Your totals across all games are on the System tab.";
        return t;
    }
    t.available = true;  // no trophies at all: 0 of 0
    return t;
}

// The console's own summary of the signed-in user's trophies, a small plain JSON file:
// {"format":1,"earnedTrophies":{"platinum":0,"gold":1,"silver":4,"bronze":15}}
bool Ps5Platform::trophyTotals(int counts[4]) {
    char path[96];
    std::snprintf(path, sizeof path, "/user/home/%08x/trophy/data/sce_trop/trpsummary.dat", (unsigned)userId_);
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char buf[512] = {0};
    std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    static const char* keys[4] = {"\"bronze\":", "\"silver\":", "\"gold\":", "\"platinum\":"};
    for (int i = 0; i < 4; i++) {
        const char* p = std::strstr(buf, keys[i]);
        if (!p) return false;
        counts[i] = std::atoi(p + std::strlen(keys[i]));
    }
    return true;
}

// ---------------------------------------------------------------- system information

static std::string bytesText(double b) {
    char buf[32];
    if (b >= 1e12) std::snprintf(buf, sizeof buf, "%.2f TB", b / 1e12);
    else std::snprintf(buf, sizeof buf, "%.1f GB", b / 1e9);
    return buf;
}

std::vector<SysRow> Ps5Platform::systemInfo(int gamesFound) {
    // Fixed values are read once; temperature and storage are re-read on every refresh.
    static std::string firmware, user, ip;
    static bool once = false;
    if (!once) {
        once = true;
        SwVersion v;
        std::memset(&v, 0, sizeof v);
        v.size = sizeof v;
        int rc = sceKernelGetProsperoSystemSwVersion(&v);
        if (rc == 0 && v.version) {  // packed BCD: 0x13600007 is 13.60
            char buf[16];
            std::snprintf(buf, sizeof buf, "%x.%02x", (v.version >> 24) & 0xff, (v.version >> 16) & 0xff);
            firmware = buf;
        }
        logLine("sysinfo: firmware rc=0x%x '%s' (0x%08x)", rc, v.str, v.version);
        char name[64] = {0};
        rc = sceUserServiceGetUserName(userId_, name, sizeof name - 1);
        if (rc == 0) user = name;
        logLine("sysinfo: user rc=0x%x '%s'", rc, user.c_str());
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) == 0) {
            for (ifaddrs* a = list; a; a = a->ifa_next) {
                if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
                char buf[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &((sockaddr_in*)a->ifa_addr)->sin_addr, buf, sizeof buf);
                if (std::strncmp(buf, "127.", 4) != 0) {
                    ip = buf;
                    break;
                }
            }
            freeifaddrs(list);
        }
        logLine("sysinfo: ip '%s'", ip.c_str());
    }

    std::vector<SysRow> rows;
    rows.push_back({"Console", "PlayStation 5", Source::Live});
    rows.push_back({"System software", firmware.empty() ? "Could not be read" : firmware,
                    firmware.empty() ? Source::Unavailable : Source::Live});
    rows.push_back({"User", user.empty() ? "Could not be read" : user,
                    user.empty() ? Source::Unavailable : Source::Live});

    // /data lives on the same partition as installed games (/user). The buffer is padded in
    // case the console's struct statfs is larger than this SDK's.
    union {
        struct statfs s;
        uint8_t pad[4096];
    } u;
    std::memset(&u, 0, sizeof u);
    struct statfs& fs = u.s;
    int src = -1;
    int fd = open("/data", O_RDONLY);
    if (fd >= 0) {
        src = _fstatfs(fd, &fs);
        close(fd);
    }
    static bool loggedFs = false;
    if (!loggedFs) {
        loggedFs = true;
        logLine("sysinfo: fstatfs fd=%d rc=%d bsize=%llu blocks=%llu avail=%llu", fd, src,
                (unsigned long long)fs.f_bsize, (unsigned long long)fs.f_blocks,
                (unsigned long long)fs.f_bavail);
    }
    if (src == 0 && fs.f_blocks > 0 && fs.f_bsize > 0) {
        double total = (double)fs.f_blocks * fs.f_bsize, avail = (double)fs.f_bavail * fs.f_bsize;
        rows.push_back({"Storage", bytesText(avail) + " free of " + bytesText(total), Source::Live});
    } else {
        rows.push_back({"Storage", "Could not be read", Source::Unavailable});
    }

    rows.push_back({"IP address", ip.empty() ? "Not connected" : ip, ip.empty() ? Source::Unavailable : Source::Live});
    rows.push_back({"Games and apps", std::to_string(gamesFound) + " installed", Source::Live});
    int tc[4];
    const bool haveTrophies = trophyTotals(tc);
    static bool loggedTrophies = false;
    if (!loggedTrophies && gamesFound > 0) {  // after the sandbox is lifted
        loggedTrophies = true;
        if (haveTrophies) logLine("sysinfo: trophies %d/%d/%d/%d", tc[0], tc[1], tc[2], tc[3]);
        else logLine("sysinfo: trophy summary not readable");
    }
    if (haveTrophies) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%d bronze  \xC2\xB7  %d silver  \xC2\xB7  %d gold  \xC2\xB7  %d platinum", tc[0],
                      tc[1], tc[2], tc[3]);
        rows.push_back({"Trophies earned", buf, Source::Live});
    } else {
        rows.push_back({"Trophies earned", "Could not be read", Source::Unavailable});
    }
    rows.push_back({"Controller", connected_ ? "Connected" : "Not connected", Source::Live});
    rows.push_back({"Nexus version", NEXUS_VERSION, Source::Live});
    return rows;
}

}  // namespace nexus
