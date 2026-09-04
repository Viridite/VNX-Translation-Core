#include "compat/loader.h"
#include "compat/bootfade.h"
#include "compat/toast.h"
#include "compat/orientation.h"

// android:screenOrientation for the game being launched. Set by the caller
// before launchApk, because the manifest is parsed up in the UI layer where
// the ApkInfo lives and re-reading the APK here just to get one integer would
// be wasteful.
static int g_screen_orient = -1;
void loaderSetScreenOrient(int v) { g_screen_orient = v; }
#include "compat/android.h"
#include "compat/sha256.h"
#include "compat/gamedb.h"
#include "compat/obb.h"
#include "compat/jnisym.h"
#include "compat/games.h"
#include "build_number.h"
#include "unity/unity_runtime.h"   // VNX-Unity-Runtime submodule
#include "arm32/arm32.h"           // ARM32 emulation layer (armeabi-v7a games)
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <GLES2/gl2.h>
#include <minizip/unzip.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <strings.h>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <setjmp.h>
#include <stdarg.h>

// ─── Fake Android TLS block ───────────────────────────────────────────────────
// Switch homebrews have TPIDR_EL0=0 (no .tls section in the NRO).  NDK-compiled
// code reads Bionic's thread-local state directly via `mrs xN, tpidr_el0` then
// `ldr x8, [xN, #0x28]` — no pthread shim is involved.  We install a zeroed fake
// TLS block before any game code runs so these accesses land in valid mapped memory.
alignas(16) static uint8_t g_android_tls[512];      // main TLS block
alignas(16) static uint8_t g_android_tls_sub[512];  // sub-buffer for tls[0x28]

// Crash recovery shared with elf_loader.cpp and shim_table.cpp
extern jmp_buf        g_recover_jmp;
extern volatile bool     g_in_recover;
extern volatile void*    g_recover_owner;
extern volatile int      g_recover_sig;
extern volatile uint32_t g_recover_esr;
extern volatile uint64_t g_recover_pc;
extern volatile uint64_t g_recover_far;
extern volatile uint64_t g_recover_lr;
extern volatile uint64_t g_recover_x0;
extern volatile uint64_t g_recover_x8;

// Persistent path storage — launchApk stores these so ANativeActivity pointers
// remain valid after launchApk returns (for runGameOnMainThread).
static std::string g_base_dir_stored;
static std::string g_obb_dir_stored;
static std::string g_apk_path_stored;
static std::string g_pkg_name_stored;

// Per-APK framerate cap (0 = uncapped/default), set by the launcher's Manage
// overlay and read here once at launch — see readFpsCap() and its use in
// runGameOnMainThread's game loop below.
static int g_fps_cap_stored = 0;

// The running game's LoadedSo — lets jni_env.cpp invoke the game's registered
// Java_ native callbacks (async replies the Java side would normally deliver,
// e.g. returnCountryCode after fetchCountryCode).
static LoadedSo* g_game_so = nullptr;
void* compatFindGameSym(const char* name) {
    return g_game_so ? g_game_so->findSym(name) : nullptr;
}

// ─── Logging ──────────────────────────────────────────────────────────────────
static FILE*   g_compat_log   = nullptr;
static uint64_t g_log_start_t = 0;   // armGetSystemTick() at launch start

// Game threads are real now (pt_create → libnx Thread), so log calls arrive
// concurrently — serialize the dedup state and FILE* behind a mutex.
static Mutex g_log_lock;

// Dedup state: collapse consecutive identical lines into "msg x<N>"
static char g_log_last[512] = {};
static int  g_log_repeat    = 0;

// ─── Detail ring buffer (every compatLog line, unthrottled) ───────────────────
// Read by the main/render thread to display live log output without file I/O.
#define DETLOG_N  28
#define DETLOG_W  164
char g_detail_log[DETLOG_N][DETLOG_W] = {};
int  g_detail_head = 0;

static void detailPush(const char* line) {
    // Prepend "[Xs] " timestamp using ticks since launch start
    uint64_t ticks = armGetSystemTick() - g_log_start_t;
    uint32_t secs  = (uint32_t)(ticks / armGetSystemTickFreq());
    char entry[DETLOG_W];
    snprintf(entry, sizeof(entry), "[%3us] %s", secs, line);
    int i = g_detail_head % DETLOG_N;
    memcpy(g_detail_log[i], entry, DETLOG_W);
    g_detail_log[i][DETLOG_W - 1] = '\0';
    ++g_detail_head;
}

static void logFlushDedup() {
    if (g_log_repeat == 0) return;
    char linebuf[560];
    if (g_log_repeat == 1) {
        snprintf(linebuf, sizeof(linebuf), "%s", g_log_last);
    } else {
        snprintf(linebuf, sizeof(linebuf), "%s  x%d", g_log_last, g_log_repeat);
    }
    if (g_compat_log) {
        // Write timestamp + line to file too
        uint64_t ticks = armGetSystemTick() - g_log_start_t;
        uint32_t secs  = (uint32_t)(ticks / armGetSystemTickFreq());
        fprintf(g_compat_log, "[%3us] %s\n", secs, linebuf);
        fflush(g_compat_log);
    }
    detailPush(linebuf);
    g_log_repeat = 0;
}

// Lock-free emergency logger for crash-forensics call sites (fault handler,
// game-loop FAULT branch). If the thread that's now crashing faulted WHILE
// holding g_log_lock (e.g. mid-format inside a normal compatLogFmt call
// elsewhere), the ordinary path would deadlock forever — the process just
// hangs with nothing on disk and no further sign of life until force-quit.
// This bypasses the mutex entirely (accepting a small risk of interleaved
// output with a concurrent normal logger, which is fine: we're crashing).
void compatLogRaw(const char* msg) {
    if (g_compat_log) {
        uint64_t ticks = armGetSystemTick() - g_log_start_t;
        uint32_t secs  = (uint32_t)(ticks / armGetSystemTickFreq());
        fprintf(g_compat_log, "[%3us] %s\n", secs, msg);
        fflush(g_compat_log);
    }
    detailPush(msg);
}

// Force the current pending log message to disk and detail buffer immediately,
// without waiting for the next different message to trigger the dedup flush.
void compatLogFlush() {
    mutexLock(&g_log_lock);
    logFlushDedup();
    mutexUnlock(&g_log_lock);
}

void compatLog(const char* msg) {
    mutexLock(&g_log_lock);
    if (g_log_repeat > 0 && strcmp(msg, g_log_last) == 0) {
        g_log_repeat++;
        mutexUnlock(&g_log_lock);
        return;
    }
    logFlushDedup();
    snprintf(g_log_last, sizeof(g_log_last), "%s", msg);
    g_log_repeat = 1;
    mutexUnlock(&g_log_lock);
}

void compatLogFmt(const char* fmt, ...) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    compatLog(buf);
}

// Logged first, before anything else touches compat_log.txt — a compat
// report reviewer needs to be able to confirm the log they're reading
// actually came from the Viridite build (and CFW/firmware) the
// submitter claims, not a stale or mismatched one. Firmware comes from the
// "set" service (works under any CFW); Atmosphere's version specifically
// is only resolvable via the Exosphere API version SPL config item, which
// only succeeds when actually running under Atmosphere — a failure there
// just means "not Atmosphere, or couldn't tell," not a real error, so it's
// logged as unknown rather than treated as a fault.
static void logEnvironmentHeader() {
#ifndef VIRIDITE_VERSION
#define VIRIDITE_VERSION "dev"
#endif
    compatLogFmt("env: Viridite (Translation Core) %s (build 0.1.%d)", VIRIDITE_VERSION, BUILD_NUMBER);

    Result rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            compatLogFmt("env: Switch firmware %s", fw.display_version);
        else
            compatLog("env: Switch firmware unknown (setsysGetFirmwareVersion failed)");
        setsysExit();
    } else {
        compatLog("env: Switch firmware unknown (set service unavailable)");
    }

    rc = splInitialize();
    if (R_SUCCEEDED(rc)) {
        // libnx's own SplConfigItem enum only covers stock Nintendo SPL
        // config items (see switch/services/spl.h) — it doesn't name this
        // one because it's an Atmosphere-specific private extension to the
        // same API, not part of the real SPL service. 65000 is the value
        // Atmosphere itself (and other homebrew that reads its version,
        // e.g. hbmenu) uses; there's no official libnx constant for it.
        const SplConfigItem SplConfigItem_ExosphereApiVersion = (SplConfigItem)65000;
        u64 raw = 0;
        if (R_SUCCEEDED(splGetConfig(SplConfigItem_ExosphereApiVersion, &raw))) {
            u32 major = (raw >> 56) & 0xFF, minor = (raw >> 48) & 0xFF, micro = (raw >> 40) & 0xFF;
            compatLogFmt("env: Atmosphere %u.%u.%u", major, minor, micro);
        } else {
            compatLog("env: Atmosphere version unknown (not Atmosphere, or API unavailable)");
        }
        splExit();
    } else {
        compatLog("env: Atmosphere version unknown (spl service unavailable)");
    }
}

static void androidTlsInstall() {
    uint64_t old_tp;
    asm volatile("mrs %0, tpidr_el0" : "=r"(old_tp));
    compatLogFmt("AndroidTLS: old TPIDR_EL0=0x%llx — installing fake TLS @%p",
                 (unsigned long long)old_tp, (void*)g_android_tls);
    *(void**)(g_android_tls + 0x00) = g_android_tls;   // TLS_SLOT_SELF
    *(void**)(g_android_tls + 0x28) = g_android_tls_sub; // slot 5: EH/locale state
    uint64_t new_tp = (uint64_t)g_android_tls;
    asm volatile("msr tpidr_el0, %0" :: "r"(new_tp) : "memory");
}

// Same fake-TLS layout, but a private heap block for a game worker thread —
// called by the pthread_create trampoline in shim_table.cpp so each real
// thread gets its own Bionic per-thread state (errno slot, EH/locale slot).
// The block intentionally leaks: game threads are few and effectively live
// for the whole session.
void androidTlsInstallThread() {
    uint8_t* blk = (uint8_t*)calloc(1, 1024);
    if (!blk) return;
    *(void**)(blk + 0x00) = blk;          // TLS_SLOT_SELF
    *(void**)(blk + 0x28) = blk + 512;    // slot 5: EH/locale state
    asm volatile("msr tpidr_el0, %0" :: "r"((uint64_t)blk) : "memory");
}

// ─── UI ring buffer ───────────────────────────────────────────────────────────
// Last UILOG_N short messages, shown as a rolling sub-step log in showProgress.
#define UILOG_N  20
#define UILOG_W  128
char g_ui_log[UILOG_N][UILOG_W] = {};
int  g_ui_head = 0;   // next write index (not wrapped)
int  g_ui_pct  = 0;   // progress bar percentage 0-100

void compatUiLog(const char* msg) {
    if (!msg) return;
    int i = g_ui_head % UILOG_N;
    strncpy(g_ui_log[i], msg, UILOG_W - 1);
    g_ui_log[i][UILOG_W - 1] = '\0';
    ++g_ui_head;
}
void compatUiSetPct(int pct) {
    g_ui_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

// ─── CompatLayer singleton ────────────────────────────────────────────────────
static CompatLayer g_compat = {};

CompatLayer* compatGet() { return &g_compat; }

// ─── Filesystem helpers ───────────────────────────────────────────────────────
static void mkdirp(const std::string& path) {
    std::string p;
    for (char c : path) {
        p += c;
        if (c == '/') mkdir(p.c_str(), 0777);
    }
    mkdir(path.c_str(), 0777);
}

// Recursively delete a directory (used to clear a stale install when a different
// build of the same package is launched).
static void rmTree(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) { remove(path.c_str()); return; }
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string child = path + "/" + n;
        struct stat st;
        if (stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) rmTree(child);
        else remove(child.c_str());
    }
    closedir(d);
    rmdir(path.c_str());
}

