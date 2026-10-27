// Launcher payload, sent to the ELF loader (port 9021). Two uses:
//  - Dev helper (tools/ps5-launcher/launch.sh): (re)starts the title given at build time
//    (-DTITLE_ID), closing it first if it is already running. -DKILL_ONLY only closes it.
//  - Nexus (shipped as assets/launcher.elf): Nexus writes the game's title ID into g_target
//    and its own into g_close, then sends this to 127.0.0.1:9021. Apps may not call the launch
//    functions themselves, and only one big app can run, so this closes Nexus and then starts
//    the game (see launchTitle for how).
// Results go to /data/nexus_launcher.log and a system notification.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef TITLE_ID
#define TITLE_ID "PPSA99780"
#endif

// Slots Nexus patches in place; left as-is when sent by launch.sh. Each is 32 bytes.
__attribute__((used)) static volatile char g_target[32] = "NEXUS_SLOT_TARGET_TITLE_ID_____";
__attribute__((used)) static volatile char g_close[32] = "NEXUS_SLOT_CLOSE_TITLE_ID______";

typedef struct {
    uint32_t size;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} LncAppParam;

int sceLncUtilInitialize(void);
int sceLncUtilLaunchApp(const char* title_id, const char* argv[], LncAppParam* param);
int sceLncUtilGetAppId(const char* title_id);
int sceLncUtilKillApp(int app_id);
int sceLncUtilForceKillApp(int app_id);
int sceShellCoreUtilLaunchByUri(const char* uri, void* param);
int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(int* user_id);
int sceUserServiceTerminate(void);

typedef struct { char unused[45]; char message[3075]; } NotifyReq;
int sceKernelSendNotificationRequest(int, NotifyReq*, size_t, int);

static void logf_(int notify, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    FILE* f = fopen("/data/nexus_launcher.log", "a");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
    if (!notify) return;
    NotifyReq req;
    memset(&req, 0, sizeof req);
    strncpy(req.message, buf, sizeof req.message - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

// Copies a patched slot, or returns 0 if it still holds its placeholder.
static int slot(const volatile char* s, char* out) {
    for (int i = 0; i < 31; i++) out[i] = s[i];
    out[31] = 0;
    return strncmp(out, "NEXUS_SLOT_", 11) != 0 && out[0];
}

static void closeApp(const char* tid) {
    int running = sceLncUtilGetAppId(tid);
    if (running <= 0) return;
    int k = sceLncUtilKillApp(running);
    if (k < 0) k = sceLncUtilForceKillApp(running);
    logf_(0, "launcher: closed %s (app 0x%x) rc=0x%x", tid, running, k);
}

// Starts a title the way the home screen does: by opening its deep link, so the home screen
// runs the launch itself (splash screen, then the app gets the screen and the controller).
// sceLncUtilLaunchApp alone starts the app but leaves the home screen in front of it. Returns
// the app id, or a negative error.
static int launchTitle(const char* tid, int user) {
    char uri[96];
    snprintf(uri, sizeof uri, "psgm:play?id=%s", tid);
    uint8_t prm[64];  // launch parameters: size, user
    memset(prm, 0, sizeof prm);
    *(uint32_t*)prm = sizeof prm;
    *(uint32_t*)(prm + 4) = (uint32_t)user;
    // An app that was just closed takes a moment to go away; until then a launch is refused.
    int u = -1;
    for (int tries = 0; tries < 12 && u < 0; tries++) {
        u = sceShellCoreUtilLaunchByUri(uri, prm);
        if (u < 0) usleep(250000);
    }
    logf_(0, "launcher: open %s -> 0x%x", uri, u);
    if (u >= 0) {
        // The deep link is handled asynchronously: wait for the app to appear.
        for (int i = 0; i < 40; i++) {
            int id = sceLncUtilGetAppId(tid);
            if (id > 0) return id;
            usleep(250000);
        }
        return (int)0x80940033;  // never started
    }
    // Fallback: start it directly.
    LncAppParam p;
    memset(&p, 0, sizeof p);
    p.size = sizeof p;
    p.user_id = (uint32_t)user;
    p.check_flag = 2;  // skip the system update check, as Itemzflow does
    int app = -1;
    for (int tries = 0; tries < 12 && app < 0; tries++) {
        app = sceLncUtilLaunchApp(tid, NULL, &p);
        if (app < 0) usleep(250000);
    }
    return app;
}

static int run(void) {
    int rc = sceLncUtilInitialize();
    int user = -1;
    sceUserServiceGetForegroundUser(&user);

    char target[32], caller[32] = {0};
    const int fromNexus = slot(g_target, target);
    if (!fromNexus) strcpy(target, TITLE_ID);

    if (fromNexus) {
        if (slot(g_close, caller)) closeApp(caller);
        else caller[0] = 0;
    } else {
        int running = sceLncUtilGetAppId(target);
        closeApp(target);
#ifdef KILL_ONLY
        if (running <= 0) logf_(1, "launcher: %s not running", target);
        return 0;
#endif
        if (running > 0) sleep(3);
    }

    int app = launchTitle(target, user);
    logf_(!fromNexus, "launcher: init=0x%x user=0x%x launch %s -> 0x%x", rc, user, target, app);
    if (fromNexus && app < 0) {
        // The title did not start (some entries cannot be launched). Bring Nexus back rather
        // than leaving the user at the home screen.
        logf_(1, "Could not start %s (error 0x%08X)", target, (unsigned)app);
        if (caller[0]) launchTitle(caller, user);
    }
    return 0;
}

int main(void) {
    sceUserServiceInitialize(NULL);
    int r = run();
    // Shut the user service down before exiting: left running, its event thread crashes as
    // the payload process goes away.
    sceUserServiceTerminate();
    return r;
}
