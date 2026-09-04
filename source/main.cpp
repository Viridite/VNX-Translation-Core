#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <sys/stat.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "apk.h"
#include "compat/loader.h"
#include "compat/jingle.h"
#include "compat/bootfade.h"
#include "compat/reveal.h"
#include "compat/achievements.h"
#include "compat/sensors.h"
#include "arm32/arm32.h"
#include "build_number.h"
#include "avatar.h"

extern void compatLogFmt(const char* fmt, ...);

static constexpr float PI_F = 3.14159265358979323846f;

static const char* APK_DIR  = "sdmc:/Viridite/apks";
static const char* LOG_FILE = "sdmc:/Viridite/log.txt";

// ---------------------------------------------------------------------------
// Layout (1280×720)
// ---------------------------------------------------------------------------
static const int SW       = 1280;
static const int SH       = 720;
static const int HEADER_H = 72;
static const int FOOTER_H = 48;
static const int LIST_Y   = HEADER_H;
static const int LIST_H   = SH - HEADER_H - FOOTER_H;
static const int ITEM_H   = 108;
static const int ICON_SZ  = 84;
static const int VISIBLE  = LIST_H / ITEM_H;

// Viridite light theme — white base with the logo's vivid green as the accent.
// C_WHITE is the primary TEXT colour (dark on white); matches the launcher.
static const SDL_Color C_BG     = {248, 251, 249, 255};  // near-white background
static const SDL_Color C_HEADER = {255, 255, 255, 255};  // white surface
static const SDL_Color C_FOOTER = {242, 248, 245, 255};  // light footer
static const SDL_Color C_SEL    = {205, 244, 224, 255};  // light mint selection
static const SDL_Color C_DIV    = {224, 234, 228, 255};  // light divider/border
static const SDL_Color C_WHITE  = {17,  32,  24,  255};  // primary text (dark)
static const SDL_Color C_GRAY   = {92,  112, 102, 255};  // secondary text
static const SDL_Color C_DIM    = {142, 160, 150, 255};  // tertiary text
static const SDL_Color C_OK     = {0,   170, 80,  255};  // accent (vivid green)
static const SDL_Color C_ERR    = {214, 48,  79,  255};
static const SDL_Color C_WARN   = {176, 120, 0,   255};
static const SDL_Color C_INST   = {0,   170, 80,  255};
static const SDL_Color C_RIM    = {0,   190, 90,  255};  // accent rim

// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static void logOpen()  { g_log = fopen(LOG_FILE, "w"); }
static void logClose() { if (g_log) { fclose(g_log); g_log = nullptr; } }
static void logMsg(const char* msg) {
    if (g_log) { fputs(msg, g_log); fputc('\n', g_log); fflush(g_log); }
}
static void logSDL(const char* prefix) {
    if (!g_log) return;
    fputs(prefix, g_log); fputs(": ", g_log);
    fputs(SDL_GetError(), g_log); fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------------------
static const int BTN_A     = 0;
static const int BTN_B     = 1;
static const int BTN_X     = 2;
static const int BTN_Y     = 3;
static const int BTN_PLUS  = 10;
static const int BTN_MINUS = 11;

// ---------------------------------------------------------------------------
// Shared loader state — written by loader thread, read by main thread.
// ---------------------------------------------------------------------------
// Ring buffers defined in loader.cpp
extern char g_ui_log[20][128];   // throttled UI messages (every 512 entries etc.)
extern int  g_ui_head;
extern int  g_ui_pct;
// Full-detail log: every compatLog line written here — read by render thread without file I/O
extern char g_detail_log[28][164];
extern int  g_detail_head;

// Current high-level stage string, set by progressCallback on the loader thread.
static char g_ui_stage[80] = "Working...";

// ---------------------------------------------------------------------------
// Loader thread plumbing
// ---------------------------------------------------------------------------
struct LoaderCtx {
    std::string app_name;      // display name, for the overlay
    std::string       apk_path;
    std::string       pkg_name;
    bool              skip_install = false;
    LaunchResult      result;
    std::atomic<bool> done{false};
};

static LoaderCtx* g_loader_ctx = nullptr;

// ── Main-thread watchdog ────────────────────────────────────────────────────
// Brain It On stops with the log ending at the loader's "launchApk returned",
// and the very next statement on that thread is the atomic store of `done`.
// So `done` is set — which means the main thread never got back to the top of
// its wait loop, i.e. it is wedged inside the loop body. The body doesn't log,
// so the log simply stops and every call in it is equally suspect.
//
// Rather than logging every frame (which floods the file and adds SD writes to
// the very path under suspicion), the main thread just bumps a counter and
// publishes the name of the call it is about to make. A watchdog thread checks
// the counter every two seconds and says nothing while it advances — so a
// healthy run costs one atomic store per phase and zero log lines. If the
// counter stops, it names the call that never returned, and keeps saying so.
static std::atomic<const char*> g_main_phase {"start"};
static std::atomic<uint64_t>    g_main_frames{0};
static std::atomic<bool>        g_watchdog_stop{false};

static inline void mainPhase(const char* p) {
    g_main_phase.store(p, std::memory_order_relaxed);
}

// Every fault we have ever seen out of a game module landed in _free_r, and
// newlib's malloc is shared by the game's code and by ours — SDL_ttf allocates
// on every string it renders, which is most of what the draw block does. If a
// game constructor corrupted the arena or left the allocator's lock held, the
// main thread would block on its next allocation and never come back, while
// the loader thread carried on to completion. That is exactly the shape of
// this hang, so ask the allocator directly rather than keep inferring.
//
// If the "allocator responded" line never appears, malloc is the wedge.
static void probeAllocator(void) {
    compatLogRaw("WATCHDOG: probing the allocator — if no line follows this, malloc is wedged");
    void* p = malloc(64);
    if (!p) {
        compatLogRaw("WATCHDOG: allocator responded but returned NULL — out of memory");
        return;
    }
    free(p);
    compatLogRaw("WATCHDOG: allocator responded normally — malloc is NOT the wedge");
}

// Continuous integrity monitor.
//
// Checking the allocator between constructors could only ever say "one of
// these 421 broke it". Sampling it every 20ms while the loader publishes which
// constructor it is executing says which one — and it does so without anyone
// having to decide in advance what to check, which is the whole problem with
// adding a bespoke check each time and shipping a build to find out.
//
// The check itself is two loads and a compare, so 50 samples a second costs
// nothing measurable next to the ELF loading going on beside it.
static void arenaMonitorTick(void) {
    static bool reported = false;
    if (reported) return;
    char why[400];
    if (shimHeapCheckFast(why, sizeof(why))) return;
    reported = true;
    char buf[560];
    snprintf(buf, sizeof(buf),
             "MONITOR: allocator broke during %s ctor[%d] — %s",
             elfCurrentModule(), elfCurrentCtor(), why);
    compatLogRaw(buf);
}

static void watchdogThreadFn(void*) {
    uint64_t last = ~0ull;
    int      stuck = 0;
    while (!g_watchdog_stop.load(std::memory_order_acquire)) {
        // 2s of stall detection, but sampled at 20ms so the allocator check
        // lands close to whatever broke it.
        for (int i = 0; i < 100 && !g_watchdog_stop.load(std::memory_order_acquire); i++) {
            svcSleepThread(20000000ULL);   // 20ms
            arenaMonitorTick();
        }
        if (g_watchdog_stop.load(std::memory_order_acquire)) break;
        uint64_t f = g_main_frames.load(std::memory_order_relaxed);
        if (f == last) {
            char buf[224];
            unsigned long a_m = 0, a_c = 0, a_r = 0, a_f = 0;
            shimAllocCounts(&a_m, &a_c, &a_r, &a_f);
            // compatLogRaw, not compatLog: if the main thread wedged while
            // holding the log mutex, the ordinary path would block here too
            // and the watchdog would die silently along with it.
            snprintf(buf, sizeof(buf),
                     "WATCHDOG: main thread has not advanced for %ds — "
                     "wedged in '%s' (frame %llu, %d ctor faults; libc m=%lu c=%lu r=%lu f=%lu)",
                     (stuck + 1) * 2, g_main_phase.load(std::memory_order_relaxed),
                     (unsigned long long)f, elfGetCtorFaultCount(),
                     a_m, a_c, a_r, a_f);
            compatLogRaw(buf);
            if (stuck == 0) probeAllocator();
            stuck++;
        } else {
            stuck = 0;
        }
        last = f;
    }
}

// Progress callback — called from loader thread.
// Updates shared state only; never touches SDL (wrong thread).
static void progressCallback(const char* stage, const char* /*detail*/) {
    if (stage) {
        strncpy(g_ui_stage, stage, sizeof(g_ui_stage) - 1);
        g_ui_stage[sizeof(g_ui_stage) - 1] = '\0';
    }
    // Mirror it to the overlay. Rate-limited inside, and a no-op when nothing
    // changed, so this stays cheap enough to sit in the progress path.
    if (g_loader_ctx)
        installStatusWrite("installing", g_loader_ctx->pkg_name.c_str(),
                           g_loader_ctx->app_name.c_str(), g_ui_stage, g_ui_pct);
}

static void loaderThreadFn(void*) {
    g_loader_ctx->result = launchApk(
        g_loader_ctx->apk_path,
        g_loader_ctx->pkg_name,
        progressCallback,
        g_loader_ctx->skip_install
    );
    // Brain It On leaves "ELF: loading complete" as the very last line in the
    // log. That call is four lines from the end of launchApk, so the only
    // things between it and here are the result assignment, the return, and
    // the destructors of launchApk's locals — a window small enough that
    // knowing which SIDE of it we're on identifies the problem. If this line
    // appears and the main thread still never proceeds, the hang is in the
    // handoff; if it doesn't, launchApk never returned.
    compatLog("loader: launchApk returned — signalling the main thread");
    compatLogFlush();
    g_loader_ctx->done.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
struct App {
    SDL_Window*    win  = nullptr;
    SDL_Renderer*  rdr  = nullptr;
    TTF_Font*      fLg  = nullptr;
    TTF_Font*      fMd  = nullptr;  // 20px — monospace-ish for log lines
    TTF_Font*      fSm  = nullptr;
    SDL_Joystick*  joy  = nullptr;

    std::vector<ApkInfo>      apks;
    std::vector<SDL_Texture*> icons;
    int selected = 0;
    int scroll   = 0;

    SDL_Texture* avatarTex = nullptr;

    // ── Scenery: cached sky/planet texture + animated starfield ─────────
    SDL_Texture* bgTex = nullptr;
    TTF_Font*    fBtn  = nullptr;  // NintendoExt shared font: HOS button glyphs
    struct Star { float x, y; int sz; float phase, speed; };
    std::vector<Star> stars;
    float selAnimY = -1.0f;        // eased focus-card position (borealis-style)

    // ── Loading screen + reveal ─────────────────────────────────────────
    // The artwork of the reveal itself is drawn by compat/reveal.cpp, which
    // needs no assets — it is vectors, not a logo texture. What is kept here is
    // the loading screen's backdrop and the fonts both screens caption with.
    SDL_Texture* bootBgTex = nullptr;  // radial-gradient backdrop
    TTF_Font*    fBootT = nullptr;     // 15px caps title
    TTF_Font*    fBootS = nullptr;     // 13px caps status
    TTF_Font*    fBootF = nullptr;     // 11px caps footer
    TTF_Font*    fMark  = nullptr;     // 20px caps — the reveal's caption
    bool         bootReady   = false;
    std::string  launchTitle;          // game name shown under the gem
    SDL_Texture* gameIconTex = nullptr;// launching game's icon (revealed by the gem)
    bool         showLogPanel = false; // Y toggles the compat_log feed back on

    // One-shot README screenshot flags (each screen captured once per run)
    bool shotMenu = false, shotLoading = false, shotResult = false, shotAbout = false;
    bool shotReveal = false;

    // A game session leaves JIT regions and worker threads behind that we
    // can't fully unload yet — a second launch reads garbage ("not an ARM
    // binary"). Block relaunch until the app is restarted.
    bool   gameRanOnce = false;
    Uint32 noticeUntil = 0;
    std::string noticeText = "One game session per launch for now — restart Viridite to play again";

    // Save the composed frame (call just before SDL_RenderPresent) as a PNG in
    // sdmc:/Viridite/screenshots/ — showcase material for the README.
    void saveScreenshot(const char* name) {
        mkdir("sdmc:/Viridite/screenshots", 0777);
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(
            0, SW, SH, 32, SDL_PIXELFORMAT_ABGR8888);
        if (!s) return;
        if (SDL_RenderReadPixels(rdr, nullptr, s->format->format,
                                 s->pixels, s->pitch) == 0) {
            char path[128];
            snprintf(path, sizeof(path), "sdmc:/Viridite/screenshots/%s", name);
            if (IMG_SavePNG(s, path) == 0) logMsg(path);
        }
        SDL_FreeSurface(s);
    }

    // ------------------------------------------------------------------
    TTF_Font* openFont(int ptsize) {
        plInitialize(PlServiceType_User);
        PlFontData fd = {};
        if (plGetSharedFontByType(&fd, PlSharedFontType_Standard) == 0 && fd.size > 0) {
            SDL_RWops* rw = SDL_RWFromConstMem(
                fd.address, (int)fd.size);
            TTF_Font* f = TTF_OpenFontRW(rw, 1, ptsize);
            if (f) { logMsg("  font: system BFTTF"); return f; }
            logSDL("  BFTTF open failed");
        }
        romfsInit();
        TTF_Font* f = TTF_OpenFont("romfs:/fonts/DejaVuSans.ttf", ptsize);
        if (f) { logMsg("  font: romfs DejaVuSans"); return f; }
        logSDL("  romfs font open failed");
        return nullptr;
    }

    // NintendoExt shared font: circled A/B/X/Y/+/- button glyphs (U+E0E0…).
    // Returns nullptr on failure — callers fall back to plain-text hints.
    TTF_Font* openExtFont(int ptsize) {
        PlFontData fd = {};
        if (plGetSharedFontByType(&fd, PlSharedFontType_NintendoExt) == 0 && fd.size > 0) {
            SDL_RWops* rw = SDL_RWFromConstMem(
                fd.address, (int)fd.size);
            TTF_Font* f = TTF_OpenFontRW(rw, 1, ptsize);
            if (f) { logMsg("  font: NintendoExt glyphs"); return f; }
        }
        logMsg("  NintendoExt font unavailable — text hints");
        return nullptr;
    }

    // ------------------------------------------------------------------
    bool init() {
        mkdir("sdmc:/Viridite", 0777);
        logOpen();
        logMsg("Viridite starting");
        socketInitializeDefault();

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
            logSDL("SDL_Init failed"); logClose(); return false;
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP) == 0)
            logSDL("IMG_Init warning");
        if (TTF_Init() != 0) {
            logSDL("TTF_Init failed"); logClose(); return false;
        }

        win = SDL_CreateWindow("Viridite",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            SW, SH, SDL_WINDOW_SHOWN);
        if (!win) { logSDL("CreateWindow failed"); logClose(); return false; }

        rdr = SDL_CreateRenderer(win, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!rdr) {
            logSDL("Accelerated renderer failed, trying software");
            rdr = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!rdr) { logSDL("CreateRenderer failed"); logClose(); return false; }

        SDL_SetRenderDrawBlendMode(rdr, SDL_BLENDMODE_BLEND);

        fLg = openFont(28);
        fMd = openFont(20);
        fSm = openFont(17);
        if (!fLg || !fSm) { logMsg("Font load failed"); logClose(); return false; }
        if (!fMd) fMd = fSm;
        fBtn = openExtFont(22);

        buildBackground();

        if (SDL_NumJoysticks() > 0) {
            joy = SDL_JoystickOpen(0);
            if (!joy) logSDL("JoystickOpen warning");
        }
        logMsg("init complete");
        return true;
    }

    // ------------------------------------------------------------------
    void cleanup() {
        avatarStop();
        if (avatarTex) SDL_DestroyTexture(avatarTex);
        if (bgTex) SDL_DestroyTexture(bgTex);
        for (auto* t : icons) if (t) SDL_DestroyTexture(t);
        // Boot-animation assets
        if (gameIconTex) SDL_DestroyTexture(gameIconTex);
        if (bootBgTex)  SDL_DestroyTexture(bootBgTex);
        reveal::release();
        if (fBootT && fBootT != fSm) TTF_CloseFont(fBootT);
        if (fBootS && fBootS != fSm) TTF_CloseFont(fBootS);
        if (fBootF && fBootF != fSm) TTF_CloseFont(fBootF);
        if (fMark  && fMark  != fSm) TTF_CloseFont(fMark);
        if (fBtn) TTF_CloseFont(fBtn);
        if (fLg)  TTF_CloseFont(fLg);
        if (fMd && fMd != fSm) TTF_CloseFont(fMd);
        if (fSm)  TTF_CloseFont(fSm);
        if (joy)  SDL_JoystickClose(joy);
        if (rdr)  SDL_DestroyRenderer(rdr);
        if (win)  SDL_DestroyWindow(win);
        sensorsShutdown();   // console IMU must be finalized before hidExit
        socketExit();
        romfsExit(); plExit();
        TTF_Quit(); IMG_Quit(); SDL_Quit();
        logMsg("cleanup done");
        logClose();
    }

    // ------------------------------------------------------------------
    void fill(int x, int y, int w, int h, SDL_Color c) {
        SDL_SetRenderDrawColor(rdr, c.r, c.g, c.b, c.a);
        SDL_Rect r = {x, y, w, h};
        SDL_RenderFillRect(rdr, &r);
    }

    // Filled rounded rectangle (centre + edges + four corner quarter-discs).
    void fillRoundedRect(int x, int y, int w, int h, int rad, SDL_Color c) {
        if (rad * 2 > w) rad = w / 2;
        if (rad * 2 > h) rad = h / 2;
        fill(x + rad, y, w - 2 * rad, h, c);         // centre column
        fill(x, y + rad, rad, h - 2 * rad, c);       // left edge
        fill(x + w - rad, y + rad, rad, h - 2 * rad, c); // right edge
        SDL_SetRenderDrawColor(rdr, c.r, c.g, c.b, c.a);
        for (int dy = 0; dy < rad; dy++) {
            int dx = (int)(sqrtf((float)(rad * rad - (rad - dy) * (rad - dy))) + 0.5f);
            SDL_Rect top = {x + rad - dx, y + dy, 2 * dx + (w - 2 * rad), 1};
            SDL_Rect bot = {x + rad - dx, y + h - 1 - dy, 2 * dx + (w - 2 * rad), 1};
            SDL_RenderFillRect(rdr, &top);
            SDL_RenderFillRect(rdr, &bot);
        }
    }

    // Rendered-glyph cache.
    //
    // drawTrackedCentered draws letter by letter so it can apply tracking, and
    // this used to rasterise and upload a fresh GPU texture for every glyph on
    // every frame, then destroy it — title, status line and footer together are
    // around fifty textures per frame, thousands within a few seconds of
    // loading. Each one is an nvmap allocation on Switch, and that much churn
    // is both wasteful and a good way to run the graphics allocator out of
    // something while the loader thread is competing for the same memory.
    //
    // The text is drawn from a small alphabet, so caching by glyph collapses
    // that to a few dozen textures created once. Only short strings are cached,
    // which keeps whole-line callers (the log panel's changing text) out of it,
    // and the cache is capped so a pathological caller cannot grow it forever.
    struct GlyphTex { SDL_Texture* tex; int w, h; };
    std::map<std::string, GlyphTex> glyphCache;
    // Advance widths, keyed by font and glyph only — width does not depend on
    // colour. TTF_SizeUTF8 converts the UTF-8 it is handed before measuring,
    // and that conversion allocates, so measuring a caption glyph by glyph
    // every frame is as much of an allocation storm as rendering it was. This
    // is the last one on the loading screen's path.
    std::map<std::string, int> widthCache;

    int glyphWidth(TTF_Font* f, const char* g) {
        char kb[48];
        snprintf(kb, sizeof(kb), "%p|%s", (void*)f, g);
        auto it = widthCache.find(kb);
        if (it != widthCache.end()) return it->second;
        int w = 0, h = 0;
        TTF_SizeUTF8(f, g, &w, &h);
        if (widthCache.size() < 1024) widthCache.emplace(kb, w);
        return w;
    }
    static constexpr size_t GLYPH_CACHE_MAX = 512;

    int drawText(TTF_Font* f, const std::string& s, SDL_Color col, int x, int y) {
        if (s.empty() || !f) return 0;

        const bool cacheable = s.size() <= 8 && glyphCache.size() < GLYPH_CACHE_MAX;
        std::string key;
        if (cacheable) {
            char kb[64];
            snprintf(kb, sizeof(kb), "%p|%02x%02x%02x%02x|", (void*)f, col.r, col.g, col.b, col.a);
            key = kb; key += s;
            auto it = glyphCache.find(key);
            if (it != glyphCache.end()) {
                SDL_Rect dst = {x, y, it->second.w, it->second.h};
                SDL_RenderCopy(rdr, it->second.tex, nullptr, &dst);
                return it->second.w;
            }
        }

        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), col);
        if (!surf) return 0;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(rdr, surf);
        int w = surf->w;
        SDL_FreeSurface(surf);
        if (!tex) return 0;
        int tw, th;
        SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
        SDL_Rect dst = {x, y, tw, th};
        SDL_RenderCopy(rdr, tex, nullptr, &dst);
        if (cacheable) glyphCache.emplace(key, GlyphTex{tex, tw, th});
        else           SDL_DestroyTexture(tex);
        return w;
    }

    // ── Scenery ─────────────────────────────────────────────────────────
    // The icon look: deep-space gradient sky, twinkling stars, and a green
    // planet horizon rising from the bottom edge with a glowing rim.
    // Sky + planet are static → rendered once into bgTex; stars animate live.

    static SDL_Color lerpCol(SDL_Color a, SDL_Color b, float t) {
        return { (Uint8)(a.r + (b.r - a.r) * t), (Uint8)(a.g + (b.g - a.g) * t),
                 (Uint8)(a.b + (b.b - a.b) * t), 255 };
    }

    void fillCircle(int cx, int cy, int r, SDL_Color c) {
        SDL_SetRenderDrawColor(rdr, c.r, c.g, c.b, c.a);
        for (int dy = -r; dy <= r; dy++) {
            int hw = (int)sqrtf((float)(r * r - dy * dy));
            SDL_Rect row = {cx - hw, cy + dy, hw * 2, 1};
            SDL_RenderFillRect(rdr, &row);
        }
    }

    static constexpr float PLANET_R    = 2200.0f;  // huge circle → gentle curve
    static constexpr int   PLANET_BUMP = 130;      // rim height above bottom edge

    void buildBackground() {
        // Light theme: a scatter of faint green motes on the near-white ground
        // (no dark sky/planet SVG). bgTex stays null so drawBackground fills C_BG.
        stars.clear();
        uint32_t rng = 0x5EED5EED;
        auto rnd = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        for (int i = 0; i < 60; i++) {
            Star s;
            s.x     = (float)(rnd() % SW);
            s.y     = (float)(rnd() % SH);
            s.sz    = (rnd() % 100 < 22) ? 3 : 2;
            s.phase = (rnd() % 628) / 100.0f;
            s.speed = 0.25f + (rnd() % 100) / 120.0f;
            stars.push_back(s);
        }
    }

    void drawBackground() {
        Uint32 now = SDL_GetTicks();
        fill(0, 0, SW, SH, C_BG);
        for (auto& s : stars) {
            s.x -= 0.02f * s.speed;
            if (s.x < 0) s.x += SW;
            float tw = 0.5f + 0.5f * sinf(now / 1000.0f * s.speed * 6.2832f + s.phase);
            Uint8 a  = (Uint8)(12 + 24 * tw);   // faint green motes on white
            fill((int)s.x, (int)s.y, s.sz, s.sz, {0, 190, 110, a});
        }
    }

    // Shared translucent header: "Viridite" with green accent + rim line
    void drawHeaderBar(const std::string& rightText = "") {
        fill(0, 0, SW, HEADER_H, {255, 255, 255, 235});
        fill(0, HEADER_H - 3, SW, 3, C_RIM);
        int w = drawText(fLg, "Virid", C_WHITE, 30, (HEADER_H - 28) / 2);
        w += drawText(fLg, "ite", C_OK, 30 + w, (HEADER_H - 28) / 2);
        drawText(fSm, BUILD_VERSION, C_DIM, 30 + w + 14, (HEADER_H + 4) / 2);
        if (!rightText.empty()) {
            int tw = 0, th = 0;
            TTF_SizeUTF8(fSm, rightText.c_str(), &tw, &th);
            drawText(fSm, rightText, C_DIM, SW - tw - 30, (HEADER_H - 18) / 2);
        }
    }

    // HOS-style footer hints, right-aligned: {glyph-utf8-or-letter, label}.
    // With the NintendoExt font the glyph IS the circled button; otherwise we
    // draw our own chip with the letter.
    void drawFooterBar(const std::vector<std::pair<std::string, std::string>>& hints,
                       const std::string& leftText = "") {
        fill(0, SH - FOOTER_H, SW, FOOTER_H, {242, 248, 245, 235});
        fill(0, SH - FOOTER_H, SW, 2, C_RIM);
        int cy = SH - FOOTER_H / 2;
        if (!leftText.empty())
            drawText(fSm, leftText, C_WARN, 30, cy - 9);
        int x = SW - 30;
        for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
            int lw = 0, lh = 0;
            TTF_SizeUTF8(fSm, it->second.c_str(), &lw, &lh);
            x -= lw;
            drawText(fSm, it->second, C_GRAY, x, cy - lh / 2);
            x -= 8;
            if (fBtn && it->first.size() > 1) {   // real HOS glyph
                int gw = 0, gh = 0;
                TTF_SizeUTF8(fBtn, it->first.c_str(), &gw, &gh);
                x -= gw;
                drawText(fBtn, it->first, C_WHITE, x, cy - gh / 2);
            } else {                              // fallback chip
                x -= 26;
                fillCircle(x + 13, cy, 13, {205, 244, 224, 255});
                std::string letter = it->first.size() > 1 ? "?" : it->first;
                int gw = 0, gh = 0;
                TTF_SizeUTF8(fSm, letter.c_str(), &gw, &gh);
                drawText(fSm, letter, C_WHITE, x + 13 - gw / 2, cy - gh / 2);
            }
            x -= 34;
        }
    }

    // HOS button glyphs in the NintendoExt shared font
    static constexpr const char* GLYPH_A     = "\xEE\x83\xA0";  // U+E0E0
    static constexpr const char* GLYPH_B     = "\xEE\x83\xA1";  // U+E0E1
    static constexpr const char* GLYPH_X     = "\xEE\x83\xA2";  // U+E0E2
    static constexpr const char* GLYPH_Y     = "\xEE\x83\xA3";  // U+E0E3
    static constexpr const char* GLYPH_PLUS  = "\xEE\x83\xAF";  // U+E0EF (+)
    static constexpr const char* GLYPH_MINUS = "\xEE\x83\xB0";  // U+E0F0 (-)

    // Pick the HOS glyph when the ext font loaded, else the plain letter
    // (drawFooterBar renders single-char hints as its own chip).
    std::string BG(const char* glyph, const char* letter) const {
        return fBtn ? glyph : letter;
    }

    static std::string formatSize(uint64_t bytes) {
        char buf[32];
        if (bytes >= 1024ull * 1024 * 1024)
            snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
        else if (bytes >= 1024ull * 1024)
            snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
        else if (bytes >= 1024ull)
            snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
        else
            snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
        return buf;
    }

    void drawMonogram(const std::string& name, int x, int y, int sz) {
        static const SDL_Color PALETTE[] = {
            {239, 83,  80,  255}, {171, 71,  188, 255}, {66,  165, 245, 255},
            {38,  166, 154, 255}, {255, 167, 38,  255}, {126, 87,  194, 255},
            {92,  107, 192, 255}, {255, 112, 67,  255},
        };
        uint32_t h = 2166136261u;
        for (char c : name) h = (h ^ (uint8_t)c) * 16777619u;
        SDL_Color bg = PALETTE[h % (sizeof(PALETTE) / sizeof(PALETTE[0]))];
        fill(x, y, sz, sz, bg);
        char letter = name.empty() ? '?' : (char)toupper((unsigned char)name[0]);
        std::string s(1, letter);
        int w = 0, h2 = 0;
        TTF_SizeUTF8(fLg, s.c_str(), &w, &h2);
        drawText(fLg, s, C_WHITE, x + (sz - w) / 2, y + (sz - h2) / 2);
    }

    std::string clamp(TTF_Font* f, const std::string& s, int maxW) {
        int w = 0, h = 0;
        TTF_SizeUTF8(f, s.c_str(), &w, &h);
        if (w <= maxW) return s;
        std::string t = s;
        while (!t.empty()) {
            t.pop_back();
            std::string try_ = t + "...";
            TTF_SizeUTF8(f, try_.c_str(), &w, &h);
            if (w <= maxW) return try_;
        }
        return "...";
    }

    // ------------------------------------------------------------------
    void loadIcons() {
        icons.assign(apks.size(), nullptr);
        for (size_t i = 0; i < apks.size(); i++) {
            if (apks[i].iconPng.empty()) continue;
            SDL_RWops* rw = SDL_RWFromConstMem(
                apks[i].iconPng.data(), (int)apks[i].iconPng.size());
            SDL_Surface* surf = IMG_Load_RW(rw, 1);
            if (!surf) continue;
            icons[i] = SDL_CreateTextureFromSurface(rdr, surf);
            SDL_FreeSurface(surf);
            apks[i].iconPng.clear();
        }
    }

    void rescan() {
        for (auto* t : icons) if (t) SDL_DestroyTexture(t);
        icons.clear();
        apks = ::scanApks(APK_DIR);
        loadIcons();
        selected = 0; scroll = 0;
    }

    // ------------------------------------------------------------------
    void render() {
        Uint32 now = SDL_GetTicks();
        drawBackground();

        if (apks.empty()) {
            drawText(fSm,
                "No APKs found — place .apk files in sdmc:/Viridite/apks/",
                C_GRAY, 30, LIST_Y + 30);
        } else {
            // Focus card (borealis-style): eased position + pulsing green glow,
            // drawn under the row content.
            int targetY = LIST_Y + (selected - scroll) * ITEM_H;
            if (selAnimY < 0) selAnimY = (float)targetY;
            selAnimY += (targetY - selAnimY) * 0.35f;
            if (fabsf(selAnimY - targetY) < 0.5f) selAnimY = (float)targetY;
            {
                int cy2 = (int)selAnimY;
                float pulse = 0.5f + 0.5f * sinf(now / 1000.0f * 2.6f);
                SDL_Rect card = {12, cy2 + 4, SW - 24, ITEM_H - 8};
                fill(card.x, card.y, card.w, card.h, {205, 244, 224, 235});
                for (int g = 1; g <= 5; g++) {       // soft outer glow
                    Uint8 a = (Uint8)((60 - g * 10) * (0.55f + 0.45f * pulse));
                    SDL_SetRenderDrawColor(rdr, 0, 200, 100, a);
                    SDL_Rect gr = {card.x - g, card.y - g,
                                   card.w + 2 * g, card.h + 2 * g};
                    SDL_RenderDrawRect(rdr, &gr);
                }
                SDL_SetRenderDrawColor(rdr, 0, 200, 100,
                                       (Uint8)(160 + 95 * pulse));
                SDL_RenderDrawRect(rdr, &card);      // crisp focus border
                fill(card.x, card.y, 5, card.h, C_RIM);
            }

            int end = std::min((int)apks.size(), scroll + VISIBLE);
            for (int i = scroll; i < end; i++) {
                int iy = LIST_Y + (i - scroll) * ITEM_H;
                SDL_SetRenderDrawColor(rdr, C_DIV.r, C_DIV.g, C_DIV.b, 130);
                SDL_RenderDrawLine(rdr, 24, iy + ITEM_H - 1, SW - 24, iy + ITEM_H - 1);

                int iconY = iy + (ITEM_H - ICON_SZ) / 2;
                if (i < (int)icons.size() && icons[i]) {
                    SDL_Rect dst = {28, iconY, ICON_SZ, ICON_SZ};
                    SDL_RenderCopy(rdr, icons[i], nullptr, &dst);
                } else {
                    drawMonogram(apks[i].appName, 28, iconY, ICON_SZ);
                }

                int tx   = 28 + ICON_SZ + 16;
                int maxW = SW - tx - 40;
                drawText(fLg, clamp(fLg, apks[i].appName, maxW), C_WHITE, tx, iy + 14);

                if (apks[i].installed) {
                    static const std::string INST = "INSTALLED";
                    int bw = 0, bh = 0;
                    TTF_SizeUTF8(fSm, INST.c_str(), &bw, &bh);
                    int bx = SW - bw - 40;
                    fill(bx - 6, iy + 14, bw + 12, bh, {205, 244, 224, 220});
                    drawText(fSm, INST, C_INST, bx, iy + 14);
                }

                std::string pkgLine =
                    (apks[i].packageName.empty() ? apks[i].filename : apks[i].packageName);
                if (!apks[i].versionName.empty())
                    pkgLine += "  v" + apks[i].versionName;
                if (apks[i].fileSizeBytes > 0)
                    pkgLine += "  ·  " + formatSize(apks[i].fileSizeBytes);
                drawText(fSm, clamp(fSm, pkgLine, maxW), C_GRAY, tx, iy + 58);
            }
            if ((int)apks.size() > VISIBLE) {
                int barH = LIST_H * VISIBLE / (int)apks.size();
                int barY = LIST_Y + LIST_H * scroll / (int)apks.size();
                fill(SW - 6, barY, 6, barH, {150, 195, 172, 200});
            }
        }

        std::string cnt;
        if (!apks.empty())
            cnt = std::to_string(apks.size()) + (apks.size() == 1 ? " APK" : " APKs");
        drawHeaderBar(cnt);

        if (noticeUntil && now < noticeUntil) {
            const char* msg = noticeText.c_str();
            int w = 0, h = 0;
            TTF_SizeUTF8(fSm, msg, &w, &h);
            fill((SW - w) / 2 - 16, SH - FOOTER_H - 44, w + 32, 34, {253, 235, 238, 240});
            drawText(fSm, msg, C_WARN, (SW - w) / 2, SH - FOOTER_H - 36);
        }

        bool docked = appletGetOperationMode() == AppletOperationMode_Console;
        drawFooterBar({{BG(GLYPH_A, "A"), "Launch"}, {BG(GLYPH_X, "X"), "Reinstall"},
                       {BG(GLYPH_Y, "Y"), "Rescan"}, {BG(GLYPH_MINUS, "-"), "About"},
                       {BG(GLYPH_PLUS, "+"), "Quit"}},
                      docked ? "Docked — games need handheld (touch screen)" : "");

        if (!shotMenu && !apks.empty() && now > 3000) {  // icons + glow settled
            shotMenu = true;
            saveScreenshot("ui_menu.png");
        }
        SDL_RenderPresent(rdr);
    }

    // ------------------------------------------------------------------
    // Snapshot the last N lines from the in-memory detail ring buffer.
    // The detail buffer is written by every compatLog() call on the loader
    // thread — no file I/O, always fresh, works during silent phases too.
    // ------------------------------------------------------------------
    void snapDetailLog(std::vector<std::string>& out, int maxLines) {
        int head = g_detail_head;  // sample once
        int total = head < DETLOG_N ? head : DETLOG_N;
        int show  = total < maxLines ? total : maxLines;
        out.clear();
        out.reserve(show);
        for (int i = show - 1; i >= 0; i--) {
            int slot = ((head - 1 - i) % DETLOG_N + DETLOG_N) % DETLOG_N;
            if (g_detail_log[slot][0])
                out.push_back(std::string(g_detail_log[slot]));
        }
    }
    static const int DETLOG_N = 28;

    // ── Boot-animation assets ───────────────────────────────────────────
    // Colours lifted straight from the .dc.html design.
    static SDL_Color bootAccent()  { return {0,   168, 84,  255}; }  // #00A854
    static SDL_Color bootTitle()   { return {0,   117, 63,  255}; }  // #00753F
    static SDL_Color bootDot()     { return {105, 240, 174, 255}; }  // #69F0AE

    // radial-gradient(ellipse 70% 60% at 50% 42%, #FFF, #F2FBF6 55%, #E4F5EC)
    SDL_Texture* makeRadialBg() {
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, SW, SH, 32, SDL_PIXELFORMAT_RGBA32);
        if (!s) return nullptr;
        const SDL_Color c0 = {255,255,255,255}, c1 = {242,251,246,255}, c2 = {228,245,236,255};
        const float cx = SW * 0.50f, cy = SH * 0.42f, rx = SW * 0.70f, ry = SH * 0.60f;
        for (int y = 0; y < SH; y++) {
            Uint32* row = (Uint32*)((Uint8*)s->pixels + y * s->pitch);
            for (int x = 0; x < SW; x++) {
                float nx = (x + 0.5f - cx) / rx, ny = (y + 0.5f - cy) / ry;
                float d  = sqrtf(nx * nx + ny * ny);
                if (d > 1.0f) d = 1.0f;
                SDL_Color c = (d < 0.55f) ? lerpCol(c0, c1, d / 0.55f)
                                          : lerpCol(c1, c2, (d - 0.55f) / 0.45f);
                row[x] = SDL_MapRGBA(s->format, c.r, c.g, c.b, 255);
            }
        }
        SDL_Texture* t = SDL_CreateTextureFromSurface(rdr, s);
        SDL_FreeSurface(s);
        return t;
    }

    void buildBootAssets() {
        if (bootReady) return;
        bootReady = true;
        romfsInit();

        bootBgTex = makeRadialBg();

        fBootT = openFont(15);
        fBootS = openFont(13);
        fBootF = openFont(11);
        fMark  = openFont(20);
        if (!fBootT) fBootT = fSm;
        if (!fBootS) fBootS = fSm;
        if (!fBootF) fBootF = fSm;
        if (!fMark)  fMark  = fSm;
    }

    // ── Tracked caps text (CSS letter-spacing) ──────────────────────────
    // Splits on UTF-8 boundaries so non-ASCII game titles don't get mangled.
    // Text helpers that do not allocate.
    //
    // These built a std::vector<std::string> with one string per letter, twice
    // per caption — once to measure and once to draw — plus a std::string for
    // the uppercase copy. Around fifty heap allocations a frame, from our own
    // code rather than SDL's, on the one screen that has to keep running while
    // the game's constructors are upsetting the allocator. Pre-rendering the
    // glyphs removed SDL_ttf's allocations and moved the wedge from frame 47 to
    // frame 160; these are what was left.
    static int utf8Len(const char* p, size_t remaining) {
        unsigned char c = (unsigned char)*p;
        int n = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3
              : (c >> 3) == 0x1E ? 4 : 1;
        return ((size_t)n > remaining) ? 1 : n;
    }
    int trackedWidth(TTF_Font* f, const char* s, int track) {
        if (!f || !s) return 0;
        size_t len = strlen(s);
        int total = 0;
        for (size_t i = 0; i < len;) {
            int n = utf8Len(s + i, len - i);
            char g[5] = {}; memcpy(g, s + i, (size_t)n);
            total += glyphWidth(f, g) + track;
            i += (size_t)n;
        }
        return total > 0 ? total - track : 0;
    }
    void drawTrackedCentered(TTF_Font* f, const char* s, SDL_Color col,
                             int cx, int y, int track) {
        if (!f || !s || !*s) return;
        size_t len = strlen(s);
        int x = cx - trackedWidth(f, s, track) / 2;
        for (size_t i = 0; i < len;) {
            int n = utf8Len(s + i, len - i);
            char g[5] = {}; memcpy(g, s + i, (size_t)n);
            i += (size_t)n;
            if (g[0] == ' ' && n == 1) { x += glyphWidth(f, " ") + track; continue; }
            x += drawText(f, g, col, x, y) + track;
        }
    }
    // Uppercase into a caller-supplied buffer; no string is created.
    static void upperAsciiInto(char* dst, size_t cap, const char* src) {
        if (!dst || !cap) return;
        size_t i = 0;
        for (; src && src[i] && i + 1 < cap; i++)
            dst[i] = (char)toupper((unsigned char)src[i]);
        dst[i] = '\0';
    }

    // ── The reveal ──────────────────────────────────────────────────────────
    //
    // Once the game is actually ready, the mark arrives, drops, breaks along its
    // seam and the game's own icon rises out of it — the closing beat of the
    // Origin Trailer, drawn rather than played back (see compat/reveal.cpp).
    //
    // It is a different screen from loading on purpose. Loading is an
    // application screen about the game; this is the logo's one moment, and a
    // logo cannot land if it has already been on screen for thirty seconds.
    Uint32 revealStart = 0;                // 0 = still loading
    float  revealSecs  = 3.1f;

    bool revealing(void) const { return revealStart != 0; }
    float revealT(void) const {              // 0..1 across the whole reveal
        if (!revealStart) return 0.0f;
        float t = (SDL_GetTicks() - revealStart) / 1000.0f / revealSecs;
        return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }

    // ------------------------------------------------------------------
    // Progress screen — fully animated, called at ~60fps from main thread.
    // Reads shared state written by the loader thread (g_ui_stage, g_ui_pct).
    // Y toggles the old compat_log.txt feed back on for debugging.
    // ------------------------------------------------------------------
    // The old developer view: live compat_log.txt feed. The boot animation
    // replaced it as the default, but a log-driven project shouldn't lose the
    // ability to watch a stall happen — Y brings it back over the animation.
    void drawLogPanel() {
        const int LH = 21, N_SHOW = 13;
        const int BOX_X = 30, BOX_W = SW - 60;
        const int BOX_H = LH * (N_SHOW + 1) + 14;
        const int y0 = SH - FOOTER_H - BOX_H - 16;

        fill(BOX_X, y0, BOX_W, BOX_H, {247, 251, 249, 245});
        fill(BOX_X, y0, BOX_W, LH + 4, {237, 244, 240, 250});
        drawText(fSm, "  compat_log.txt", {120, 140, 225, 255}, BOX_X + 8, y0 + 3);
        drawText(fSm, "Y: hide", C_DIM, BOX_X + BOX_W - 70, y0 + 3);

        std::vector<std::string> logLines;
        snapDetailLog(logLines, N_SHOW);

        const SDL_Color C_LOG     = {125, 150, 230, 255};
        const SDL_Color C_LOG_NEW = {40,  60,  90,  255};
        const SDL_Color C_LOG_WARN= {176, 120, 0,   255};
        const SDL_Color C_LOG_ERR = {214, 48,  79,  255};

        int startIdx = (int)logLines.size() > N_SHOW ? (int)logLines.size() - N_SHOW : 0;
        int liy = y0 + LH + 8;
        for (int i = startIdx; i < (int)logLines.size(); i++) {
            bool isLast = (i == (int)logLines.size() - 1);
            const std::string& ln = logLines[i];
            SDL_Color c = isLast ? C_LOG_NEW : C_LOG;
            if (!isLast) {
                if (ln.find("FAULT") != std::string::npos ||
                    ln.find("fail")  != std::string::npos ||
                    ln.find("ERR")   != std::string::npos)
                    c = C_LOG_ERR;
                else if (ln.find("WARN") != std::string::npos ||
                         ln.find("warn") != std::string::npos)
                    c = C_LOG_WARN;
            }
            drawText(fMd ? fMd : fSm,
                     clamp(fMd ? fMd : fSm, (isLast ? "> " : "  ") + ln, BOX_W - 24),
                     c, BOX_X + 10, liy);
            liy += LH;
        }
    }

    // ------------------------------------------------------------------
    // Progress screen — the Viridite boot animation, called at ~60fps from the
    // main thread. Reads shared state written by the loader thread (g_ui_stage,
    // g_ui_pct). Y toggles the compat_log.txt feed back on for debugging.
    // ------------------------------------------------------------------
    // ── The loading screen ──────────────────────────────────────────────────
    //
    // Deliberately not the logo. The reveal at the end is the logo's moment, and
    // it cannot land if the same artwork has been on screen for the previous
    // thirty seconds — so loading is an application screen instead: the game
    // being loaded, named, with an honest account of how far along it is.
    //
    // Left-aligned, because the reveal is centred and the cut between them
    // should read as a change of scene rather than a change of contents.
    void drawLoadingScreen(Uint32 now, Uint32 elapsed_s) {
        mainPhase("loading/layout");

        const int PAD  = 96;
        const int ICON = 176;
        const int IY   = 232;
        const int TX   = PAD + ICON + 48;     // text column

        // ── The game's own icon, as the subject of the screen ──
        mainPhase("loading/icon");
        if (gameIconTex) {
            // A soft plate behind it, so a dark or transparent icon still reads
            // as a raised object rather than a hole.
            fillRoundedRect(PAD - 6, IY - 6, ICON + 12, ICON + 12, 44,
                            {0, 168, 84, 28});
            SDL_Rect id = {PAD, IY, ICON, ICON};
            SDL_RenderCopy(rdr, gameIconTex, nullptr, &id);
        } else {
            fillRoundedRect(PAD, IY, ICON, ICON, 40, {0, 168, 84, 46});
        }

        // ── Title ──
        mainPhase("loading/title");
        static char titleBuf[96];
        if (launchTitle.empty()) snprintf(titleBuf, sizeof titleBuf, "Loading");
        else                     snprintf(titleBuf, sizeof titleBuf, "%s", launchTitle.c_str());
        drawText(fBootT, titleBuf, {0, 60, 32, 255}, TX, IY + 18);

        // ── Stage, with the elapsed nudge kept from the old screen ──
        mainPhase("loading/stage");
        static char status[160];
        snprintf(status, sizeof status, "%s",
                 g_ui_stage[0] ? g_ui_stage : "Reading game data");
        if (elapsed_s >= 30) {
            size_t sl = strlen(status);
            snprintf(status + sl, sizeof status - sl, " — still working");
        }
        drawText(fBootS, status, {0, 102, 51, 160}, TX, IY + 74);

        // ── Percentage, right-aligned against the end of the bar ──
        const int BARX = TX;
        const int BARW = SW - TX - PAD;
        const int BARY = IY + 132;

        mainPhase("loading/pct");
        if (g_ui_pct > 0) {
            char pctBuf[8];
            const int pct = g_ui_pct > 100 ? 100 : g_ui_pct;
            snprintf(pctBuf, sizeof pctBuf, "%d%%", pct);
            int w = 0, h = 0;
            TTF_SizeUTF8(fBootS, pctBuf, &w, &h);
            drawText(fBootS, pctBuf, {0, 117, 63, 200}, BARX + BARW - w, IY + 74);
        }

        // ── Material 3 Expressive wavy progress indicator ──
        //
        // The completed portion is a travelling sine wave and the remainder is
        // a flat track, which is what makes it read as *moving* even when a
        // long stage leaves the percentage still. Amplitude eases out as it
        // fills, so the finish settles instead of thrashing at 99%.
        mainPhase("loading/wave");
        const float pctF = (g_ui_pct > 0) ? (g_ui_pct > 100 ? 1.0f : g_ui_pct / 100.0f)
                                          : 0.0f;
        const float phase  = (now % 1400) / 1400.0f;      // one wavelength / 1.4s
        const float lambda = 56.0f;
        const float amp    = 5.0f * (1.0f - pctF * pctF); // calm at the end
        const SDL_Color TRACK = {0, 168, 84, 40};
        const SDL_Color WAVE_A = {0, 200, 83, 255}, WAVE_B = {0, 117, 63, 255};

        int filled = (int)(BARW * pctF + 0.5f);
        if (g_ui_pct <= 0) {
            // Indeterminate: a short wave packet travelling the whole track,
            // rather than a bar pretending to know a number it has not got.
            const int PKT = (int)(BARW * 0.28f);
            int sx = (int)((now % 1800) / 1800.0f * (BARW + PKT)) - PKT;
            for (int i = 0; i < BARW; i++) {
                const bool inPkt = (i >= sx && i < sx + PKT);
                const float y = amp * sinf(6.2831853f * (i / lambda - phase));
                if (inPkt) {
                    const float k = (float)(i - sx) / PKT;
                    const float env = sinf(k * 3.1415926f);   // fade both ends
                    SDL_Color c = lerpCol(WAVE_A, WAVE_B, k);
                    c.a = (Uint8)(255.0f * env);
                    fill(BARX + i, BARY + (int)(y * env) - 2, 1, 4, c);
                } else {
                    fill(BARX + i, BARY - 1, 1, 2, TRACK);
                }
            }
        } else {
            for (int i = 0; i < BARW; i++) {
                if (i < filled) {
                    const float y = amp * sinf(6.2831853f * (i / lambda - phase));
                    fill(BARX + i, BARY + (int)y - 2, 1, 4,
                         lerpCol(WAVE_A, WAVE_B, i / (float)BARW));
                } else {
                    fill(BARX + i, BARY - 1, 1, 2, TRACK);
                }
            }
            // M3 puts a stop dot at the far end of the track; it gives the bar
            // a definite length, so partial progress reads against a whole.
            fillCircle(BARX + BARW - 2, BARY, 2, {0, 168, 84, 90});
        }

        // ── Wordmark, small and out of the way. It is not the subject here. ──
        mainPhase("loading/wordmark");
        const int wmW = trackedWidth(fBootF, "VIRIDITE", 3);
        drawTrackedCentered(fBootF, "VIRIDITE", {0, 102, 51, 64},
                            SW - PAD - wmW / 2, SH - 56, 3);
    }

    void showProgress() {
        Uint32 now = SDL_GetTicks();
        mainPhase("showProgress/buildBootAssets");
        buildBootAssets();
        mainPhase("showProgress/draw");

        // Track elapsed time per stage
        static char   s_stage[80] = {};
        static Uint32 s_stage_t   = 0;
        if (strncmp(g_ui_stage, s_stage, sizeof(s_stage)) != 0) {
            memcpy(s_stage, g_ui_stage, sizeof(s_stage));
            s_stage[sizeof(s_stage) - 1] = '\0';
            s_stage_t = now;
        }
        Uint32 elapsed_s = (now - s_stage_t) / 1000;

        if (revealing()) {
            // ── The reveal ──────────────────────────────────────────────────
            // The artwork is drawn by compat/reveal.cpp, which owns its own
            // paper and needs none of the backdrop above. The caption is drawn
            // here rather than in there so it goes through the glyph cache and
            // lands at native resolution instead of through the supersample.
            mainPhase("draw/reveal");
            const float rt = revealT();
            reveal::draw(rdr, rt, gameIconTex);

            const float capA = reveal::captionAlpha(rt);
            if (capA > 0.004f) {
                mainPhase("draw/revealCaption");
                static char titleBuf[96];
                if (launchTitle.empty()) snprintf(titleBuf, sizeof(titleBuf), "READY");
                else                     upperAsciiInto(titleBuf, sizeof(titleBuf),
                                                        launchTitle.c_str());
                SDL_Color tc = bootTitle();
                tc.a = (Uint8)(255.0f * capA);
                drawTrackedCentered(fMark, titleBuf, tc, SW / 2, reveal::CAPTION_Y, 8);
            }

            mainPhase("draw/revealMark");
            drawTrackedCentered(fBootF, "VIRIDITE", {0, 102, 51, 77}, SW / 2, SH - 48, 3);

            // The last stretch dips to black, and bootFadeDraw() picks the
            // picture back up over the game's first frames — so the two screens
            // are joined by one continuous fade rather than a cut.
            const float k = reveal::blackAlpha(rt);
            if (k > 0.0f) fill(0, 0, SW, SH, {0, 0, 0, (Uint8)(255.0f * k)});

            if (!shotReveal && rt > 0.62f && rt < 0.80f) {
                shotReveal = true;
                mainPhase("showProgress/screenshot");
                saveScreenshot("ui_reveal.png");
            }
        } else {
            mainPhase("draw/backdrop");
            if (bootBgTex) SDL_RenderCopy(rdr, bootBgTex, nullptr, nullptr);
            else           fill(0, 0, SW, SH, C_BG);
            drawLoadingScreen(now, elapsed_s);

            // Inside the loading branch, so a run that never reported 40% does
            // not save a frame of the reveal under the loading screen's name.
            if (!shotLoading && g_ui_pct >= 40) {
                shotLoading = true;
                mainPhase("showProgress/screenshot");
                saveScreenshot("ui_loading.png");
            }
        }

        if (showLogPanel) { mainPhase("showProgress/logPanel"); drawLogPanel(); }

        mainPhase("showProgress/SDL_RenderPresent");
        SDL_RenderPresent(rdr);
        mainPhase("showProgress/done");
    }

    // ------------------------------------------------------------------
    // Run launchApk on a background thread while this method drives the
    // animated progress screen on the main thread at ~60fps.
    // ------------------------------------------------------------------
    LaunchResult runLaunch(const ApkInfo& apk, bool skipInstall) {
        std::string pkg = apk.packageName.empty() ? apk.filename : apk.packageName;

        // Detail log is in-memory — no cache to invalidate, always fresh

        // The boot animation captions itself with the game being loaded, and
        // shows the game's own icon (the gem "reveals" it).
        launchTitle = !apk.appName.empty() ? apk.appName
                    : (!apk.packageName.empty() ? apk.packageName : apk.filename);
        if (gameIconTex) { SDL_DestroyTexture(gameIconTex); gameIconTex = nullptr; }
        if (!apk.iconPng.empty()) {
            SDL_RWops* rw = SDL_RWFromConstMem(apk.iconPng.data(), (int)apk.iconPng.size());
            if (SDL_Surface* s = IMG_Load_RW(rw, 1)) {
                gameIconTex = SDL_CreateTextureFromSurface(rdr, s);
                SDL_FreeSurface(s);
            }
        }

        // Set initial stage before thread starts so first frame looks right
        const char* verb = skipInstall ? "Launching (cached)" : "Installing + Launching";
        strncpy(g_ui_stage, verb, sizeof(g_ui_stage) - 1);

        loaderSetScreenOrient(apk.screenOrient);

        // Open the achievement store before the game's code can reach a JNI
        // call. The first unlock can happen surprisingly early — some titles
        // fire a "welcome" achievement in their first scene — and one that
        // arrives before the store is open would be dropped.
        achievements::init(pkg.c_str());

        LoaderCtx ctx;
        ctx.apk_path    = apk.path;
        ctx.pkg_name    = pkg;
        ctx.app_name    = launchTitle;
        ctx.skip_install = skipInstall;
        g_loader_ctx    = &ctx;

        // The loader thread is pure CPU work (APK unzip, ELF relocation, JIT
        // dual-mapping) with nothing meaningful on screen but our own idle
        // progress UI — no GPU work competing for clocks. Real Switch titles
        // use this exact FastLoad boost mode during their own load screens
        // for the same reason. Reset back to Normal before the game (which
        // *does* need real GPU clocks) ever gets control.
        appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);

        Thread t;
        // Pre-render every glyph the loading screen can draw, before the
        // loader thread starts.
        //
        // The main thread wedges because it allocates: SDL_ttf mallocs for any
        // glyph it has not cached, and once the game's constructors have upset
        // the allocator that allocation never returns. The glyph cache removed
        // most of them, but the first sighting of each character still
        // allocates, and the stage text gains new characters as loading
        // proceeds — at exactly the wrong moment.
        //
        // Warming the cache here means no allocations on the render path for
        // the whole of loading, so a damaged allocator cannot take down the
        // part of the process that was still working. It does not repair the
        // damage; it stops the damage spreading.
        {
            static const char* kWarm =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                "0123456789 .,:;!?'()[[]{}<>/|-_+=*&^%$#@";
            TTF_Font* fonts[] = { fBootT, fBootS, fBootF, fSm };
            const SDL_Color cols[] = { bootTitle(), {0, 102, 51, 140},
                                       {0, 102, 51, 77}, C_GRAY };
            int warmed = 0;
            for (size_t fi = 0; fi < sizeof(fonts)/sizeof(fonts[0]); fi++) {
                if (!fonts[fi]) continue;
                for (const char* c = kWarm; *c; c++) {
                    char g[2] = { *c, 0 };
                    // Drawn off-screen: what matters is the texture left in the
                    // cache, not the pixels.
                    drawText(fonts[fi], g, cols[fi], -1000, -1000);
                    glyphWidth(fonts[fi], g);   // and its measurement
                    warmed++;
                }
            }
            compatLogFmt("ui: pre-rendered %d glyphs — the loading screen will "
                         "not allocate again", warmed);
        }

        // 8MB, not 1MB.
        //
        // This thread runs the game's static initialisers — 421 of them for
        // Unity, which is not code written with a small stack in mind. Android
        // gives the main thread 8MB and Unity is built against that
        // expectation. On 1MB, a deep initialiser walks off the bottom of the
        // stack and into whatever is mapped below it, and both symptoms we have
        // follow from that one cause: the heap arena gets scribbled on, so
        // _free_r faults unlinking a chunk whose neighbour is now garbage
        // (155 times, always the same instruction), and SDL's own state is in
        // the same heap, so the render thread wedges in its next texture
        // allocation. It has never recovered, at any point, which is what
        // corrupted state looks like rather than contention.
        //
        // The two have always moved together — the main thread stops within a
        // second of the first constructor fault, across runs where that moment
        // ranged from three seconds to eight — and nothing else explains both.
        threadCreate(&t, loaderThreadFn, nullptr, nullptr,
                     0x800000 /*8MB stack — matches Android's main thread*/,
                     0x2C, 1 /*CPU 1*/);
        threadStart(&t);

        // Ask the CPU to catch the writer directly.
        //
        // A watchpoint on av->top traps the instruction that stores to it, which
        // is the one fact twenty releases of inference have failed to produce.
        // It is a privileged syscall, so hbloader may well refuse — the result
        // is logged either way, because "we did not get it" is worth knowing
        // rather than assuming.
        {
            extern void* __malloc_av_[];
            const u64 addr = (u64)(uintptr_t)&__malloc_av_[2];
            if (envIsSyscallHinted(0x6C)) {
                // DBGWCR: E=1, PAC=EL0(0b10), LSC=store(0b10), BAS=all 8 bytes.
                const u64 ctrl = (0xFFull << 5) | (0b10ull << 3) | (0b10ull << 1) | 1ull;
                Result rc = svcSetHardwareBreakPoint(0x10, ctrl, addr);
                compatLogFmt("monitor: watchpoint on av->top (%p) rc=0x%x", (void*)addr, rc);
            } else {
                compatLogFmt("monitor: svcSetHardwareBreakPoint not permitted — "
                             "sampling av->top every 20ms instead (%p)", (void*)addr);
            }
        }

        Thread wd;
        bool wdOk = R_SUCCEEDED(threadCreate(&wd, watchdogThreadFn, nullptr, nullptr,
                                             0x4000 /*16KB stack*/, 0x3B, 2 /*CPU 2*/)) &&
                    R_SUCCEEDED(threadStart(&wd));

        // Main thread render loop — keeps the UI alive until the loader finishes
        bool quitting = false;
        while (!ctx.done.load(std::memory_order_acquire) && !quitting) {
            mainPhase("waitLoop/SDL_PollEvent");
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) quitting = true;
                if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == BTN_PLUS) quitting = true;
                // Y peeks at the live compat_log.txt feed behind the animation
                if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == BTN_Y)
                    showLogPanel = !showLogPanel;
            }
            // If the user bails during an ARM32 run, stop the interpreter so the
            // loader thread can exit instead of the app appearing to hang.
            if (quitting) a32::requestAbort();
            showProgress();
            mainPhase("waitLoop/SDL_Delay");
            SDL_Delay(16); // ~60fps
            mainPhase("waitLoop/checkDone");
            g_main_frames.fetch_add(1, std::memory_order_relaxed);
        }

        // The loader has signalled; the main thread is past its wait loop. Two
        // uninstrumented things sit between here and the join, and Brain It On
        // stops somewhere in them: the log ends at the loader's own
        // "launchApk returned" and the join marker never appears. Bracketing
        // the final render separates "stuck waiting" from "stuck rendering".
        compatLog("loader: wait loop exited — rendering final frame");
        compatLogFlush();
        // Render a final frame so the last log line is visible
        showProgress();
        compatLog("loader: final frame rendered");
        compatLogFlush();

        // The loader thread must never wedge the console. If the user asked to
        // quit and it doesn't wind down promptly (e.g. a hung native game-init
        // that a fault-handler can't catch), force-exit the process — the OS
        // reclaims the stuck thread and drops back to the home menu, instead of
        // threadWaitForExit() blocking here forever.
        if (quitting && !ctx.done.load(std::memory_order_acquire)) {
            a32::requestAbort();
            Uint32 t0 = SDL_GetTicks();
            while (!ctx.done.load(std::memory_order_acquire)) {
                if (SDL_GetTicks() - t0 > 15000) {
                    compatLogFmt("loader still running 15s after quit — force-exiting to free the console");
                    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
                    exit(0);   // clean return to hbmenu (same path the game loop uses)
                }
                SDL_Event ev; while (SDL_PollEvent(&ev)) {}
                showProgress();
                SDL_Delay(16);
            }
        }

        // Brain It On stopped dead here: the loader logged "loading complete"
        // and "main/render thread tid=" never appeared, so the work finished
        // and the thread itself never terminated — most likely something a
        // faulted constructor left running or holding. These three lines make
        // the difference between "stuck joining" and "stuck starting the game"
        // visible in the log instead of inferable from an absence.
        compatLog("loader: work finished, joining loader thread");
        compatLogFlush();
        // The loader has already signalled done, so its work is over — but the
        // thread itself can still fail to terminate (Unity's constructors spawn
        // threads, and 155 of them faulted out through longjmp). Waiting
        // forever turns "the game didn't start" into "the console needs a power
        // hold", which is a far worse outcome than giving up on the join.
        {
            Uint32 t0 = SDL_GetTicks();
            bool exited = false;
            while (!exited) {
                // 100ms slices on the thread's own handle, so the UI keeps
                // drawing instead of freezing on an indefinite join.
                if (R_SUCCEEDED(svcWaitSynchronizationSingle(t.handle, 100000000ULL))) {
                    exited = true;
                    break;
                }
                if (SDL_GetTicks() - t0 > 10000) {
                    compatLog("loader: thread has not exited after 10s — "
                              "continuing without it rather than wedging");
                    compatLogFlush();
                    break;
                }
                SDL_Event ev; while (SDL_PollEvent(&ev)) {}
                showProgress();
            }
            if (exited) threadWaitForExit(&t);   // reap it properly
        }
        compatLog("loader: thread joined");
        compatLogFlush();
        installStatusWrite(ctx.result.game_so ? "done" : "error",
                           pkg.c_str(), launchTitle.c_str(),
                           ctx.result.game_so ? "Ready" : "Failed",
                           ctx.result.game_so ? 100 : 0);
        threadClose(&t);
        if (wdOk) {
            g_watchdog_stop.store(true, std::memory_order_release);
            svcCancelSynchronization(wd.handle);   // break the 2s sleep early
            threadWaitForExit(&wd);
            threadClose(&wd);
        }
        g_loader_ctx = nullptr;

        // FastLoad throttles the GPU to its minimum clock — fine while we're
        // just drawing a static progress bar, disastrous once real gameplay
        // starts. Always drop back to Normal here, even if loading failed.
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);

        // If game loaded OK (and the user didn't cancel), run it here on the main
        // thread (SDL2's EGL context is current on this thread, so GL calls reach
        // the screen).
        if (!quitting && ctx.result.game_so) {
            // The reveal, with the jingle. Its length follows the audio so the
            // two land together; if audio could not open, the default stands
            // and it simply plays silently.
            // Breadcrumbs. Everything from here to the game's first frame is
            // new, and a hard fault in it leaves no other trace — the logs
            // simply stop. Each stage announces itself so the last line written
            // names the one that failed.
            compatLog("boot: rendering jingle");
            compatLogFlush();
            if (jingle::play() && jingle::length() > 0.5f)
                revealSecs = jingle::length();
            compatLogFmt("boot: reveal starting (%.2fs)", revealSecs);
            compatLogFlush();

            // The renderer is normally vsynced, in which case SDL_RenderPresent
            // is already the frame wait and sleeping on top of it halves the
            // rate — which the reveal, unlike the loading screen, can be seen
            // to do: the drop and the shards both move far enough per frame for
            // 30fps to read as a stutter. Only sleep if nothing else is pacing
            // us (the software-renderer fallback).
            SDL_RendererInfo ri = {};
            const bool vsynced = SDL_GetRendererInfo(rdr, &ri) == 0 &&
                                 (ri.flags & SDL_RENDERER_PRESENTVSYNC);

            revealStart = SDL_GetTicks();
            while (revealT() < 1.0f) {
                SDL_Event ev; while (SDL_PollEvent(&ev)) {}
                showProgress();
                if (!vsynced) SDL_Delay(16);
            }
            revealStart = 0;
            jingle::stop();
            reveal::release();
            bootFadeBegin(0.75f);
            compatLog("boot: reveal done — handing the screen to the game");
            compatLogFlush();

            std::string base_dir = std::string("sdmc:/Viridite/games/") + pkg;
            compatLog("loader: handing off to the game on the main thread");
            compatLogFlush();
            if (ctx.result.is_arm32) {
                // A 32-bit game is driven by the interpreter, but from here —
                // this thread owns the GL context, and the guest's GL goes
                // through the bridge onto whatever is current.
                a32::runFrames();
            } else {
                runGameOnMainThread(ctx.result.game_so, win, ctx.apk_path, base_dir);
            }
            gameRanOnce = true;
        }

        return ctx.result;
    }

    // ------------------------------------------------------------------
    void showLaunchResult(const LaunchResult& res, int idx) {
        if (idx < 0 || idx >= (int)apks.size()) return;
        const ApkInfo& apk = apks[idx];

        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { done = true; }
                if (ev.type == SDL_JOYBUTTONDOWN &&
                    (ev.jbutton.button == BTN_B || ev.jbutton.button == BTN_PLUS)) { done = true; }
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) { done = true; }
            }

            drawBackground();
            drawHeaderBar();

            // Translucent result panel so text reads over the starfield
            fill(40, LIST_Y + 6, SW - 80, SH - LIST_Y - FOOTER_H - 12, {247, 251, 249, 220});
            {
                SDL_Rect panel = {40, LIST_Y + 6, SW - 80, SH - LIST_Y - FOOTER_H - 12};
                SDL_SetRenderDrawColor(rdr, res.ok ? 52 : 235, res.ok ? 230 : 90,
                                       res.ok ? 134 : 90, 200);
                SDL_RenderDrawRect(rdr, &panel);
            }

            int iconSz = 112;
            if (idx < (int)icons.size() && icons[idx]) {
                SDL_Rect dst = {(SW - iconSz) / 2, LIST_Y + 16, iconSz, iconSz};
                SDL_RenderCopy(rdr, icons[idx], nullptr, &dst);
            } else {
                drawMonogram(apk.appName, (SW - iconSz) / 2, LIST_Y + 16, iconSz);
            }

            int nameY = LIST_Y + 16 + iconSz + 12;
            {
                int w = 0, h = 0;
                std::string nm = clamp(fLg, apk.appName, SW - 80);
                TTF_SizeUTF8(fLg, nm.c_str(), &w, &h);
                drawText(fLg, nm, C_WHITE, (SW - w) / 2, nameY);
            }

            std::string statusStr = res.ok ? "Launch OK" : "Launch Failed";
            SDL_Color   statusCol = res.ok ? C_OK : C_ERR;
            {
                int w = 0, h = 0;
                TTF_SizeUTF8(fLg, statusStr.c_str(), &w, &h);
                drawText(fLg, statusStr, statusCol, (SW - w) / 2, nameY + 44);
            }

            int y = nameY + 100;
            if (!res.ok) {
                if (!res.errorStage.empty())
                    { drawText(fSm, "Failed at:  " + res.errorStage, C_WARN, 60, y); y += 28; }
                if (!res.errorDetail.empty())
                    { drawText(fSm, res.errorDetail, C_GRAY, 60, y); y += 28; }
            }
            if (res.unresolved > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "Unresolved symbols: %d  (may crash when those code paths execute)",
                         res.unresolved);
                drawText(fSm, buf, C_WARN, 60, y); y += 28;
            }
            if (res.svcPermCode != 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "JIT alloc: 0x%08X — code segment not executable", res.svcPermCode);
                drawText(fSm, buf, C_ERR, 60, y); y += 28;
                drawText(fSm,
                         "jitCreate/jitTransitionToExecutable failed. Needs Atmosphere CFW.",
                         C_GRAY, 60, y); y += 28;
            }
            y += 8;
            drawText(fSm, "Full log: sdmc:/Viridite/compat_log.txt", C_DIM, 60, y);

            drawFooterBar({{BG(GLYPH_B, "B"), "Back to menu"}, {BG(GLYPH_PLUS, "+"), "Menu"}});

            if (!shotResult) {
                shotResult = true;
                saveScreenshot("ui_result.png");
            }
            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }
    }

    // ------------------------------------------------------------------
    void showAbout() {
        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { done = true; }
                if (ev.type == SDL_JOYBUTTONDOWN &&
                    (ev.jbutton.button == BTN_B || ev.jbutton.button == BTN_MINUS))
                    { done = true; }
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                    { done = true; }
            }

            std::vector<uint8_t> img;
            if (avatarPollNewImage(img)) {
                SDL_RWops* rw = SDL_RWFromConstMem(img.data(), (int)img.size());
                SDL_Surface* surf = IMG_Load_RW(rw, 1);
                if (surf) {
                    if (avatarTex) SDL_DestroyTexture(avatarTex);
                    avatarTex = SDL_CreateTextureFromSurface(rdr, surf);
                    SDL_FreeSurface(surf);
                }
            }

            drawBackground();
            drawHeaderBar();

            int avSz = 160;
            int avX  = (SW - avSz) / 2;
            int avY  = LIST_Y + 30;
            if (avatarTex) {
                SDL_Rect dst = {avX, avY, avSz, avSz};
                SDL_RenderCopy(rdr, avatarTex, nullptr, &dst);
            } else {
                drawMonogram("Viridite", avX, avY, avSz);
                // Centred placeholder text below the monogram — only shown
                // for the one frame before the bundled avatar decodes
                static const std::string FETCH = "Loading avatar...";
                int fw = 0, fh = 0;
                TTF_SizeUTF8(fSm, FETCH.c_str(), &fw, &fh);
                drawText(fSm, FETCH, C_DIM, (SW - fw) / 2, avY + avSz + 8);
            }

            int y = avY + avSz + 40;
            auto center = [&](TTF_Font* f, const std::string& s, SDL_Color col) {
                int w = 0, h = 0;
                TTF_SizeUTF8(f, s.c_str(), &w, &h);
                drawText(f, s, col, (SW - w) / 2, y);
                y += h + 10;
            };
            center(fLg, "Viridite", C_WHITE);
            center(fSm, BUILD_VERSION, C_DIM);
            center(fSm, "by Viridite Contributors", C_GRAY);
            y += 10;
            center(fSm, "Android NDK compatibility layer for Nintendo Switch (HorizonOS)", C_GRAY);

            drawFooterBar({{BG(GLYPH_B, "B"), "Back to menu"}});

            if (!shotAbout && avatarTex) {  // wait for the avatar to arrive
                shotAbout = true;
                saveScreenshot("ui_about.png");
            }
            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }
    }
};