// Extract a single ZIP entry to a file path
static bool extractEntry(unzFile zf, const std::string& dest) {
    unz_file_info fi;
    if (unzGetCurrentFileInfo(zf, &fi, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK)
        return false;
    if (unzOpenCurrentFile(zf) != UNZ_OK) return false;

    FILE* f = fopen(dest.c_str(), "wb");
    if (!f) { unzCloseCurrentFile(zf); return false; }

    char buf[65536];
    int n;
    while ((n = unzReadCurrentFile(zf, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    unzCloseCurrentFile(zf);
    return n == 0;
}

// ─── APK extraction ───────────────────────────────────────────────────────────
static bool extractApk(const std::string& apk_path, const std::string& dest_dir,
                       ProgressCb cb) {
    unzFile zf = unzOpen(apk_path.c_str());
    if (!zf) { compatLogFmt("extract: cannot open %s", apk_path.c_str()); return false; }

    mkdirp(dest_dir + "/lib/");
    mkdirp(dest_dir + "/assets/");

    unz_global_info gi;
    if (unzGetGlobalInfo(zf, &gi) != UNZ_OK) { unzClose(zf); return false; }

    char name[1024];
    int extracted = 0;
    for (uLong i = 0; i < gi.number_entry; i++) {
        unz_file_info fi;
        if (unzGetCurrentFileInfo(zf, &fi, name, sizeof(name),
                                  nullptr, 0, nullptr, 0) != UNZ_OK) break;

        std::string n = name;
        // Periodic on-disk progress so a big asset extraction never looks frozen.
        if ((++extracted & 0x1FF) == 0) {
            compatLogFmt("extract: %d/%lu files...", extracted, (unsigned long)gi.number_entry);
            compatLogFlush();
            if (cb) { char b[64]; snprintf(b, sizeof(b), "%d files...", extracted); cb("Extracting", b); }
        }

        if (n.rfind("lib/arm64-v8a/", 0) == 0 && n.size() > 14 &&
            n.back() != '/') {
            std::string rel  = n.substr(14);
            std::string dest = dest_dir + "/lib/" + rel;
            compatLogFmt("extract lib: %s", rel.c_str());
            if (cb) cb("Extracting APK", rel.c_str());
            extractEntry(zf, dest);

        } else if (n.rfind("lib/armeabi-v7a/", 0) == 0 && n.size() > 16 &&
                   n.back() != '/') {
            // 32-bit libs go to lib32/ — used only when there's no arm64 build,
            // in which case they run under the ARM32 emulation layer.
            mkdirp(dest_dir + "/lib32/");
            std::string rel  = n.substr(16);
            std::string dest = dest_dir + "/lib32/" + rel;
            compatLogFmt("extract lib32: %s", rel.c_str());
            extractEntry(zf, dest);

        } else if (n.rfind("assets/", 0) == 0 && n.back() != '/') {
            std::string rel  = n.substr(7);
            std::string dest = dest_dir + "/assets/" + rel;
            size_t p = 0;
            while ((p = rel.find('/', p)) != std::string::npos) {
                mkdirp(dest_dir + "/assets/" + rel.substr(0, p));
                p++;
            }
            extractEntry(zf, dest);

        } else if (n.rfind("res/", 0) == 0 && n.back() != '/') {
            // res/ used to be dropped on the floor, on the reasoning that an
            // NDK game's data lives in assets/. That holds for Hill Climb
            // Racing and not for others: Cut the Rope keeps art there, the
            // Square Enix titles keep their intro movies in res/raw, and
            // Swordigo keeps its music there. It is small next to assets/ —
            // Android resources, not game data — so extracting it costs
            // little and stops those games looking for files that were thrown
            // away during install.
            std::string rel  = n.substr(4);
            std::string dest = dest_dir + "/res/" + rel;
            mkdirp(dest_dir + "/res/");
            size_t p = 0;
            while ((p = rel.find('/', p)) != std::string::npos) {
                mkdirp(dest_dir + "/res/" + rel.substr(0, p));
                p++;
            }
            extractEntry(zf, dest);
        }

        if (i + 1 < gi.number_entry && unzGoToNextFile(zf) != UNZ_OK) break;
    }

    unzClose(zf);
    compatLog("extract: done");
    return true;
}

// ─── Per-game asset patches ───────────────────────────────────────────────────
// Some games ship in-app reference images for specific hardware (Hill Climb
// Racing bundles MOGA Bluetooth-controller button-layout guides — see
// moga_pro_guide/moga_pocket_guide save keys). Swap them for Switch
// controller-layout equivalents bundled in romfs, so if/when real controller
// support lands, the game's own onboarding art already shows Switch buttons
// instead of a MOGA pad nobody reading this owns. Applied after every
// extraction (fresh or cached) so it's always current even if the patch
// image itself changes across builds, and resized to match whatever
// dimensions the ORIGINAL file actually has (read from the file we're about
// to replace) rather than a hardcoded guess, since the exact original
// dimensions aren't known ahead of time.
// Which bundled controller diagram to patch in, from the launcher's
// .launch_input marker. Falls back to the Pro Controller image, which is the
// closest thing to a generic pad, when the marker is missing or unrecognised.
// Diagram + layout for whatever the launcher said is in the player's hands.
static GuideController guideControllerForInput() {
    char buf[32] = {0};
    FILE* f = fopen("sdmc:/Viridite/.launch_input", "r");
    if (f) { if (!fgets(buf, sizeof buf, f)) buf[0] = 0; fclose(f); }
    for (char* p = buf; *p; p++) if (*p == '\n' || *p == '\r') { *p = 0; break; }
    if (!strcmp(buf, "handheld"))     return GuideController::Handheld;
    if (!strcmp(buf, "joycon_dual"))  return GuideController::JoyDual;
    if (!strcmp(buf, "joycon_left"))  return GuideController::JoyLeft;
    if (!strcmp(buf, "joycon_right")) return GuideController::JoyRight;
    return GuideController::Pro;
}

// What each control does in Hill Climb Racing. Read out of libgame.so rather
// than assumed: the touch handler writes the same two bytes the shoulders do —
// obj+1264 from the RIGHT of the screen and obj+1265 from the LEFT — and HCR
// puts its gas pedal on the right, which is what pins R to gas and L to brake.
// C, Z and both stick clicks are dropped by the game's own handler, so there's
// nothing to label there.
static const GuideLabel kHillClimbLabels[] = {
    {GuideButton::R,    "Accelerate"},
    {GuideButton::L,    "Brake / reverse"},
    {GuideButton::Face, "Menus"},
    {GuideButton::Plus, "Pause"},
};

static const char* guideForInput() {
    char buf[32] = {0};
    FILE* f = fopen("sdmc:/Viridite/.launch_input", "r");
    if (f) { if (!fgets(buf, sizeof buf, f)) buf[0] = 0; fclose(f); }
    for (char* p = buf; *p; p++) if (*p == '\n' || *p == '\r') { *p = 0; break; }

    if (!strcmp(buf, "handheld"))     return "romfs:/patches/controllers/ctrl_handheld.png";
    if (!strcmp(buf, "joycon_dual"))  return "romfs:/patches/controllers/ctrl_joycon_dual.png";
    if (!strcmp(buf, "joycon_left"))  return "romfs:/patches/controllers/ctrl_joycon_left.png";
    if (!strcmp(buf, "joycon_right")) return "romfs:/patches/controllers/ctrl_joycon_right.png";
    return "romfs:/patches/controllers/ctrl_pro.png";
}

struct AssetPatch { const char* filename; const char* romfsSrc; };
static const AssetPatch kHillClimbPatches[] = {
    // Both guide slots get the SAME image — the one matching whatever
    // controller was picked in the launcher. HCR decides between its Pro and
    // Pocket guides by MOGA controller type, which has no bearing on what's
    // actually attached to a Switch, so showing the same correct diagram for
    // either is better than letting it pick a pad you aren't holding.
    {"Moga_Pro_Guide.png",    nullptr},   // resolved at runtime, see guideForInput
    {"Moga_Pocket_Guide.png", nullptr},
};

// Recursively collect EVERY file under dir matching `filename`
// (case-insensitive). Cocos2d-x games routinely ship the same asset at several
// resolutions — assets/hd/, assets/sd/, and so on — so stopping at the first
// match patched one copy and left the game free to load a different one. That
// is consistent with the controller guide still showing a MOGA pad while the
// log reported the replacement written.
static void listFilesRecursive(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string nm = ent->d_name;
        if (nm == "." || nm == "..") continue;
        std::string path = dir + "/" + nm;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) listFilesRecursive(path, out);
        else                     out.push_back(path);
    }
    closedir(d);
}

static void findAllRecursive(const std::string& dir, const std::string& filename,
                             std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string nm = ent->d_name;
        if (nm == "." || nm == "..") continue;
        std::string path = dir + "/" + nm;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) findAllRecursive(path, filename, out);
        else if (strcasecmp(nm.c_str(), filename.c_str()) == 0) out.push_back(path);
    }
    closedir(d);
}

static void applyGamePatches(const std::string& pkg_name, const std::string& asset_dir) {
    if (pkg_name != "com.fingersoft.hillclimb") return;
    romfsInit();
    const char* guideSrc = guideForInput();
    compatLogFmt("patch: controller guide -> %s", guideSrc);

    // Every controller-help-looking asset the game actually ships, listed once.
    // Which file HCR puts on screen has been guesswork twice now — the log said
    // the replacement was written and a MOGA pad still appeared — and that is
    // only answerable by seeing what candidates exist. Names are cheap; another
    // hardware round trip is not.
    {
        static const char* kNeedles[] = {"moga", "guide", "controller", "gamepad"};
        std::vector<std::string> all;
        listFilesRecursive(asset_dir, all);
        int shown = 0;
        for (const std::string& f : all) {
            std::string lower = f;
            for (char& c : lower) c = (char)tolower((unsigned char)c);
            for (const char* n : kNeedles) {
                if (lower.find(n) != std::string::npos) {
                    compatLogFmt("patch: candidate asset %s", f.c_str());
                    if (++shown >= 24) { compatLog("patch: (candidate list truncated)"); }
                    break;
                }
            }
            if (shown >= 24) break;
        }
        if (!shown) compatLog("patch: no controller-guide-looking assets found at all");
    }
    for (const AssetPatch& patch : kHillClimbPatches) {
        std::vector<std::string> targets;
        findAllRecursive(asset_dir, patch.filename, targets);
        if (targets.empty()) {
            compatLogFmt("patch: %s not found under assets — skipped", patch.filename);
            continue;
        }
        if (targets.size() > 1)
            compatLogFmt("patch: %s exists in %zu places — replacing all of them",
                         patch.filename, targets.size());
        for (const std::string& target : targets) {
        SDL_Surface* orig = IMG_Load(target.c_str());
        if (!orig) {
            compatLogFmt("patch: couldn't read original %s (%s) — skipped",
                         target.c_str(), IMG_GetError());
            continue;
        }
        int origW = orig->w, origH = orig->h;
        SDL_FreeSurface(orig);

        SDL_Surface* repl = IMG_Load((patch.romfsSrc ? patch.romfsSrc : guideSrc));
        if (!repl) {
            compatLogFmt("patch: bundled replacement %s missing (%s) — skipped",
                         (patch.romfsSrc ? patch.romfsSrc : guideSrc), IMG_GetError());
            continue;
        }
        SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(
            0, origW, origH, 32, SDL_PIXELFORMAT_ABGR8888);
        if (scaled) {
            SDL_SetSurfaceBlendMode(repl, SDL_BLENDMODE_NONE);  // scale RGBA as-is, no alpha compositing
            SDL_BlitScaled(repl, nullptr, scaled, nullptr);
            // Annotate AFTER the rescale so the text is rendered at final size
            // and stays crisp, rather than being scaled along with the artwork.
            guideDrawLabels(scaled, guideControllerForInput(),
                            kHillClimbLabels,
                            (int)(sizeof kHillClimbLabels / sizeof *kHillClimbLabels));
            if (IMG_SavePNG(scaled, target.c_str()) == 0)
                compatLogFmt("patch: replaced %s (%dx%d, Switch controller layout)",
                             target.c_str(), origW, origH);
            else
                compatLogFmt("patch: failed to write %s (%s)", target.c_str(), IMG_GetError());
            SDL_FreeSurface(scaled);
        }
        SDL_FreeSurface(repl);
        }
    }
}

// ─── Find all .so files ───────────────────────────────────────────────────────
// Returns {size, path} pairs sorted smallest-first so dependency libs load
// before the main game lib (which is typically the largest).
static std::vector<std::pair<size_t, std::string>> findAllSos(const std::string& lib_dir) {
    DIR* d = opendir(lib_dir.c_str());
    if (!d) return {};

    std::vector<std::pair<size_t, std::string>> sos;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string nm = ent->d_name;
        if (nm.size() < 4 || nm.compare(nm.size()-3, 3, ".so") != 0) continue;
        std::string path = lib_dir + "/" + nm;
        struct stat st;
        if (stat(path.c_str(), &st) == 0)
            sos.push_back({(size_t)st.st_size, path});
    }
    closedir(d);

    // Smallest first: dependency/helper libs before the main game lib
    std::sort(sos.begin(), sos.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });

    return sos;
}

// ─── EGL / window setup ───────────────────────────────────────────────────────
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLContext g_egl_context  = EGL_NO_CONTEXT;
static EGLSurface g_egl_surface  = EGL_NO_SURFACE;

// Hand the display window over to Unity. Unlike cocos2d-x (which renders
// through the Core's SDL2 EGL context), Unity's nativeRecreateGfxState creates
// its OWN EGL surface + context on the ANativeWindow. Switch/nvn EGL allows a
// single window surface per nwindow, so the Core's SDL2 surface has to be
// released first or Unity's eglCreateWindowSurface fails with EGL_BAD_ALLOC.
// Releases current + destroys the Core's surface, leaving the nwindow free;
// the SDL2 context object is left alive (harmless, unused from here). Returns
// the shared EGLDisplay so the Unity path can query/flush if needed.
void* compatUnityReleaseWindow() {
    if (g_egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(g_egl_display, g_egl_surface);
            g_egl_surface = EGL_NO_SURFACE;
        }
        compatLog("unity: released Core SDL2 EGL surface — nwindow free for Unity's own context");
    }
    return (void*)g_egl_display;
}

static bool setupEGL(ANativeWindow* win) {
    g_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl_display == EGL_NO_DISPLAY) {
        compatLog("EGL: no display");
        return false;
    }
    EGLint major, minor;
    if (!eglInitialize(g_egl_display, &major, &minor)) {
        compatLog("EGL: init failed");
        return false;
    }
    compatLogFmt("EGL: version %d.%d", major, minor);

    eglBindAPI(EGL_OPENGL_ES_API);

    static const EGLint cfg_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(g_egl_display, cfg_attribs, &cfg, 1, &ncfg) || ncfg == 0) {
        compatLog("EGL: no matching config");
        return false;
    }

    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    g_egl_context = eglCreateContext(g_egl_display, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (g_egl_context == EGL_NO_CONTEXT) {
        compatLog("EGL: create context failed");
        return false;
    }

    g_egl_surface = eglCreateWindowSurface(g_egl_display, cfg,
                                           (EGLNativeWindowType)win->nwin, nullptr);
    if (g_egl_surface == EGL_NO_SURFACE) {
        compatLog("EGL: create window surface failed");
        return false;
    }

    if (!eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) {
        compatLog("EGL: makeCurrent failed");
        return false;
    }

    compatLog("EGL: setup OK");
    return true;
}

// ─── Per-APK framerate cap ─────────────────────────────────────────────────────
// Written by the launcher's Manage overlay as a plain integer in
// games/<pkg>/.fps_cap (same simple-marker-file convention as .installed and
// .apk_sha256). Missing file (never set, or explicitly cleared) means
// uncapped/default — the game runs exactly as it did before this existed.
static int readFpsCap(const std::string& base_dir) {
    FILE* f = fopen((base_dir + "/.fps_cap").c_str(), "r");
    if (!f) return 0;
    int fps = 0;
    if (fscanf(f, "%d", &fps) != 1) fps = 0;
    fclose(f);
    return (fps > 0 && fps <= 60) ? fps : 0;
}

// ─── APK integrity hash ────────────────────────────────────────────────────────
// Hashing the whole raw APK is only cheap once — cache the result next to the
// .installed marker so every subsequent launch (which skips extraction) reads
// a 64-byte file instead of re-hashing a potentially 100+MB APK. The website's
// compat-submission pipeline hashes the exact same raw APK bytes when a report
// is submitted, so this lets a reviewer confirm the attached logs actually came
// from the APK the report claims (see compat-reports' meta.json apk_sha256).
static std::string apkSha256Cached(const std::string& apk_path, const std::string& base_dir) {
    std::string cache_path = base_dir + "/.apk_sha256";
    FILE* cf = fopen(cache_path.c_str(), "rb");
    if (cf) {
        char buf[65] = {0};
        size_t n = fread(buf, 1, 64, cf);
        fclose(cf);
        if (n == 64) return std::string(buf, 64);
    }
    std::string hash = sha256File(apk_path);
    if (!hash.empty()) {
        FILE* wf = fopen(cache_path.c_str(), "wb");
        if (wf) { fwrite(hash.data(), 1, hash.size(), wf); fclose(wf); }
    }
    return hash;
}

// ─── XAPK support ─────────────────────────────────────────────────────────────
// A .xapk is a plain ZIP whose members are the base APK plus config split APKs
// (config.arm64_v8a.apk for the 64-bit libs, config.<lang>/<dpi> for resources)
// and, for Unity titles, Play Asset Delivery packs (UnityDataAssetPack.apk,
// AddressablesAssetPack.apk) that hold the game data. A real APK never contains
// .apk members, so their presence is what distinguishes a XAPK from an APK.
static bool zipContainsApks(const std::string& path) {
    unzFile zf = unzOpen(path.c_str());
    if (!zf) return false;
    bool found = false;
    unz_global_info gi;
    if (unzGetGlobalInfo(zf, &gi) == UNZ_OK) {
        char name[1024];
        for (uLong i = 0; i < gi.number_entry && !found; i++) {
            unz_file_info fi;
            if (unzGetCurrentFileInfo(zf, &fi, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK) break;
            std::string n = name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".apk") == 0) found = true;
            if (i + 1 < gi.number_entry && unzGoToNextFile(zf) != UNZ_OK) break;
        }
    }
    unzClose(zf);
    return found;
}
static bool isXapk(const std::string& path) {
    if (path.size() > 5 && path.compare(path.size() - 5, 5, ".xapk") == 0) return true;
    return zipContainsApks(path);
}