// ---------------------------------------------------------------------------
// Forwarder support — a small NRO (or Sphaira/hbmenu entry) can chain-load us
// with a package name via libnx's envSetNextLoad(path, argv), the same
// mechanism RetroArch forwarders use to boot straight into a ROM+core instead
// of RetroArch's own content browser. libnx's crt0 already parses the
// loader-provided argument block into plain argc/argv before main() runs, so
// no envGetArgv() plumbing is needed here — just read argv like any other
// command-line program.
//
// argv[0] MUST be this binary's own real path, NOT the package name. libnx's
// romfsInit() falls back to argv[0] to find and open its own .nro file on
// the SD card and read its embedded RomFS section — overwrite argv[0] with
// anything else and RomFS mounting silently fails, which took down every
// font load immediately (confirmed via this binary's own log.txt: "BFTTF
// open failed" + "romfs font open failed" for all three fonts, then "Font
// load failed" and an early exit — no compat_log.txt was ever opened,
// because that happens later in runLaunch(), well past where this failed).
// The caller (launcher/source/main.cpp) passes "<our own path> <package>"
// as the argv string, so the real argument lands at argv[1], same as any
// normal argv[0]-is-the-program-path command line.
// ---------------------------------------------------------------------------
// This binary is the ENGINE half of a two-part split: the Viridite launcher
// (kept as a separate, smaller NRO — see launcher/) shows the app list and
// chain-loads here via envSetNextLoad(path, argv) with a package name in
// argv[1] — the same mechanism the earlier single-binary "forwarder mode"
// used, just now the ALWAYS path instead of a fallback. This binary has no
// interactive picker of its own any more; it always expects a package name.
int main(int argc, char** argv) {
    // Before anything allocates. The walk runs forward from here, so anchoring
    // at the first game module left SDL's own blocks — including every texture
    // and font allocation the render thread later trips over — outside the
    // range being checked.
    shimHeapAnchor();
    App app;

    if (!app.init()) return 1;

    mkdir(APK_DIR, 0777);

    app.drawBackground();
    app.drawHeaderBar();
    app.drawText(app.fSm, "Scanning for APKs...", C_GRAY, 30, LIST_Y + 30);
    SDL_RenderPresent(app.rdr);

    app.apks = scanApks(APK_DIR);

    // log.txt (opened early in app.init(), unlike compat_log.txt which only
    // opens once launchApk() runs) — cheap insurance so a failure anywhere
    // between here and runLaunch() still leaves a trace of what argv held.
    char argvDbg[160];
    snprintf(argvDbg, sizeof(argvDbg), "core-x64: argc=%d argv[0]=%s argv[1]=%s",
             argc, (argc > 0 && argv[0]) ? argv[0] : "(none)",
             (argc > 1 && argv[1]) ? argv[1] : "(none)");
    logMsg(argvDbg);

    // ── Dry-run self-test ────────────────────────────────────────────────────
    // Invoked by the launcher with --selftest. Loads every installed game the
    // way a real launch would, stops before any game code executes, writes what
    // it found, and hands control straight back. Two handoffs and a launcher
    // restart, which is fast, and the alternative — duplicating the ELF loader
    // into the launcher so it could do this in-process — would mean the thing
    // being tested is not the thing that runs.
    if (argc > 2 && argv[1] && strcmp(argv[1], "--selftest") == 0) {
        // One game per invocation, named by the launcher.
        //
        // The first attempt dry-loaded every installed game in a single pass.
        // Brain It On loaded in a second with no unresolved symbols and then
        // the run stopped dead, because launchApk is not built to be called
        // twice in one process: the first game's JIT mappings, virtmem
        // reservations and Android TLS are all still held when the second load
        // starts. Testing the game you have selected is also the thing you
        // actually want, so this is both the correct design and the simpler
        // one.
        const std::string pkg = argv[2];
        logMsg(("core-x64: dry-run self-test for " + pkg).c_str());
        elfSetDryRun(true);

        FILE* out = fopen("sdmc:/Viridite/selftest_core.txt", "w");
        // Every line is flushed as it is written. The previous run left a
        // zero-byte file: stdio had buffered the results and the process never
        // reached fclose, so the one thing that would have said how far it got
        // was the thing that got lost.
        auto emit = [&](const char* fmt, ...) {
            if (!out) return;
            va_list va; va_start(va, fmt);
            vfprintf(out, fmt, va); va_end(va);
            fflush(out);
        };
        emit("v1\n");

        int idx = -1;
        for (size_t i = 0; i < app.apks.size(); i++) {
            const ApkInfo& a = app.apks[i];
            if ((a.packageName.empty() ? a.filename : a.packageName) == pkg) { idx = (int)i; break; }
        }
        if (idx < 0) {
            emit("FAIL|%s|not found in the APK list\n", pkg.c_str());
        } else if (!apkIsInstalled(pkg)) {
            emit("SKIP|%s|not installed — launch it once first\n", pkg.c_str());
        } else {
            const ApkInfo& a = app.apks[idx];
            app.launchTitle = a.appName.empty() ? pkg : a.appName;
            emit("INFO|%s|starting dry load\n", pkg.c_str());

            elfResetCounts();
            uint64_t t0 = armGetSystemTick();
            LaunchResult r = launchApk(a.path, pkg, nullptr, /*skip_install=*/true);
            uint32_t ms = (uint32_t)(((armGetSystemTick() - t0) * 1000) /
                                     armGetSystemTickFreq());
            if (r.game_so)
                emit("PASS|%s|loaded in %ums, %d unresolved symbol(s)\n",
                     pkg.c_str(), ms, elfGetUnresolvedCount());
            else
                emit("FAIL|%s|%s: %s\n", pkg.c_str(),
                     r.errorStage.empty() ? "load failed" : r.errorStage.c_str(),
                     r.errorDetail.c_str());
        }
        if (out) fclose(out);

        // hbmenu reported an error on the way back, and nothing in our code
        // prints that — so the hand-back itself was refused. Log the result
        // rather than assume it took.
        Result rcNext = envSetNextLoad("sdmc:/switch/Viridite.nro",
                                       "sdmc:/switch/Viridite.nro --selftest-results");
        char nb[96];
        snprintf(nb, sizeof(nb), "core-x64: dry run done, envSetNextLoad rc=0x%x", rcNext);
        logMsg(nb);
        app.cleanup();
        return 0;
    }

    const char* wantPkg = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : nullptr;
    int idx = -1;

    // The launcher records the exact file it chose (two builds of one game can
    // share a package id). Prefer that so we launch the right one.
    std::string wantPath;
    if (FILE* lm = fopen("sdmc:/Viridite/.launch_apk", "r")) {
        char b[512] = {0};
        if (fgets(b, sizeof(b), lm)) wantPath = b;
        fclose(lm);
        while (!wantPath.empty() && (wantPath.back() == '\n' || wantPath.back() == '\r')) wantPath.pop_back();
    }
    if (!wantPath.empty()) {
        for (size_t i = 0; i < app.apks.size(); i++)
            if (app.apks[i].path == wantPath) { idx = (int)i; break; }
        if (idx >= 0) logMsg(("core-x64: launch target -> " + wantPath).c_str());
    }
    if (idx < 0 && wantPkg) {
        for (size_t i = 0; i < app.apks.size(); i++)
            if (app.apks[i].packageName == wantPkg) { idx = (int)i; break; }
    }

    if (idx < 0) {
        // Launched with no/unknown package — this binary isn't meant to be
        // run directly. Say so plainly instead of showing a blank screen.
        compatLogFmt("core-x64: no valid package argument (got '%s') — this binary "
                     "is launched by the Viridite launcher, not directly",
                     wantPkg ? wantPkg : "(none)");
        app.drawBackground();
        app.drawHeaderBar();
        app.drawText(app.fLg, "Viridite Translation Core (x64)", C_WHITE, 30, LIST_Y + 30);
        app.drawText(app.fSm,
            "This is the game-loading engine, not the launcher — it needs a "
            "package name to run.", C_GRAY, 30, LIST_Y + 76);
        app.drawText(app.fSm,
            "Launch a game from the Viridite app list instead.",
            C_GRAY, 30, LIST_Y + 104);
        app.drawFooterBar({{app.BG(app.GLYPH_PLUS, "+"), "Quit"}});
        SDL_RenderPresent(app.rdr);
        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) done = true;
                if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == BTN_PLUS) done = true;
            }
            SDL_Delay(16);
        }
        app.cleanup();
        return 1;
    }

    const ApkInfo& apk = app.apks[idx];
    bool skip = apk.installed;
    LaunchResult res = app.runLaunch(apk, skip);
    if (!skip) app.apks[idx].installed = true;
    // Every successful game session now exits the whole process directly
    // from inside the game loop itself (crash or deliberate quit alike) —
    // runLaunch() only returns here at all when that DIDN'T happen (APK/ELF
    // load failure, or nativeRender missing), so reaching this line always
    // means something worth explaining before closing back to the launcher
    // (or the Switch home menu, if this was launched standalone for testing).
    app.showLaunchResult(res, idx);
    app.cleanup();
    return 0;
}