// Does this APK ship arm64 libraries? Asked so a cached install that has lost
// them can be told apart from a game that genuinely never had them. Without
// that distinction a 64-bit game silently runs on the 32-bit interpreter,
// which is slower, far less complete, and looks like a broken game rather than
// a broken install.
static bool apkHasArm64Libs(const std::string& path) {
    unzFile zf = unzOpen(path.c_str());
    if (!zf) return false;
    bool found = false;
    unz_global_info gi;
    if (unzGetGlobalInfo(zf, &gi) == UNZ_OK) {
        char name[1024];
        for (uLong i = 0; i < gi.number_entry && !found; i++) {
            unz_file_info fi;
            if (unzGetCurrentFileInfo(zf, &fi, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK) break;
            std::string n = name;
            if (n.rfind("lib/arm64-v8a/", 0) == 0 && n.size() > 14 &&
                n.size() > 3 && n.compare(n.size() - 3, 3, ".so") == 0)
                found = true;
            if (i + 1 < gi.number_entry && unzGoToNextFile(zf) != UNZ_OK) break;
        }
    }
    unzClose(zf);
    return found;
}

// Unpack every APK inside the XAPK through the normal extractApk path, merging
// their arm64 libs + assets into one game dir. arm32/x86 splits are skipped
// (nothing to contribute here); extractApk itself ignores non-arm64 libs, so
// this is just to avoid spilling a large split to a temp file for no reason.
static bool extractXapk(const std::string& xapk_path, const std::string& dest_dir, ProgressCb cb) {
    unzFile zf = unzOpen(xapk_path.c_str());
    if (!zf) { compatLogFmt("xapk: cannot open %s", xapk_path.c_str()); return false; }
    unz_global_info gi;
    if (unzGetGlobalInfo(zf, &gi) != UNZ_OK) { unzClose(zf); return false; }

    const std::string tmp = dest_dir + "/.split_tmp.apk";
    char name[1024];
    int done = 0; bool any = false;
    for (uLong i = 0; i < gi.number_entry; i++) {
        unz_file_info fi;
        if (unzGetCurrentFileInfo(zf, &fi, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK) break;
        std::string n = name;
        bool isApk = n.size() > 4 && n.compare(n.size() - 4, 4, ".apk") == 0 && n.back() != '/';
        if (isApk) {
            // Only the base APK, the arm64/armeabi lib splits, and Unity asset
            // packs carry anything we need. Skipping the per-language/-density
            // config splits (config.en/config.hdpi/…) and x86 avoids a pile of
            // pointless temp-file round-trips to the SD card.
            bool isConfig = n.find("config.") != std::string::npos;
            bool wanted = !isConfig
                        || n.find("config.arm64_v8a") != std::string::npos
                        || n.find("config.armeabi")   != std::string::npos;
            if (!wanted) {
                compatLogFmt("xapk: skip %s (resource split)", n.c_str());
            } else {
                compatLogFmt("xapk: unpack %s (%lu KB)...", n.c_str(), (unsigned long)(fi.uncompressed_size/1024));
                compatLogFlush();                      // so progress is on disk before the (slow) extract
                if (cb) cb("Installing XAPK", n.c_str());
                if (extractEntry(zf, tmp) && extractApk(tmp, dest_dir, cb)) { any = true; done++; }
                compatLogFmt("xapk: unpacked %s", n.c_str());
                compatLogFlush();
            }
        }
        if (i + 1 < gi.number_entry && unzGoToNextFile(zf) != UNZ_OK) break;
    }
    unzClose(zf);
    remove(tmp.c_str());
    compatLogFmt("xapk: done (%d apk(s) merged)", done);
    compatLogFlush();
    return any;
}

// ─── apkInstall ──────────────────────────────────────────────────────────────
bool apkInstall(const std::string& apk_path, const std::string& pkg_name, ProgressCb cb) {
    std::string base_dir  = std::string("sdmc:/Viridite/games/") + pkg_name;
    mkdirp(base_dir);
    mkdirp(base_dir + "/lib/");
    mkdirp(base_dir + "/assets/");

    bool xapk = isXapk(apk_path);
    if (cb) cb(xapk ? "Installing XAPK" : "Installing APK", "Extracting libs and assets...");
    compatLogFmt("apkInstall: %s -> %s (%s)", apk_path.c_str(), pkg_name.c_str(),
                 xapk ? "xapk" : "apk");
    bool ok = xapk ? extractXapk(apk_path, base_dir, cb)
                   : extractApk(apk_path, base_dir, cb);
    if (!ok) {
        compatLog("apkInstall: extraction failed");
        return false;
    }
    // Write .installed marker so subsequent launches can skip extraction
    std::string marker = base_dir + "/.installed";
    FILE* mf = fopen(marker.c_str(), "w");
    if (mf) { fputs(apk_path.c_str(), mf); fclose(mf); }
    compatLog("apkInstall: done — marker written");
    return true;
}

// Present a frame drawn by the ARM32 interpreter.
//
// The interpreter has no business knowing about EGL — it runs guest code and
// the guest never calls eglSwapBuffers itself (cocos2d-x expects the Java side
// to do it). Keeping the swap here means both the 64-bit and 32-bit paths
// present through exactly the same context and surface.
namespace a32 { void a32FrameSwap(void); }
void a32::a32FrameSwap(void) {
    // Whatever is current on this thread — nothing creates a context for the
    // 32-bit path, it borrows SDL's, and asking EGL directly avoids depending
    // on globals that only the 64-bit path fills in.
    EGLDisplay d = eglGetCurrentDisplay();
    EGLSurface s = eglGetCurrentSurface(EGL_DRAW);
    if (d == EGL_NO_DISPLAY || s == EGL_NO_SURFACE) return;
    // Only once a context is known to be current. GL entry points resolve
    // through a per-context dispatch table, so calling one with nothing bound
    // is not a no-op — it is a null dereference, and this runs on every frame.
    bootFadeDraw();                       // the tail of the Viridite reveal
    toast::draw();                        // achievement unlocks, over the game
    eglSwapBuffers(d, s);
}

// ─── launchApk ───────────────────────────────────────────────────────────────
LaunchResult launchApk(const std::string& apk_path, const std::string& pkg_name,
                       ProgressCb cb, bool already_installed) {
    LaunchResult result;

    std::string log_path = "sdmc:/Viridite/compat_log.txt";
    g_compat_log = fopen(log_path.c_str(), "w");
    g_log_start_t = armGetSystemTick();
    logEnvironmentHeader();
    compatLogFmt("launchApk: %s  pkg=%s  installed=%d",
                 apk_path.c_str(), pkg_name.c_str(), (int)already_installed);

    // Preflight: the whole JIT dual-mapping scheme (elf_loader.cpp's SplitMap)
    // rests on svcCreateCodeMemory (0x4B) and svcControlCodeMemory (0x4C)
    // actually being available — both are privileged syscalls that depend on
    // the CFW/environment we're launched under. Checking this up front (a
    // technique borrowed from max_nx, a similar Android-.so-on-Switch loader)
    // means a missing syscall fails here with a clear message instead of a
    // much more confusing failure partway through ELF loading.
    if (!envIsSyscallHinted(0x4B) || !envIsSyscallHinted(0x4C)) {
        compatLogFmt("launchApk: required syscalls unavailable (CreateCodeMemory hinted=%d, ControlCodeMemory hinted=%d)",
                     envIsSyscallHinted(0x4B), envIsSyscallHinted(0x4C));
        result.errorStage  = "Checking environment";
        result.errorDetail = "This CFW/environment doesn't allow JIT code memory "
                              "(svcCreateCodeMemory/svcControlCodeMemory unavailable) — "
                              "Viridite can't load game binaries here.";
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        return result;
    }

    // ── 1. Set up directories (always, mkdirp is idempotent) ─────────────────
    std::string base_dir  = std::string("sdmc:/Viridite/games/") + pkg_name;
    std::string lib_dir   = base_dir + "/lib";
    std::string asset_dir = base_dir + "/assets";
    mkdirp(base_dir);
    mkdirp(lib_dir);
    mkdirp(asset_dir);

    {
        std::string apkHash = apkSha256Cached(apk_path, base_dir);
        if (!apkHash.empty())
            compatLogFmt("launchApk: apk sha256=%s", apkHash.c_str());
        else
            compatLog("launchApk: apk sha256 unavailable (couldn't read the APK file)");
    }

    g_fps_cap_stored = readFpsCap(base_dir);
    compatLogFmt("launchApk: fps cap=%s",
                 g_fps_cap_stored > 0 ? std::to_string(g_fps_cap_stored).c_str() : "none (default)");

    // Two builds of one game share a package → share this dir. If the cached
    // install came from a different file, wipe it and re-extract so we don't run
    // stale libs of the wrong arch/version (e.g. arm64 1.67 vs arm32 1.70).
    if (already_installed) {
        std::string installedFrom;
        if (FILE* mf = fopen((base_dir + "/.installed").c_str(), "r")) {
            char b[512] = {0};
            if (fgets(b, sizeof(b), mf)) installedFrom = b;
            fclose(mf);
            while (!installedFrom.empty() && (installedFrom.back()=='\n' || installedFrom.back()=='\r'))
                installedFrom.pop_back();
        }
        const bool haveArm64 = !findAllSos(lib_dir).empty();
        const bool haveArm32 = !findAllSos(base_dir + "/lib32").empty();
        const bool noLibs    = !haveArm64 && !haveArm32;
        // An install holding only 32-bit libs for a game that ships 64-bit ones
        // is stale, not a 32-bit game. Two builds of Hill Climb Racing share
        // this package name — 1.67 has arm64, 1.70 is 32-bit only — so running
        // the second wipes lib/ and leaves lib32/ behind, and the first then
        // finds "some libs present" and launches on the ARM32 interpreter.
        // Checking only whether *both* directories were empty could never see
        // that: the game looked installed, just 64 bits lighter.
        const bool lostArm64 = !haveArm64 && apkHasArm64Libs(apk_path);
        if (installedFrom != apk_path || noLibs || lostArm64) {
            compatLogFmt("launchApk: re-extracting (%s)",
                         noLibs    ? "cached install has no arm64/arm32 libs — likely from an older build"
                       : lostArm64 ? "cached install has no arm64 libs but this APK ships them — "
                                     "left behind by a 32-bit build of the same package"
                                   : "cached install is from a different file");
            rmTree(lib_dir);
            rmTree(base_dir + "/lib32");
            rmTree(asset_dir);
            mkdirp(lib_dir); mkdirp(asset_dir);
            already_installed = false;
        }
    }

    // ── 2. Extract APK/XAPK (skipped when already installed) ─────────────────
    if (!already_installed) {
        bool xapk = isXapk(apk_path);
        compatUiLog(xapk ? "Extracting XAPK..." : "Extracting APK...");
        compatUiSetPct(2);
        if (cb) cb(xapk ? "Installing XAPK" : "Installing APK", "Extracting libs and assets...");
        compatLogFmt("Extracting %s...", xapk ? "XAPK" : "APK");
        bool ok = xapk ? extractXapk(apk_path, base_dir, cb)
                       : extractApk(apk_path, base_dir, cb);
        if (!ok) {
            compatLog("Extraction failed");
            result.errorStage  = "Extracting APK";
            result.errorDetail = "Could not open or read the APK file.";
            if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
            return result;
        }
        // Write .installed marker
        std::string marker = base_dir + "/.installed";
        FILE* mf = fopen(marker.c_str(), "w");
        if (mf) { fputs(apk_path.c_str(), mf); fclose(mf); }
        compatLog("APK installed — marker written");
    } else {
        compatUiLog("Using cached install (skip extract)");
        compatUiSetPct(12);
        if (cb) cb("Loading cached install", "Skipping APK extraction...");
        compatLog("Already installed — skipping extraction");
    }

    // ── 2b. Expansion files (OBB) ───────────────────────────────────────────
    // An APK is capped at 100MB on Google Play, so a large game keeps its data
    // in an OBB beside the APK. Copying only the .apk meant every such title
    // could do nothing but fail on its first asset read, with the data never
    // having been on the Switch at all — most of the Square Enix catalogue,
    // GTA: San Andreas, Castle of Illusion, After Burner Climax, KH Union χ.
    //
    // These are not unpacked, because Android does not unpack them: the game
    // is handed a path and reads the OBB itself. Unpacking would double the
    // space a 2GB title needs and hand games a layout they never expect.
    {
        const std::string obb_dir = obb::dirFor(base_dir);
        std::vector<obb::File> obbs = obb::find(apk_path, pkg_name);
        if (!obbs.empty()) {
            compatUiLog("Installing expansion files...");
            if (cb) cb("Installing expansion files", "Copying OBB data...");
            for (const obb::File& f : obbs)
                compatLogFmt("obb: found %s (%s)", f.canonicalName.c_str(), f.path.c_str());
            int n = obb::install(obbs, obb_dir);
            if (n < 0)
                compatLogFmt("obb: could not create %s — the game will not find its data",
                             obb_dir.c_str());
            else
                compatLogFmt("obb: %d of %zu in place at %s", n, obbs.size(), obb_dir.c_str());
        } else {
            compatLog("obb: none found beside the APK");
        }
        // Set regardless: a game that asks where its OBBs are should be told
        // the same directory whether or not anything was put there, and the
        // fopen shim needs it to resolve a hardcoded Android path.
        compatSetObbDir(obb_dir.c_str(), pkg_name.c_str());

        // A title whose data lives in an OBB beside the APK cannot start
        // without it — it will load, link, run its constructors and then fail
        // on a file read somewhere deep in its own asset code, which reads as
        // a Viridite bug rather than as a missing download. Say which file is
        // missing and where it goes, while that is still cheap to say.
        //
        // The deep test is exempt: it loads a game without running it, which
        // is exactly the case where the data legitimately isn't there yet.
        const gamedb::Title* t = gamedb::findByPackage(pkg_name.c_str());
        if (t && t->needsObb && obbs.empty() && !elfIsDryRun()) {
            compatLogFmt("obb: %s keeps its game data in an expansion file and none "
                         "was found — refusing to load into a certain failure", t->name);
            result.errorStage  = "Expansion file missing";
            result.errorDetail = std::string(t->name) + " needs its .obb expansion file "
                                 "next to the APK (in Android/obb/" + pkg_name +
                                 "/, a folder named for the package, or beside the APK "
                                 "itself). The APK alone does not contain the game data.";
            if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
            return result;
        }
    }

    // Applied every launch (fresh or cached) so it's always current.
    applyGamePatches(pkg_name, asset_dir);

    // ── 3. Find all .so files ────────────────────────────────────────────────
    compatUiLog("Scanning for .so files...");
    compatUiSetPct(12);
    if (cb) cb("Finding libraries", "Scanning extracted libs...");
    auto all_sos = findAllSos(lib_dir);
    if (all_sos.empty()) {
        // No arm64 build — fall back to the ARM32 emulation layer if the game
        // shipped armeabi-v7a libs (extracted to lib32/).
        auto sos32 = findAllSos(base_dir + "/lib32");
        if (!sos32.empty()) {
            const std::string& main32 = sos32.back().second;   // largest
            compatLogFmt("engine: no arm64 — ARM32 emulation layer for %s", main32.c_str());
            // Reaching here for an APK that ships arm64 means the extraction
            // did not produce lib/ — the game is being run on the interpreter
            // when it has a native build available. Say so plainly: silently
            // downgrading reads as "the game is broken" for the rest of the log.
            if (apkHasArm64Libs(apk_path))
                compatLog("engine: WARNING — this APK ships arm64 libs but none were "
                          "installed. The native path was skipped; that is an install "
                          "fault, not a 32-bit game.");

            // The dry run must stop here too. Constructors are skipped through
            // elfRunCtors on the 64-bit path, but a 32-bit game runs inside the
            // interpreter, which that flag never reaches — so without this a
            // "deep test" of a 32-bit game would start it for real.
            if (elfIsDryRun()) {
                compatLogFmt("arm32: dry run — %zu libraries found, not executed", sos32.size());
                for (auto& [sz, path] : sos32) {
                    size_t sl = path.rfind('/');
                    compatLogFmt("  arm32 lib: %s (%zu bytes)%s",
                                 sl == std::string::npos ? path.c_str() : path.c_str() + sl + 1,
                                 sz, a32::isElf32Arm(path.c_str()) ? "" : "  <-- NOT ELF32/ARM");
                }
                result.ok = true;
                result.game_so = (void*)1;   // "loaded", for the caller's verdict
                if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
                return result;
            }
            compatAudioSetAssetsDir(asset_dir.c_str());

            int rc = a32::run(main32.c_str(), pkg_name.c_str());
            compatLogFmt("arm32: load returned %d", rc);
            result.ok = (rc == 0);
            if (rc != 0) {
                result.errorStage  = "ARM32 emulation";
                result.errorDetail = "ARM32 layer stopped early — see compat_log.txt.";
            } else {
                // Loaded, not run. Rendering needs the main thread's GL
                // context, so it is marked ready and driven from there — the
                // same split the 64-bit path uses, and the reason creating a
                // second EGL surface here failed: SDL already owns the only one
                // the window allows.
                result.is_arm32 = true;
                result.game_so  = (void*)1;   // non-null: "there is a game to run"
            }
            // Leave the log open when the game is about to run.
            //
            // This branch used to be terminal — it ran the game to completion
            // and closed up — so it closed the log on the way out. Now that
            // rendering happens afterwards on the main thread, closing here
            // means every frame, every unimplemented instruction and every
            // halt in the game itself is written to a file that is not there.
            // The log ending at "load returned 0" was that, not the game
            // stopping.
            if (rc != 0 && g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
            return result;
        }
        compatLog("No arm64 or armeabi-v7a .so found in APK");
        result.errorStage  = "Finding libraries";
        result.errorDetail = "No arm64-v8a or armeabi-v7a .so found — unsupported architecture.";
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        return result;
    }
    bool sawIl2cpp = false, sawUnity = false;
    for (auto& [sz, path] : all_sos) {
        size_t sl = path.rfind('/');
        const char* nm = (sl != std::string::npos) ? path.c_str() + sl + 1 : path.c_str();
        compatLogFmt("findAllSos: %s (%zu bytes)", nm, sz);
        if (strcmp(nm, "libil2cpp.so") == 0) sawIl2cpp = true;
        if (strcmp(nm, "libunity.so")  == 0) sawUnity  = true;
    }

    // Engine detection. Unity (libunity.so + IL2CPP) is a completely different
    // native runtime from the cocos2d-x games this Core drives directly — it
    // boots through libmain.so's JNI_OnLoad → NativeLoader.load → dlopen of
    // libunity, then registers its game loop dynamically via RegisterNatives.
    // The Unity-specific bringup lives in the separate VNX-Unity-Runtime
    // module (see unityIsGame/unityRun); everything below stays the cocos2d-x
    // path. Runtime deps that Unity dlopen's on demand need a real dlopen —
    // point it at this game's lib dir.
    elfSetDlopenDir(lib_dir.c_str());
    const bool isUnity = sawUnity || sawIl2cpp;
    if (isUnity)
        compatLog("engine: Unity IL2CPP detected (libunity.so/libil2cpp.so) — "
                  "routing to VNX-Unity-Runtime bringup");
    else
        compatLog("engine: cocos2d-x / generic NDK path");

    // What is known about this title, if anything. Nothing here changes whether
    // a game runs — it decides which library gets entered, and it puts what is
    // known into the log before the load rather than leaving a failure to be
    // read backwards from a fault address.
    const gamedb::Title* title = gamedb::findByPackage(pkg_name.c_str());
    if (title) {
        compatLogFmt("title: %s (%s) — %s, %s",
                     title->name, title->version,
                     gamedb::engineName(title->engine),
                     gamedb::supportTag(title->support));
        if (title->extraData)
            compatLogFmt("title: needs beyond the APK: %s", title->extraData);
        if (title->support == gamedb::Support::Unsupported)
            compatLogFmt("title: %s is not a shape this Core can drive — expect "
                         "this load to stop early", gamedb::engineName(title->engine));
    } else {
        compatLogFmt("title: no database entry for %s — loading on the generic path",
                     pkg_name.empty() ? "(unknown package)" : pkg_name.c_str());
    }

    // ── Which library is the game ───────────────────────────────────────────
    // "The largest .so" is right for a game shipping one big binary and a few
    // small helpers, and wrong for plenty of others: a Unity game's largest
    // library is libil2cpp.so while the entry point is libmain.so, and a game
    // shipping FMOD Studio can easily have its audio backend outweigh it. The
    // rules, and which one answered, are in compat/gamedb.h — host-tested in
    // test/gamedb/harness.cpp, because this is exactly the decision that cannot
    // be checked on hardware without owning every APK in the table.
    std::vector<std::string> so_names(all_sos.size());   // basenames, parallel to all_sos
    std::vector<const char*> so_name_ptrs(all_sos.size());
    std::vector<size_t>      so_sizes(all_sos.size());
    for (size_t i = 0; i < all_sos.size(); i++) {
        const std::string& path = all_sos[i].second;
        size_t sl = path.rfind('/');
        so_names[i]     = (sl == std::string::npos) ? path : path.substr(sl + 1);
        so_name_ptrs[i] = so_names[i].c_str();
        so_sizes[i]     = all_sos[i].first;
    }
    const char* entry_why = nullptr;
    int entry_idx = gamedb::chooseEntrySo(pkg_name.c_str(), so_name_ptrs.data(),
                                          so_sizes.data(), so_name_ptrs.size(),
                                          &entry_why);
    if (entry_idx < 0) entry_idx = (int)all_sos.size() - 1;   // unreachable: the set is non-empty
    const std::string& main_so = all_sos[entry_idx].second;
    compatLogFmt("engine: entering %s — %s", so_names[entry_idx].c_str(),
                 entry_why ? entry_why : "largest library");

    // ── 4. Set up JNI ───────────────────────────────────────────────────────
    compatUiLog("Setting up JNI stubs...");
    compatUiSetPct(15);
    if (cb) cb("Setting up JNI", "Preparing Android runtime environment...");
    compatLog("Setting up JNI environment...");
    jniSetup(&g_compat);

    // ── 5. Load all ELF libraries (smallest-first = deps before main game) ──
    // All SOs are loaded BEFORE any constructors run, so cross-library symbols
    // are available during constructor calls.
    elfResetCounts();
    std::vector<LoadedSo*> loaded;
    LoadedSo* so = nullptr;  // the main (largest) SO
    int so_idx = 0;
    int so_total = (int)all_sos.size();
    for (auto& [sz, so_path] : all_sos) {
        size_t sl = so_path.rfind('/');
        const char* soName = (sl != std::string::npos)
                             ? so_path.c_str() + sl + 1 : so_path.c_str();
        {
            char ub[80];
            snprintf(ub, sizeof(ub), "Loading %s...", soName);
            compatUiLog(ub);
        }
        int pct = 18 + (so_total > 0 ? 40 * so_idx / so_total : 0);
        compatUiSetPct(pct);
        if (cb) cb("Loading ELF library", soName);
        compatLogFmt("Loading: %s", soName);
        LoadedSo* loaded_so = elfLoad(so_path.c_str(), cb);
        // Advance the progress bar now that elfLoad returned, so the screen
        // moves past the last RELA/JMPREL update and shows clear completion.
        {
            int pct2 = 18 + (so_total > 0 ? 40 * (so_idx + 1) / so_total : 40);
            compatUiSetPct(pct2 < 58 ? pct2 : 57);
        }
        if (loaded_so) {
            loaded.push_back(loaded_so);
            if (so_path == main_so) so = loaded_so;
            char ub[80];
            snprintf(ub, sizeof(ub), "Loaded %s OK", soName);
            compatUiLog(ub);
            if (cb) cb("ELF loaded", soName);
        } else {
            compatLogFmt("WARN: failed to load %s — skipping", soName);
            char ub[80];
            snprintf(ub, sizeof(ub), "WARN: %s failed to load", soName);
            compatUiLog(ub);
        }
        ++so_idx;
    }

    result.unresolved  = elfGetUnresolvedCount();
    result.svcPermCode = elfGetLastSvcPermCode();

    if (!so) {
        compatLog("Main .so failed to load");
        result.errorStage  = "Loading ELF library";
        result.errorDetail = "ELF loader rejected the main .so — may not be valid ARM64.";
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        return result;
    }
    if (result.unresolved > 0) {
        compatLogFmt("ELF: %d total unresolved symbols across all libs", result.unresolved);
    }

    // ── 5b. Verify code pages are executable ────────────────────────────────
    if (cb) cb("Checking code permissions", "Verifying JIT code mapping...");
    if (result.svcPermCode != 0) {
        compatLogFmt("Aborting launch — JIT code mapping failed (0x%08X). "
                     "Calling into the game would cause a Switch fatal error.",
                     result.svcPermCode);
        result.errorStage  = "Setting code pages executable";
        result.errorDetail = "JIT allocation failed — code segment is not executable. "
                             "Check that Atmosphere CFW is active and has JIT permission.";
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        return result;
    }

    // ── 5c. Run constructors for all SOs (dependency order, smallest first) ──
    compatUiSetPct(58);
    // Install fake Bionic TLS block BEFORE any game code runs.
    // libgame.so reads directly from TPIDR_EL0 via `mrs` — our shim table cannot
    // intercept this.  Without this, every site that touches Bionic's thread-local
    // state (EH globals, locale, etc.) faults at virtual address 0x28.
    androidTlsInstall();
    {
        int total_ctors = 0;
        for (LoadedSo* lso : loaded) total_ctors += (int)lso->init_arr_count;
        compatLogFmt("CTORS: starting %d constructors — this may take 30-120s, please wait", total_ctors);
        compatLogFlush();
        char cb_msg[96];
        snprintf(cb_msg, sizeof(cb_msg), "%d constructors — may take 30-120s, please wait...", total_ctors);
        compatUiLog(cb_msg);
        if (cb) cb("Running constructors", cb_msg);
    }
    for (LoadedSo* lso : loaded) {
        elfRunCtors(lso, cb);
    }

    // ── 6. Set up ANativeWindow (no EGL here — main thread does that) ────────
    // The window is whatever the orientation rules decided, not a fixed
    // 1280x720: a portrait game gets a portrait-shaped window and is never
    // told that the panel it is on is the wrong way round.
    {
        GameOrient want = GameOrient::Unspecified;
        switch (g_screen_orient) {           // Android screenOrientation values
            case 0: case 6: case 8: case 11: want = GameOrient::Landscape;       break;
            case 1: case 7: case 9: case 12: want = GameOrient::Portrait;        break;
            case 4: case 10:                 want = GameOrient::Sensor;          break;
            default:                         want = GameOrient::Unspecified;     break;
        }
        // sensorLandscape/sensorPortrait keep their axis but may flip within
        // it; for our purposes that is the same as the fixed orientation.
        if (g_screen_orient == 6)  want = GameOrient::SensorLandscape;
        if (g_screen_orient == 7)  want = GameOrient::SensorPortrait;
        orientInit(want);
    }
    const Presentation& pres = orientGet();

    ANativeWindow* nwin = &g_compat.window;
    nwin->width  = pres.content_w;
    nwin->height = pres.content_h;
    nwin->format = 1; // RGBA_8888
    nwin->nwin   = nwindowGetDefault();
    if (pres.transform)
        nwindowSetTransform(nwin->nwin, pres.transform);

    // ── 7. Set up ANativeActivity ────────────────────────────────────────────
    // Store paths durably so the pointers remain valid after this function returns.
    g_base_dir_stored  = base_dir;
    g_apk_path_stored  = apk_path;
    g_pkg_name_stored  = pkg_name;

    ANativeActivity* act = &g_compat.activity;
    memset(&g_compat.callbacks, 0, sizeof(g_compat.callbacks));
    act->callbacks        = &g_compat.callbacks;
    act->vm               = (JavaVM*)g_compat.vm_outer;
    act->env              = (JNIEnv*)g_compat.env_outer;
    act->clazz            = (void*)0x4001;
    act->internalDataPath = g_base_dir_stored.c_str();
    act->externalDataPath = g_base_dir_stored.c_str();
    // Where this game's expansion files were installed (see step 2b). Games
    // reach these either through the activity or through getObbDir().
    g_obb_dir_stored      = obb::dirFor(base_dir);
    act->obbPath          = g_obb_dir_stored.c_str();
    act->sdkVersion       = 26;
    act->instance         = nullptr;
    act->window           = nwin;

    strncpy(g_compat.asset_mgr.base_path, asset_dir.c_str(),
            sizeof(g_compat.asset_mgr.base_path) - 1);
    act->assetManager = &g_compat.asset_mgr;

    // ── 8. Scan libgame for Java_ native methods ─────────────────────────────
    compatLog("Scanning for Java_ native methods in main .so...");
    if (so->symtab && so->strtab) {
        int jcount = 0;
        for (uint32_t i = 1; i < so->sym_count; i++) {
            const Elf64_Sym& s = so->symtab[i];
            if (s.st_shndx == SHN_UNDEF || s.st_value == 0) continue;
            if (so->strsz > 0 && (uint64_t)s.st_name >= so->strsz) continue;
            if (so->alloc_size > 0 && s.st_value >= so->alloc_size) continue;
            const char* sname = so->strtab + s.st_name;
            if (strncmp(sname, "Java_", 5) == 0) {
                compatLogFmt("JAVA_METHOD: %s @%p", sname,
                             (void*)((uint8_t*)so->base + s.st_value));
                ++jcount;
            }
        }
        compatLogFmt("JAVA_METHOD: %d total Java_ symbols found", jcount);
    }

    // ── 9+. Game execution runs on the MAIN thread (has SDL2 EGL context). ──
    // Return here — main.cpp's runLaunch will call runGameOnMainThread.
    compatUiLog("ELF loaded — handing off to main thread");
    compatUiSetPct(95);
    if (cb) cb("ELF loaded", "Starting game on main thread...");
    compatLog("ELF: loading complete — game execution on main thread");
    compatLogFlush();

    result.ok      = true;
    result.game_so = (void*)so;
    // Note: g_compat_log stays open; runGameOnMainThread will close it.
    return result;
}

// ─── Branding overlay ─────────────────────────────────────────────────────────
// Draws "Viridite vX.Y.Z" over the game's own loading screen — a small
// GLES2 textured quad composited directly into the game's frame, right after
// nativeRender() returns and before the buffer swap. We can't reuse the
// game's own bitmap font without reverse-engineering its asset pipeline, so
// this renders our own app font once into a texture instead (matching the
// launcher's colour scheme: white "Android" + icon-green "Horizon").
// Hidden automatically once the game calls splashScreenHasCompleted (see
// compatMarkSplashDone below) so it never overlaps actual gameplay.
static volatile bool g_splash_active = true;
void compatMarkSplashDone() { g_splash_active = false; }

namespace {
struct BrandOverlay {
    bool     ready   = false;
    bool     failed  = false;
    GLuint   prog    = 0;
    GLuint   tex     = 0;
    GLuint   vbo     = 0;
    GLint    aPos = -1, aUV = -1, uRect = -1, uScreen = -1, uTex = -1;
    int      texW = 0, texH = 0;
};
static BrandOverlay g_brand;

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        compatLogFmt("branding: shader compile FAIL: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static void initBrandOverlay() {
    g_brand.failed = true;  // reset to false only on full success below

    // Render the text once into an SDL surface using the same fonts the
    // launcher UI uses (system BFTTF, falling back to the bundled romfs font).
    PlFontData fd = {};
    TTF_Font* font = nullptr;
    if (plGetSharedFontByType(&fd, PlSharedFontType_Standard) == 0 && fd.size > 0) {
        SDL_RWops* rw = SDL_RWFromConstMem(fd.address, (int)fd.size);
        font = TTF_OpenFontRW(rw, 1, 26);
    }
    if (!font) {
        romfsInit();
        font = TTF_OpenFont("romfs:/fonts/DejaVuSans.ttf", 26);
    }
    if (!font) { compatLog("branding: font load FAIL — overlay disabled"); return; }
    // HCR's own version text is a bold condensed display font — our system
    // font isn't that shape, but bold at least matches its weight better
    // than a thin regular face sitting right next to it.
    TTF_SetFontStyle(font, TTF_STYLE_BOLD);

    const SDL_Color white = {255, 255, 255, 255};
    const SDL_Color green = {52,  230, 134, 255};
    const SDL_Color dim   = {180, 182, 205, 255};
    const SDL_Color black = {0,   0,   0,   255};

    // HCR's own version text is set in "Agency FB" (found via the bundled
    // gamefont.fnt bitmap-font descriptor: bold=1, with a baked-in outline —
    // that's the chunky look). Agency FB itself is a commercial Windows font
    // we can't bundle, but the outline is easy to fake: render each piece
    // twice — once in black, blitted at several small offsets around the
    // real position, then the actual colour on top — a standard "poor man's
    // stroke" technique.
    std::string verStr = std::string(" ") + BUILD_VERSION;
    struct TextPiece { const char* str; SDL_Color color; };
    TextPiece pieces[3] = {
        {"Android ",      white},
        {"Horizon",       green},
        {verStr.c_str(),  dim},
    };
    SDL_Surface* fillSurf[3] = {};
    SDL_Surface* outlineSurf[3] = {};
    bool ok = true;
    for (int i = 0; i < 3; i++) {
        fillSurf[i]    = TTF_RenderUTF8_Blended(font, pieces[i].str, pieces[i].color);
        outlineSurf[i] = TTF_RenderUTF8_Blended(font, pieces[i].str, black);
        if (!fillSurf[i] || !outlineSurf[i]) ok = false;
    }
    TTF_CloseFont(font);
    if (!ok) {
        compatLog("branding: text render FAIL — overlay disabled");
        for (int i = 0; i < 3; i++) { if (fillSurf[i]) SDL_FreeSurface(fillSurf[i]);
                                       if (outlineSurf[i]) SDL_FreeSurface(outlineSurf[i]); }
        return;
    }

    const int OUTLINE = 2;  // px of stroke padding on every side
    int w = OUTLINE * 2, h = 0;
    for (int i = 0; i < 3; i++) { w += fillSurf[i]->w; if (fillSurf[i]->h > h) h = fillSurf[i]->h; }
    h += OUTLINE * 2;
    SDL_Surface* combo = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!combo) {
        compatLog("branding: combo surface FAIL");
        for (int i = 0; i < 3; i++) { SDL_FreeSurface(fillSurf[i]); SDL_FreeSurface(outlineSurf[i]); }
        return;
    }
    SDL_FillRect(combo, nullptr, SDL_MapRGBA(combo->format, 0, 0, 0, 0));
    static const int kOffsets[8][2] = {
        {-1,-1},{0,-1},{1,-1}, {-1,0},{1,0}, {-1,1},{0,1},{1,1}
    };
    int x = OUTLINE;
    for (int i = 0; i < 3; i++) {
        SDL_SetSurfaceBlendMode(outlineSurf[i], SDL_BLENDMODE_BLEND);
        for (auto& o : kOffsets) {
            SDL_Rect dst = {x + o[0], OUTLINE + o[1], outlineSurf[i]->w, outlineSurf[i]->h};
            SDL_BlitSurface(outlineSurf[i], nullptr, combo, &dst);
        }
        x += fillSurf[i]->w;
    }
    x = OUTLINE;
    for (int i = 0; i < 3; i++) {
        SDL_SetSurfaceBlendMode(fillSurf[i], SDL_BLENDMODE_BLEND);
        SDL_Rect dst = {x, OUTLINE, fillSurf[i]->w, fillSurf[i]->h};
        SDL_BlitSurface(fillSurf[i], nullptr, combo, &dst);
        x += fillSurf[i]->w;
        SDL_FreeSurface(fillSurf[i]);
        SDL_FreeSurface(outlineSurf[i]);
    }

    glGenTextures(1, &g_brand.tex);
    glBindTexture(GL_TEXTURE_2D, g_brand.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, combo->w, combo->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, combo->pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_brand.texW = combo->w; g_brand.texH = combo->h;
    SDL_FreeSurface(combo);

    static const char* kVS =
        "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
        "uniform vec4 uRect; uniform vec2 uScreen;\n"
        "void main() {\n"
        "  vec2 pix = uRect.xy + aPos * uRect.zw;\n"
        "  vec2 ndc = vec2(pix.x / uScreen.x * 2.0 - 1.0, 1.0 - pix.y / uScreen.y * 2.0);\n"
        "  gl_Position = vec4(ndc, 0.0, 1.0); vUV = aUV;\n"
        "}\n";
    static const char* kFS =
        "precision mediump float; varying vec2 vUV; uniform sampler2D uTex;\n"
        "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVS);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) return;
    g_brand.prog = glCreateProgram();
    glAttachShader(g_brand.prog, vs);
    glAttachShader(g_brand.prog, fs);
    // Deliberately high, unusual attribute indices — cocos2d-x's own shaders
    // use low indices (0-2ish) and its GL state cache assumes nothing else
    // touches those slots. Using indices it never looks at means our draw
    // can't desync its cache no matter what we leave enabled/disabled.
    glBindAttribLocation(g_brand.prog, 8, "aPos");
    glBindAttribLocation(g_brand.prog, 9, "aUV");
    glLinkProgram(g_brand.prog);
    GLint linked = 0;
    glGetProgramiv(g_brand.prog, GL_LINK_STATUS, &linked);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!linked) { compatLog("branding: shader link FAIL — overlay disabled"); return; }

    g_brand.aPos    = 8;
    g_brand.aUV     = 9;
    g_brand.uRect   = glGetUniformLocation(g_brand.prog, "uRect");
    g_brand.uScreen = glGetUniformLocation(g_brand.prog, "uScreen");
    g_brand.uTex    = glGetUniformLocation(g_brand.prog, "uTex");

    // Unit quad: pos(x,y) + uv(x,y) per vertex, triangle strip
    const float verts[] = {
        0,0, 0,0,
        1,0, 1,0,
        0,1, 0,1,
        1,1, 1,1,
    };
    glGenBuffers(1, &g_brand.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_brand.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    g_brand.failed = false;
    g_brand.ready  = true;
    compatLogFmt("branding: overlay ready (%dx%d texture)", g_brand.texW, g_brand.texH);
}

// ─── Loading-screen detection by pixel fingerprint ───────────────────────────
// splashScreenHasCompleted only tells us the ENTIRE splash+loading sequence
// is over — it doesn't distinguish the earlier Fingersoft logo animation
// (plain black background) from the later "HILL CLIMB RACING / LOADING..."
// screen with the version text, so the overlay was appearing during the logo
// too ("showed too early"). Instead, sample a few fixed screen points each
// frame and only show the overlay when their colours match this specific
// screen's look (estimated from a real handheld screenshot — corners of its
// dark vignette background + the white loading bar). Coordinates are in
// image space (0,0 = top-left); glReadPixels needs window space (0,0 =
// bottom-left), so the Y is flipped at sample time.
struct ProbePoint { int x, y; uint8_t r, g, b; };
// Recalibrated from a REAL captured log (build 77 test): the two top corners
// read (48,54,66) and (45,49,60) — a blue-gray, not the near-black originally
// guessed. The loading bar point turned out unstable (255,255,255 early,
// dropping to 67,73,76 once the fill animation passes it), so it's dropped
// in favour of a third corner — same static vignette tone, always stable.
// Top corners confirmed matching in a real log (48,54,66)/(45,49,60) vs
// (46,51,63) — no recalibration needed there. The bottom-left corner reads
// genuinely darker (16,19,27), consistently across two samples — the
// vignette isn't uniform, it darkens further toward that corner.
static const ProbePoint kLoadingProbes[3] = {
    {10,   10,  46, 51, 63},   // dark vignette corner, top-left
    {1270, 10,  46, 51, 63},   // dark vignette corner, top-right
    {10,  710,  16, 19, 27},   // vignette corner, bottom-left (darker falloff)
};
static const int kProbeTolerance = 25;

// Logs actual vs. expected every 300 frames (~5s at 60fps, not every frame —
// this now runs for the whole session, not just briefly during loading) so a wrong
// guess is cheap to recalibrate from the next compat_log.txt — this data is
// exactly what's needed to correct kLoadingProbes without more guesswork.
// Two-sided debounce for the latch, tuned from two failed extremes seen on
// real hardware:
//   - A 90-frame SUSTAINED mismatch requirement (first attempt) was too
//     slow: the loading→garage transition is multi-stage (fades through
//     more than one dark moment), so a brief re-match mid-transition kept
//     resetting the streak counter, and the overlay visibly disappeared
//     then reappeared before finally latching — "it fades out then comes
//     back, it shouldn't".
//   - Latching on the very FIRST mismatched frame (second attempt) was too
//     fast: real hardware showed it latching off at frame 161 (a couple of
//     seconds in), while the loading screen normally holds for much longer
//     — a single noisy/anti-aliased frame was enough to kill it for good.
// Fix: require a few consecutive matching frames to CONFIRM we're really on
// the loading screen (filters a stray single-frame false match), and a
// short — not long — sustained mismatch to CONFIRM we've really left
// (filters a stray single-frame false negative, without being slow enough
// for a multi-second transition to fool it via a brief re-match).
static int  s_matchStreak    = 0;
static int  s_mismatchStreak = 0;
static bool s_confirmedOnLoadingScreen = false;
static const int kEntryConfirmFrames = 5;   // ~0.1s — filters single-frame noise
static const int kExitConfirmFrames  = 8;   // short: reported lingering too long
                                             // in the fuel-select screen at 20

// glReadPixels is a genuine GPU pipeline stall — it forces the GPU to finish
// everything queued so far and copy pixels back to CPU memory, breaking the
// CPU/GPU overlap that keeps frame time low. Doing this every single frame
// while the overlay is active is real, avoidable stutter. The background
// doesn't change fast enough to need per-frame precision anyway — sampling
// every few frames costs a few extra ms of detection latency (imperceptible)
// for a big cut in stalls.
static const int kProbeEveryNFrames = 4;
static bool s_lastMatch = false;

static bool isOnLoadingScreen(int frame) {
    if (g_brand.failed) return false;  // already latched off — skip the probe entirely
    if (frame % kProbeEveryNFrames != 0) return s_lastMatch || s_confirmedOnLoadingScreen;

    uint8_t px[4];
    bool match = true;
    char detail[256]; detail[0] = '\0';
    for (int i = 0; i < 3; i++) {
        const ProbePoint& p = kLoadingProbes[i];
        glReadPixels(p.x, 720 - 1 - p.y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        bool ok = std::abs((int)px[0] - (int)p.r) <= kProbeTolerance &&
                  std::abs((int)px[1] - (int)p.g) <= kProbeTolerance &&
                  std::abs((int)px[2] - (int)p.b) <= kProbeTolerance;
        if (!ok) match = false;
        char part[80];
        snprintf(part, sizeof(part), " [%d]=%d,%d,%d(want %d,%d,%d)%s",
                 i, px[0], px[1], px[2], p.r, p.g, p.b, ok ? "" : "X");
        strncat(detail, part, sizeof(detail) - strlen(detail) - 1);
    }
    if (frame % 300 == 0)
        compatLogFmt("branding: probe%s → %s", detail, match ? "MATCH" : "no match");
    s_lastMatch = match;

    if (match) {
        s_mismatchStreak = 0;
        if (!s_confirmedOnLoadingScreen && ++s_matchStreak >= kEntryConfirmFrames) {
            s_confirmedOnLoadingScreen = true;
            compatLogFmt("branding: confirmed on loading screen (frame %d)", frame);
        }
    } else {
        s_matchStreak = 0;
        if (s_confirmedOnLoadingScreen && ++s_mismatchStreak >= kExitConfirmFrames) {
            g_brand.failed = true;  // confirmed past it — done for good
            compatLogFmt("branding: past loading screen (frame %d) — overlay off for rest of session", frame);
        }
    }
    // Show the overlay whenever we're either confirmed-on or provisionally
    // matching (i.e. don't wait for the entry debounce before ever drawing —
    // that's just to decide when it's safe to LATCH, not to gate visibility).
    return match || s_confirmedOnLoadingScreen;
}

// Draw the overlay quad over the game's already-rendered frame. Screen-space
// position: bottom-left, stacked just above where HCR draws its own
// "1.67.0 (166)" version text, so both are visible without overlapping.
// cocos2d-x keeps its OWN software cache of GL state (current program, bound
// buffer/texture, enabled attribs...) and skips redundant driver calls when it
// thinks nothing changed. Our first cut here changed real GL state behind
// its back — its cache went stale, and its very next draw call fed the GPU
// wrong attribute/program/texture state, which is exactly why the screen
// went solid black after the overlay appeared for one frame. Fix: save every
// piece of global GL state we touch and restore it bit-for-bit before
// returning, so cocos2d's next draw sees an identical GL machine to the one
// it left behind and its cache stays valid. Vertex attributes use indices 8/9
// (cocos2d only uses low indices), so we don't even need to save/restore
// per-attribute pointer state — just enable/disable is enough there.
static void drawBrandOverlay() {
    if (g_brand.failed) return;
    if (!g_brand.ready) initBrandOverlay();
    if (!g_brand.ready) return;

    // Position estimated from a real handheld-mode screenshot: HCR draws its
    // own "1.67.0 (166)" bottom-left, ~20px in from the left edge and ~15px
    // up from the bottom, roughly 30px tall. We sit right next to it on the
    // same line rather than stacking above — matches "next to the version
    // text" from the original ask. Nudge X_GAP/Y_BOTTOM if it's off in
    // practice; there's no way to pixel-measure this without the file itself.
    const float SCALE      = 0.9f;
    const float X_GAP      = 190.0f;  // estimated right edge of "1.67.0 (166)" + padding
    const float Y_BOTTOM   = 15.0f;   // estimated bottom margin, matching the game's own
    float w = g_brand.texW * SCALE, h = g_brand.texH * SCALE;
    float x = X_GAP;
    float y = 720.0f - h - Y_BOTTOM;

    GLint  prevProgram = 0, prevArrayBuf = 0, prevTex = 0, prevActiveTex = 0;
    GLint  prevBlendSrcRGB = 0, prevBlendDstRGB = 0, prevBlendSrcA = 0, prevBlendDstA = 0;
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevCull  = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDstA);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_brand.prog);
    glUniform4f(g_brand.uRect, x, y, w, h);
    glUniform2f(g_brand.uScreen, 1280.0f, 720.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_brand.tex);
    glUniform1i(g_brand.uTex, 0);

    glBindBuffer(GL_ARRAY_BUFFER, g_brand.vbo);
    glEnableVertexAttribArray(g_brand.aPos);
    glEnableVertexAttribArray(g_brand.aUV);
    glVertexAttribPointer(g_brand.aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glVertexAttribPointer(g_brand.aUV,  2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(g_brand.aPos);
    glDisableVertexAttribArray(g_brand.aUV);

    // Restore everything, exactly, before handing the frame back to cocos2d.
    glUseProgram(prevProgram);
    glBindBuffer(GL_ARRAY_BUFFER, prevArrayBuf);
    glActiveTexture(prevActiveTex);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    glBlendFuncSeparate(prevBlendSrcRGB, prevBlendDstRGB, prevBlendSrcA, prevBlendDstA);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevCull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
}

// The game draws directly via raw EGL/GLES2 on the same window/context the
// launcher's SDL_Renderer uses. When the game loop ends — whether by a clean
// quit or (more importantly) a caught mid-frame crash — it can leave GL in an
// arbitrary state: a bound shader/texture/buffer, or scissor/stencil test
// left enabled from whatever draw call was interrupted. SDL's renderer
// doesn't necessarily reset every one of these before its own draws, so a
// crash could leave the launcher showing a corrupted frame (a stray white
// quad, a clipped scissor rect, etc.) — visually "a white box then nothing,
// have to close with Home" — even though our side of the game loop exited
// and logged cleanly. Reset everything to sane defaults before handing the
// window back to the launcher.
static void resetGLStateForLauncher(int screenW, int screenH) {
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int i = 0; i < 16; i++) glDisableVertexAttribArray((GLuint)i);
    for (int unit = 0; unit < 4; unit++) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glViewport(0, 0, screenW, screenH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
} // namespace

// See loader.h doc comment. Definitive "hide the overlay for good" trigger —
// pixel-fingerprinting alone matched vehicle-select/garage/upgrade screens
// too (same dark-vignette corners as the loading screen, confirmed on
// hardware holding a MATCH for 80+ seconds past when loading actually
// finished), so trackPage firing at all — something the loading screen
// itself never does — is the reliable signal instead.
void compatMarkPastLoading() {
    if (g_brand.failed) return;
    g_brand.failed = true;
    compatLog("branding: trackPage fired — definitely past loading, overlay off for rest of session");

    // Real menus/gameplay start drawing for real now — FastLoad throttles the
    // GPU to its minimum clock, which was fine while the engine was just
    // unpacking assets behind a loading bar, but would tank fps from here on.
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    compatLog("perf: CPU boost mode -> Normal (leaving loading, GPU clocks restored)");
}

// ─── runGameOnMainThread ─────────────────────────────────────────────────────
// Called from the MAIN thread (SDL2's EGL context is current on this thread).
// Captures SDL2's EGL context, runs JNI_OnLoad, nativeSetPaths, nativeInit,
// then loops on nativeRender + eglSwapBuffers until the game faults or exits.
// sdl_win is SDL_Window* used for buffer swap via SDL_GL_SwapWindow.
void runGameOnMainThread(void* game_so_ptr,
                         void* sdl_win,
                         const std::string& apk_path,
                         const std::string& data_path) {
    LoadedSo* so = (LoadedSo*)game_so_ptr;
    SDL_Window* win = (SDL_Window*)sdl_win;
    g_game_so = so;   // for compatFindGameSym (JNI → native callbacks)

    // JNI_OnLoad/nativeInit and the engine's own splash+loading screen are
    // pure CPU work (asset decompression, scene graph setup) with nothing
    // real on screen yet — same shape as our own loader thread. Boost CPU
    // clocks here; compatMarkPastLoading() (trackPage firing) drops it back
    // to Normal the moment real gameplay/menu rendering needs the GPU back.
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    compatLog("perf: CPU boost mode -> FastLoad (game startup/loading, GPU throttled)");

    // Re-install fake Android TLS on THIS thread.  TPIDR_EL0 is per-thread;
    // launchApk set it on its thread (the background worker) for ctors, but
    // runGameOnMainThread runs on the main thread where TPIDR_EL0 is still 0.
    androidTlsInstall();

    // SimpleAudioEngine paths are asset-relative — point audio.cpp at the
    // extracted APK assets, and bring the mixer up now, outside the game
    // loop's recovery window (lazy init mid-frame is a fault suspect).
    compatAudioSetAssetsDir((data_path + "/assets").c_str());
    compatAudioWarmup();

    // Restore the game's saved state (UserDefaults) from the previous session
    jniUserDefaultsLoad((data_path + "/userdefaults.bin").c_str());

    // Capture SDL2's active EGL context (current on this main thread).
    g_egl_display = eglGetCurrentDisplay();
    g_egl_surface = eglGetCurrentSurface(EGL_DRAW);
    g_egl_context = eglGetCurrentContext();
    if (g_egl_display != EGL_NO_DISPLAY && g_egl_surface != EGL_NO_SURFACE &&
        g_egl_context != EGL_NO_CONTEXT) {
        compatLog("EGL: using SDL2 context on main thread");
    } else {
        compatLog("EGL: SDL2 context not current — GL calls may fail");
    }
    // Reference point for "game stdio[tid=...]" lines below — anything tagged
    // with this exact tid ran on the render thread (a real stutter suspect if
    // it's doing decode work); anything else ran on a background thread.
    compatLogFmt("main/render thread tid=%p", (void*)threadGetSelf());
    compatLogFlush();

    // ── Unity IL2CPP games take a completely different path ─────────────────
    // Detected by libunity.so being loaded (see engine detection in launchApk).
    // Hand off to the VNX-Unity-Runtime submodule, which reproduces Unity's
    // native boot from here. It runs on this main thread with the GL context
    // current, same as the cocos2d-x loop below. When it returns we're done —
    // the diagnostics are in compat_log.txt.
    {
        std::string lib_dir = data_path + "/lib";
        if (unityIsGame(lib_dir)) {
            compatUiLog("Unity game — starting Unity runtime bringup");
            UnityLaunchResult ur = unityRun(lib_dir, apk_path, data_path,
                                            g_compat.activity.internalDataPath
                                            ? g_compat.activity.internalDataPath : "");
            if (!ur.ok) {
                compatLogFmt("unity: bringup did not reach a running game — stage=%s detail=%s",
                             ur.errorStage ? ur.errorStage : "?", ur.errorDetail.c_str());
                compatUiLog(ur.errorStage ? ur.errorStage : "Unity bringup incomplete");
            }
            appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
            if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
            return;
        }
    }

    JNIEnv* env = (JNIEnv*)g_compat.env_outer;
    jobject obj = (jobject)(uintptr_t)0x4001;

    // ── JNI_OnLoad ────────────────────────────────────────────────────────────
    typedef int32_t (*JNI_OnLoad_fn)(JavaVM**, void*);
    JNI_OnLoad_fn jni_onload = (JNI_OnLoad_fn)so->findSym("JNI_OnLoad");
    if (jni_onload) {
        compatLogFmt("Calling JNI_OnLoad @%p ...", (void*)jni_onload);
        g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0; g_recover_far = 0;
        if (setjmp(g_recover_jmp) == 0) {
            int32_t ver = jni_onload((JavaVM**)g_compat.vm_outer, nullptr);
            g_in_recover = false;
            compatLogFmt("JNI_OnLoad returned: 0x%X", ver);
        } else {
            g_in_recover = false;
            char sym_buf[160];
            elfNearestSym(so, g_recover_pc - (uint64_t)so->base, sym_buf, sizeof(sym_buf));
            compatLogFmt("JNI_OnLoad FAULT sig=%d esr=0x%08x pc=%p far=%p sym=%s — skipped",
                         g_recover_sig, g_recover_esr,
                         (void*)g_recover_pc, (void*)g_recover_far, sym_buf);
            const uint32_t* insn = (const uint32_t*)(uintptr_t)g_recover_pc;
            compatLogFmt("INSN: [pc-12]=%08x [pc-8]=%08x [pc-4]=%08x [pc]=%08x [pc+4]=%08x",
                         insn[-3], insn[-2], insn[-1], insn[0], insn[1]);
        }
        compatLogFlush();
    }

    // ── ANativeActivity_onCreate (NativeActivity path) ────────────────────────
    typedef void (*OnCreate_fn)(ANativeActivity*, void*, size_t);
    OnCreate_fn on_create = (OnCreate_fn)so->findSym("ANativeActivity_onCreate");
    if (on_create) {
        ANativeActivity* act = &g_compat.activity;
        compatLogFmt("ANativeActivity_onCreate @%p", (void*)on_create);
        g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0; g_recover_far = 0;
        if (setjmp(g_recover_jmp) == 0) {
            on_create(act, nullptr, 0);
            g_in_recover = false;
            compatLog("onCreate returned");
        } else {
            g_in_recover = false;
            compatLogFmt("onCreate FAULT sig=%d esr=0x%08x pc=%p far=%p — skipped",
                         g_recover_sig, g_recover_esr,
                         (void*)g_recover_pc, (void*)g_recover_far);
        }
        compatLogFlush();

        // Deliver window if callback registered
        ANativeActivityCallbacks* cbs = &g_compat.callbacks;
        if (cbs->onStart)  { cbs->onStart(act); }
        if (cbs->onResume) { cbs->onResume(act); }
        if (cbs->onNativeWindowCreated) {
            compatLog("onNativeWindowCreated");
            cbs->onNativeWindowCreated(act, &g_compat.window);
        }
        compatLog("Entering game loop (NativeActivity)");
        compatLogFlush();
        // NativeActivity drives its own render loop via callbacks; we wait.
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        return;
    }

    // ── Cocos2d-x direct Java_ path ───────────────────────────────────────────
    compatLog("No NativeActivity — Cocos2d-x direct Java_ path");
    compatLogFlush();

    // Find one of cocos2d-x's engine entry points.
    //
    // This used to be a single findSym on the stock name,
    // Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender. That is what Hill
    // Climb Racing exports, so it worked — and it is why a game could be
    // plainly cocos2d-x, load and link with nothing unresolved, and then sit
    // there with no render loop: the same engine compiled under the game's own
    // package exports a differently-spelled symbol, and the lookup came back
    // empty with nothing to say about it. Four answers, in order:
    //
    //   1. the stock org.cocos2dx.lib name — unchanged, so HCR resolves
    //      exactly as before and cannot regress,
    //   2. the org.cocos2dx.cpp spelling, the other one in common use,
    //   3. any exported Java_ symbol whose class and method match, whatever
    //      package it was built into (see compat/jnisym.h for the mangling),
    //   4. the JNI registration table, for a game that registers its natives
    //      through JNI_OnLoad instead of exporting them.
    auto findCocosSym = [&](const char* cls, const char* method) -> void* {
        char buf[256];
        snprintf(buf, sizeof(buf), "Java_org_cocos2dx_lib_%s_%s", cls, method);
        if (void* p = so->findSym(buf)) return p;
        snprintf(buf, sizeof(buf), "Java_org_cocos2dx_cpp_%s_%s", cls, method);
        if (void* p = so->findSym(buf)) {
            compatLogFmt("cocos2d-x: %s.%s found under org.cocos2dx.cpp", cls, method);
            return p;
        }
        if (so->symtab && so->strtab) {
            for (uint32_t i = 1; i < so->sym_count; i++) {
                const Elf64_Sym& sy = so->symtab[i];
                if (sy.st_shndx == SHN_UNDEF || sy.st_value == 0) continue;
                if (so->strsz > 0 && (uint64_t)sy.st_name >= so->strsz) continue;
                if (so->alloc_size > 0 && sy.st_value >= so->alloc_size) continue;
                const char* sname = so->strtab + sy.st_name;
                if (!jnisym::matches(sname, cls, method)) continue;
                compatLogFmt("cocos2d-x: %s.%s exported as %s (repackaged engine)",
                             cls, method, sname);
                return (void*)((uint8_t*)so->base + sy.st_value);
            }
        }
        if (void* p = jniFindRegisteredNative(method)) {
            compatLogFmt("cocos2d-x: %s.%s came from RegisterNatives, not an export",
                         cls, method);
            return p;
        }
        compatLogFmt("cocos2d-x: %s.%s not found by any spelling", cls, method);
        return nullptr;
    };

    typedef void (*SetPaths_fn)(JNIEnv*, jobject, jstring, jstring);
    SetPaths_fn setPaths = (SetPaths_fn)findCocosSym("Cocos2dxActivity", "nativeSetPaths");
    if (setPaths) {
        compatLogFmt("Cocos2d-x: nativeSetPaths @%p", (void*)setPaths);
        g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0; g_recover_far = 0;
        if (setjmp(g_recover_jmp) == 0) {
            setPaths(env, obj, (jstring)apk_path.c_str(), (jstring)data_path.c_str());
            g_in_recover = false;
            compatLog("Cocos2d-x: nativeSetPaths OK");
        } else {
            g_in_recover = false;
            char sym_buf[160];
            elfNearestSym(so, g_recover_pc - (uint64_t)so->base, sym_buf, sizeof(sym_buf));
            compatLogFmt("Cocos2d-x: nativeSetPaths FAULT sig=%d esr=0x%08x pc=%p far=%p sym=%s",
                         g_recover_sig, g_recover_esr,
                         (void*)g_recover_pc, (void*)g_recover_far, sym_buf);
            { const uint32_t* insn = (const uint32_t*)(uintptr_t)g_recover_pc;
              compatLogFmt("INSN: [pc-12]=%08x [pc-8]=%08x [pc-4]=%08x [pc]=%08x [pc+4]=%08x",
                           insn[-3], insn[-2], insn[-1], insn[0], insn[1]); }
        }
        compatLogFlush();
    }

    // Whatever the game's Java side would have done between setting paths and
    // starting the engine. For Hill Climb Racing that is NewBillingHandle.Init()
    // filling the shop's product list — 69 native calls that, not being made,
    // left the list empty and the shop crashing on it. This is the window
    // Android does it in, so it is the window to do it in.
    {
        static LoadedSo* s_so = nullptr;
        s_so = so;
        gameBeforeNativeInit(g_pkg_name_stored.c_str(), env, obj,
                             [](const char* sym) -> void* {
                                 return s_so ? s_so->findSym(sym) : nullptr;
                             });
    }

    typedef void (*NativeInit_fn)(JNIEnv*, jobject, jint, jint);
    NativeInit_fn nativeInit = (NativeInit_fn)findCocosSym("Cocos2dxRenderer", "nativeInit");
    if (!nativeInit)
        nativeInit = (NativeInit_fn)findCocosSym("Cocos2dxRenderer", "nativeResize");
    if (nativeInit) {
        compatLogFmt("Cocos2d-x: nativeInit @%p 1280x720", (void*)nativeInit);
        g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0; g_recover_far = 0;
        if (setjmp(g_recover_jmp) == 0) {
            nativeInit(env, obj, 1280, 720);
            g_in_recover = false;
            compatLog("Cocos2d-x: nativeInit OK");
        } else {
            g_in_recover = false;
            char sym_buf[160];
            elfNearestSym(so, g_recover_pc - (uint64_t)so->base, sym_buf, sizeof(sym_buf));
            compatLogFmt("Cocos2d-x: nativeInit FAULT sig=%d esr=0x%08x pc=%p far=%p sym=%s",
                         g_recover_sig, g_recover_esr,
                         (void*)g_recover_pc, (void*)g_recover_far, sym_buf);
            { const uint32_t* insn = (const uint32_t*)(uintptr_t)g_recover_pc;
              compatLogFmt("INSN: [pc-12]=%08x [pc-8]=%08x [pc-4]=%08x [pc]=%08x [pc+4]=%08x",
                           insn[-3], insn[-2], insn[-1], insn[0], insn[1]); }
        }
        compatLogFlush();
    }

    typedef void (*NativeRender_fn)(JNIEnv*, jobject);
    NativeRender_fn nativeRender = (NativeRender_fn)findCocosSym("Cocos2dxRenderer", "nativeRender");

    // SDL reports fingers in 0..1 of the physical panel. The game thinks it is
    // on a screen the size of the content rect, so a tap has to be moved into
    // that rect — otherwise every touch in a pillarboxed portrait game lands
    // several hundred pixels to the right of where the player aimed.
    auto mapFinger = [](float fx, float fy, float* ox, float* oy) {
        const Presentation& p = orientGet();
        int sx = (int)(fx * p.screen_w + 0.5f);
        int sy = (int)(fy * p.screen_h + 0.5f);
        orientMapTouch(&sx, &sy);
        *ox = (float)sx;
        *oy = (float)sy;
    };

    // ─── Touch input: SDL finger events → Cocos2dxRenderer touch natives ─────
    // The Java GLSurfaceView would normally deliver these; we call the game's
    // registered native entry points directly. Begin/End take a single id+xy;
    // Move/Cancel take JNI arrays (blob layout: [jint len][payload]).
    typedef void     (*TouchBE_fn)(JNIEnv*, jobject, jint, jfloat, jfloat);
    typedef void     (*TouchArr_fn)(JNIEnv*, jobject, void*, void*, void*);
    typedef jboolean (*KeyDown_fn)(JNIEnv*, jobject, jint);
    TouchBE_fn  touchBegin = (TouchBE_fn)findCocosSym("Cocos2dxRenderer", "nativeTouchesBegin");
    TouchBE_fn  touchEnd = (TouchBE_fn)findCocosSym("Cocos2dxRenderer", "nativeTouchesEnd");
    TouchArr_fn touchMove = (TouchArr_fn)findCocosSym("Cocos2dxRenderer", "nativeTouchesMove");
    KeyDown_fn  keyDown = (KeyDown_fn)findCocosSym("Cocos2dxRenderer", "nativeKeyDown");
    compatLogFmt("touch: begin=%p end=%p move=%p keyDown=%p",
                 (void*)touchBegin, (void*)touchEnd, (void*)touchMove, (void*)keyDown);

    // ─── Controller input: Switch pad → the game's own MOGA controller path ──
    // Hill Climb Racing ships real gamepad support (that's what its
    // Moga_Pro_Guide asset is for) behind two natives the Java MainActivity
    // would normally drive. Both signatures were read out of libgame.so rather
    // than guessed, since calling a native with the wrong argument shape would
    // fault:
    //
    //   onControllerConnectionEvent(env, thiz, jboolean connected, jint type)
    //     tbz w2,#0      -> arg1 is the connected flag
    //     sub w8,w3,#2 / cmp w8,#3 / b.hi ret
    //                    -> arg2 must be 2..5 or the whole call is ignored,
    //                       and it's stored as the active controller type
    //
    //   onControllerKeyEvent(env, thiz, jint keyCode, jboolean pressed)
    //     mov w19,w2     -> arg1 is the key code
    //     and w21,w3,#1  -> arg2 is the pressed flag
    //
    // The key handler branches on that stored type: type 4 decodes a
    // media-remote-ish range (89..109), while 2/3/5 decode the layout we
    // actually want — DPAD_UP..DPAD_CENTER (19..23), ENTER (66) and
    // BUTTON_A..BUTTON_SELECT (96..109). Codes are plain Android KeyEvent
    // values. 3 is used below simply because it lands in the non-4 group.
    typedef void (*CtrlConn_fn)(JNIEnv*, jobject, jboolean, jint);
    typedef void (*CtrlKey_fn)(JNIEnv*, jobject, jint, jboolean);
    CtrlConn_fn ctrlConn = (CtrlConn_fn)so->findSym(
        "Java_com_fingersoft_game_MainActivity_onControllerConnectionEvent");
    CtrlKey_fn  ctrlKey  = (CtrlKey_fn)so->findSym(
        "Java_com_fingersoft_game_MainActivity_onControllerKeyEvent");
    compatLogFmt("controller: conn=%p key=%p", (void*)ctrlConn, (void*)ctrlKey);

    // Android KeyEvent codes this build actually decodes. Everything else is
    // routed to the jump table's default case and does nothing, so there's no
    // point sending it.
    enum : jint {
        AKEY_DPAD_UP = 19, AKEY_DPAD_DOWN = 20, AKEY_DPAD_LEFT = 21,
        AKEY_DPAD_RIGHT = 22, AKEY_DPAD_CENTER = 23,
        AKEY_BUTTON_A = 96, AKEY_BUTTON_B = 97, AKEY_BUTTON_X = 99,
        AKEY_BUTTON_Y = 100, AKEY_BUTTON_L1 = 102, AKEY_BUTTON_R1 = 103,
        AKEY_BUTTON_L2 = 104, AKEY_BUTTON_R2 = 105, AKEY_BUTTON_START = 108,
    };
    // Switch pad (libnx/SDL button order) -> Android key. Gas and brake sit on
    // the shoulders because HCR's own layout puts them there, and the face
    // buttons stay available for menus. ZL/ZR duplicate L/R because the game
    // decodes L1/L2 to one case and R1/R2 to another, so either shoulder works.
    struct PadMap { int sdlButton; jint akey; };
    static const PadMap PAD_MAP[] = {
        {0,  AKEY_BUTTON_A},     {1,  AKEY_BUTTON_B},
        {2,  AKEY_BUTTON_X},     {3,  AKEY_BUTTON_Y},
        {6,  AKEY_BUTTON_L1},    {7,  AKEY_BUTTON_R1},   // L, R
        {8,  AKEY_BUTTON_L2},    {9,  AKEY_BUTTON_R2},   // ZL, ZR
        {10, AKEY_BUTTON_START},
        {12, AKEY_DPAD_LEFT},    {13, AKEY_DPAD_UP},
        {14, AKEY_DPAD_RIGHT},   {15, AKEY_DPAD_DOWN},
    };
    auto sendPadKey = [&](int sdlButton, bool pressed) {
        if (!ctrlKey) return;
        for (const auto& m : PAD_MAP)
            if (m.sdlButton == sdlButton)
                ctrlKey(env, obj, m.akey, pressed ? JNI_TRUE : JNI_FALSE);
    };

    // The launcher writes the chosen input mode to a marker file before
    // chain-loading (same channel as .fps_cap). Docked it always says
    // controller, since there's no touch screen to offer. Announcing a
    // controller changes how the game reads input, so someone who picked touch
    // must not get one registered behind their back.
    bool wantController = true;
    {
        FILE* f = fopen("sdmc:/Viridite/.launch_input", "r");
        if (f) {
            char buf[32] = {0};
            if (fgets(buf, sizeof buf, f)) {
                if (strncmp(buf, "touch", 5) == 0) wantController = false;
            }
            fclose(f);
        }
        compatLogFmt("controller: launcher requested %s input",
                     wantController ? "controller" : "touch");
    }

    if (ctrlConn && !wantController) {
        compatLog("controller: touch mode chosen — not announcing a controller");
        ctrlConn = nullptr;          // also re-enables the B->BACK fallback below
        ctrlKey  = nullptr;
    }

    if (ctrlConn) {
        // Announce a controller before the loop starts, so the game has one
        // registered by the time it reads input. Docked play depends on this:
        // without it there's no touch screen and nothing else to drive.
        ctrlConn(env, obj, JNI_TRUE, 3);
        compatLog("controller: announced connected (type 3) — pad input enabled");
    } else {
        compatLog("controller: game exposes no controller natives — pad input unavailable");
    }

    struct IntArr1   { jint len; jint   v[1]; };
    struct FloatArr1 { jint len; jfloat v[1]; };

    if (nativeRender) {
        compatLogFmt("Cocos2d-x: nativeRender @%p — game loop", (void*)nativeRender);
        compatLogFlush();
        volatile int frame = 0;
        bool crashed = false;
        // Frame-stall detector: measures real wall-clock time between one
        // completed frame (right after its swap) and the next. A game
        // holding a steady 60fps takes ~16.6ms/frame; anything well past
        // that for a single frame is a real, momentary hitch, not scheduler
        // noise. Logging exactly which frame and how long it stopped for is
        // what actually tells us WHERE to spend future optimization effort
        // instead of guessing from "it feels stuttery sometimes".
        // Target frame time for the optional per-APK cap (see
        // g_fps_cap_stored/readFpsCap above) — 0 means uncapped/default,
        // matching prior behavior exactly. The stall thresholds below widen
        // to match an active cap so the intentional pacing sleep added below
        // never logs itself as a "stall".
        const uint64_t targetFrameMs      = g_fps_cap_stored > 0 ? (1000 / (uint64_t)g_fps_cap_stored) : 0;
        const uint64_t kFrameStallMs      = std::max<uint64_t>(33, targetFrameMs + 5);
        const uint64_t kFrameStallSevereMs = std::max<uint64_t>(100, targetFrameMs + 60);
        if (targetFrameMs > 0)
            compatLogFmt("perf: framerate capped to %dfps (~%llums/frame)",
                         g_fps_cap_stored, (unsigned long long)targetFrameMs);
        uint64_t lastFrameTick = 0;
        while (appletMainLoop()) {
            // Recovery window covers the whole iteration (event poll, render,
            // swap) — a fault inside eglSwapBuffers/SDL used to rethrow to the
            // OS with nothing in the log.
            g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0; g_recover_far = 0;
            if (setjmp(g_recover_jmp) == 0) {
                // Poll SDL events so + button exits
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_QUIT ||
                        (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == 10 /*PLUS*/)) {
                        g_in_recover = false;
                        goto game_loop_done;
                    }
                    // Pad → the game's controller path. Sent before the BACK
                    // shim below so B reaches the game as a real button when a
                    // controller is registered, instead of only ever meaning
                    // "back".
                    if (ev.type == SDL_JOYBUTTONDOWN) sendPadKey(ev.jbutton.button, true);
                    if (ev.type == SDL_JOYBUTTONUP)   sendPadKey(ev.jbutton.button, false);
                    // Hat is the other spelling of the D-pad; SDL reports one
                    // or the other depending on build, so both are handled.
                    if (ev.type == SDL_JOYHATMOTION && ctrlKey) {
                        static Uint8 prevHat = SDL_HAT_CENTERED;
                        struct { Uint8 bit; jint key; } HAT[] = {
                            {SDL_HAT_UP, AKEY_DPAD_UP},     {SDL_HAT_DOWN,  AKEY_DPAD_DOWN},
                            {SDL_HAT_LEFT, AKEY_DPAD_LEFT}, {SDL_HAT_RIGHT, AKEY_DPAD_RIGHT},
                        };
                        for (const auto& h : HAT) {
                            bool was = (prevHat & h.bit) != 0, now = (ev.jhat.value & h.bit) != 0;
                            if (was != now) ctrlKey(env, obj, h.key, now ? JNI_TRUE : JNI_FALSE);
                        }
                        prevHat = ev.jhat.value;
                    }
                    // B button → Android BACK key (cocos routes it to menus).
                    // Only when no controller is registered; otherwise B is a
                    // real face button above.
                    if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == 1 /*B*/ &&
                        keyDown && !ctrlConn)
                        keyDown(env, obj, 4 /*AKEYCODE_BACK*/);
                    if (ev.type == SDL_FINGERDOWN && touchBegin) {
                        float mx, my; mapFinger(ev.tfinger.x, ev.tfinger.y, &mx, &my);
                        touchBegin(env, obj, (jint)ev.tfinger.fingerId, mx, my);
                    }
                    if (ev.type == SDL_FINGERUP && touchEnd) {
                        float mx, my; mapFinger(ev.tfinger.x, ev.tfinger.y, &mx, &my);
                        touchEnd(env, obj, (jint)ev.tfinger.fingerId, mx, my);
                    }
                    if (ev.type == SDL_FINGERMOTION && touchMove) {
                        float mx, my; mapFinger(ev.tfinger.x, ev.tfinger.y, &mx, &my);
                        IntArr1   ids = {1, {(jint)ev.tfinger.fingerId}};
                        FloatArr1 xs  = {1, {mx}};
                        FloatArr1 ys  = {1, {my}};
                        touchMove(env, obj, &ids, &xs, &ys);
                    }
                }

                nativeRender(env, obj);

                // (The in-game Viridite branding overlay that used to draw over
                // the loading screen was removed by request — the launcher and
                // its own splash already brand the session; stamping the game's
                // own loading screen isn't wanted. initBrandOverlay/
                // drawBrandOverlay are left defined but unused.)

                // Milestone screenshots (frame 30/300/900) were removed —
                // frame-stall logging (see kFrameStallMs below) caught this
                // capture itself causing a real ~1000ms stall at frame 900
                // (SDL_RenderReadPixels + IMG_SavePNG are both genuinely
                // expensive). The images they produced are already captured
                // and committed at docs/screenshots/game_frame{30,300,900}.png
                // and embedded in the README — nothing left to gain from
                // paying this cost on every future test run.

                bootFadeDraw();       // the tail of the Viridite reveal
                toast::draw();        // achievement unlocks, over the game
                // Swap buffers (Cocos2d-x doesn't call eglSwapBuffers itself)
                if (g_egl_display != EGL_NO_DISPLAY && g_egl_surface != EGL_NO_SURFACE)
                    eglSwapBuffers(g_egl_display, g_egl_surface);

                    // Docking, undocking and clipping the Joy-Cons on or off
                    // all change the rules, and all of them happen mid-game.
                    // Re-checking once a frame is cheap — orientUpdate only
                    // reads HID state and returns false when nothing moved.
                    if (orientUpdate()) {
                        const Presentation& np = orientGet();
                        g_compat.window.width  = np.content_w;
                        g_compat.window.height = np.content_h;
                        nwindowSetTransform(nwindowGetDefault(), np.transform);
                        // A game that queried the window size once will not ask
                        // again, so anything that resizes mid-run keeps drawing
                        // at its original shape until it next queries. The
                        // letterbox shims keep that inside the content rect
                        // regardless, so the worst case is bars of the wrong
                        // width rather than a broken picture.
                    }
                else if (win)
                    SDL_GL_SwapWindow(win);

                // Optional per-APK frame pacing (see targetFrameMs above) —
                // sleeps off whatever's left of the target frame time before
                // the stall detector takes its measurement below, so a
                // steady capped session reads as steady, not as a stall every
                // single frame.
                if (targetFrameMs > 0 && lastFrameTick != 0) {
                    uint64_t elapsedMs = (armGetSystemTick() - lastFrameTick) * 1000 / armGetSystemTickFreq();
                    if (elapsedMs < targetFrameMs)
                        SDL_Delay((Uint32)(targetFrameMs - elapsedMs));
                }

                // Frame-stall detector — measured right after the swap (and
                // the pacing sleep above, if any) so it covers the whole
                // frame (event poll, nativeRender, overlay, swap) exactly
                // once per iteration. Logged only past the threshold, so a
                // smooth 60fps session stays silent; this is a genuinely rare
                // event (not per-frame telemetry), so a real disk write per
                // stall is fine — nothing like the earlier per-frame logging
                // bugs this project already fixed.
                {
                    uint64_t nowTick = armGetSystemTick();
                    if (lastFrameTick != 0) {
                        uint64_t deltaMs = (nowTick - lastFrameTick) * 1000 / armGetSystemTickFreq();
                        if (deltaMs >= kFrameStallMs) {
                            compatLogFmt("%s: frame %d stalled for %llums",
                                         deltaMs >= kFrameStallSevereMs ? "STALL(severe)" : "stall",
                                         frame, (unsigned long long)deltaMs);
                        }
                    }
                    lastFrameTick = nowTick;
                }

                g_in_recover = false;
            } else {
                g_in_recover = false;
                char sym_buf[160];
                elfNearestSym(so, g_recover_pc - (uint64_t)so->base, sym_buf, sizeof(sym_buf));
                // Caller + operands, so "how did we get here" is answerable from
                // the log alone. A patched-but-still-crashing block previously
                // could not be explained without another hardware round trip.
                {
                    char lrb[128];
                    compatLogFmt("FAULT caller: lr=%p (%s) x0=%p x8=%p",
                                 (void*)g_recover_lr,
                                 so ? elfNearestSym(so, g_recover_lr, lrb, sizeof lrb) : "?",
                                 (void*)g_recover_x0, (void*)g_recover_x8);
                }
                compatLogFmt("Cocos2d-x: game loop FAULT sig=%d esr=0x%08x pc=%p far=%p sym=%s frame=%d — stop",
                             g_recover_sig, g_recover_esr,
                             (void*)g_recover_pc, (void*)g_recover_far, sym_buf, frame);
                {
                    extern void elfLogAddrInfo(const char*, uint64_t);
                    extern void elfDescribePc(uint64_t pc, char* buf, size_t sz);
                    char where[256];
                    elfDescribePc(g_recover_pc, where, sizeof(where));
                    compatLogFmt("FAULT pc: %s", where);
                    elfLogAddrInfo("FAULT pc", g_recover_pc);
                    elfLogAddrInfo("FAULT far", g_recover_far);
                }
                { const uint32_t* insn = (const uint32_t*)(uintptr_t)g_recover_pc;
                  compatLogFmt("INSN: [pc-12]=%08x [pc-8]=%08x [pc-4]=%08x [pc]=%08x [pc+4]=%08x",
                               insn[-3], insn[-2], insn[-1], insn[0], insn[1]); }
                crashed = true;
                goto game_loop_done;
            }

            ++frame;
            // fflush() to the SD card is a real, if brief, stall on FAT32 —
            // every 5s (300 frames) was adding a small periodic stutter for
            // marginal benefit; every 15s is still plenty for "is it alive"
            // diagnostics without paying that cost 3x as often.
            if (frame % 900 == 0) {
                compatLogFmt("game: frame %d", frame);
                compatLogFlush();
            }
        }
        game_loop_done:
        compatLogFmt("Cocos2d-x: loop done frames=%d", frame);
        // The mixer outlives the game (it belongs to the launcher process) —
        // silence it so music doesn't keep playing over the APK browser.
        compatAudioStopMusic();
        compatAudioStopAllEffects();
        jniUserDefaultsSave(/*force=*/true);
        g_game_so = nullptr;
        // Reset GL to sane defaults before the launcher's SDL_Renderer draws
        // again on this same context — see resetGLStateForLauncher's comment.
        // Deliberately NOT calling eglSwapBuffers/SDL_GL_SwapWindow here: an
        // extra swap outside of SDL_Renderer's own SDL_RenderPresent calls
        // desyncs its internal front/back-buffer bookkeeping (SDL2's GLES2
        // renderer backend assumes it's the only thing swapping this
        // surface) — that's almost certainly last build's freeze → fade →
        // rapid black/frozen-frame flicker. The very next SDL_RenderPresent
        // in the launcher's own screens presents cleanly on its own.
        resetGLStateForLauncher(1280, 720);
        // The game can call eglSwapInterval (it's in our shim table, mapped
        // straight to the real EGL function) to control its OWN frame
        // pacing — e.g. 0 for uncapped rendering. That setting is global to
        // the surface and survives a crash; if left at 0, the launcher's
        // SDL_Renderer (which expects vsync — SDL_RENDERER_PRESENTVSYNC)
        // would present as fast as the driver allows with no pacing at all,
        // which reads exactly like the persistent flicker reported after a
        // crash. Force vsync back on before the launcher renders again.
        if (g_egl_display != EGL_NO_DISPLAY) eglSwapInterval(g_egl_display, 1);

        // Games spawn real background libnx threads now (pt_create — e.g.
        // HCR's own asset loader, seen starting mid-session in the log).
        // We have no registry of them and no safe way to force-stop
        // arbitrary running native code, so after a CRASH we can't
        // guarantee none of them are still executing — still touching
        // JNI/audio/heap state that the launcher's menu code would then
        // race against. Two different GL/EGL state fixes (swap-desync,
        // then vsync) were tried across builds 78-79 and neither changed
        // the reported freeze→fade→rapid-flicker symptom, which fits an
        // ACTIVE ongoing conflict far better than a one-time leftover
        // state issue. Rather than keep guessing at symptoms, remove the
        // shared risk entirely: on a caught crash, don't attempt to
        // return to the launcher's menu in this same (possibly still
        // multi-threaded, possibly heap-corrupted) process at all — just
        // exit cleanly. Horizon OS tears down every thread in the process
        // together, which a same-process "return to menu" fundamentally
        // cannot guarantee. The launcher UI already forced a full app
        // restart before allowing a second game session (see gameRanOnce
        // in main.cpp), so landing back at the app list is a smaller UX
        // regression than an unrecoverable flicker.
        //
        // This risk is NOT specific to a crash — a deliberate + button quit
        // exits this same loop with `crashed` still false, but any real
        // background libnx thread the game spawned (e.g. HCR's own asset
        // loader) is just as likely to still be running either way. Reported
        // "+ makes everything flicker" is the same freeze→fade→rapid-flicker
        // symptom as the crash case, for the same reason — so exit
        // unconditionally here instead of only on a caught crash.
        compatLogFmt("Cocos2d-x: %s — exiting app cleanly rather than "
                     "risking a still-running game thread racing the launcher",
                     crashed ? "crash recovery" : "quit requested");
        compatLogFlush();
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        exit(0);
    } else {
        compatLog("Cocos2d-x: nativeRender not found");
    }

    compatLogFlush();
    if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
}
