// Host test harness for the game database. gamedb.h is header-only and depends
// on nothing but <cstring>, so it can be exercised on a dev machine — which is
// the whole reason the entry-library rule lives there rather than inline in
// launchApk, where it could only ever be checked on hardware with a real APK.
//
//   g++ -std=c++17 -I include test/gamedb/harness.cpp -o /tmp/gamedbtest && /tmp/gamedbtest
#include "compat/gamedb.h"
#include <cstdio>
#include <cstring>

using namespace gamedb;

static int g_pass = 0, g_fail = 0;

static void check(bool ok, const char* what) {
    if (ok) { g_pass++; printf("  ok   %s\n", what); }
    else    { g_fail++; printf("  FAIL %s\n", what); }
}

// Convenience: run chooseEntrySo over a fixed library set.
struct Lib { const char* name; size_t size; };
template <size_t N>
static const char* pick(const char* pkg, const Lib (&libs)[N], const char** why = nullptr) {
    const char* names[N];
    size_t sizes[N];
    for (size_t i = 0; i < N; i++) { names[i] = libs[i].name; sizes[i] = libs[i].size; }
    int idx = chooseEntrySo(pkg, names, sizes, N, why);
    return idx < 0 ? nullptr : names[idx];
}

int main() {
    printf("gamedb\n");

    // ── Package lookup ──────────────────────────────────────────────────────
    const Title* hcr = findByPackage("com.fingersoft.hillclimb");
    check(hcr && strcmp(hcr->name, "Hill Climb Racing") == 0, "Hill Climb Racing resolves");
    check(hcr && hcr->support == Support::Playable, "Hill Climb Racing is the playable one");
    check(findByPackage("com.example.nothing") == nullptr, "an unknown package resolves to nothing");
    check(findByPackage("") == nullptr && findByPackage(nullptr) == nullptr,
          "empty and null packages resolve to nothing");

    // Exactly one title may claim Playable without a hardware session behind
    // it. If a second ever appears here, it is because someone added a row
    // optimistically — which is the failure this table exists to prevent.
    int playable = 0;
    for (size_t i = 0; i < kTitleCount; i++)
        if (kTitles[i].support == Support::Playable) playable++;
    check(playable == 1, "exactly one title claims to be playable");

    // Reference-only rows must never be reachable by a package lookup.
    for (size_t i = 0; i < kTitleCount; i++) {
        if (kTitles[i].pkg) continue;
        check(findByPackage(kTitles[i].name) == nullptr,
              "reference-only row is not matched by its display name");
        break;
    }

    // No two rows may claim the same package id — the second would be dead.
    bool dupe = false;
    for (size_t i = 0; i < kTitleCount; i++) {
        if (!kTitles[i].pkg) continue;
        for (size_t j = i + 1; j < kTitleCount; j++)
            if (kTitles[j].pkg && strcmp(kTitles[i].pkg, kTitles[j].pkg) == 0) dupe = true;
    }
    check(!dupe, "no duplicate package ids");

    // ── Library roles ───────────────────────────────────────────────────────
    check(isKnownDependency("libfmodstudio.so"), "FMOD Studio is a dependency");
    check(isKnownDependency("libc++_shared.so"), "the C++ runtime is a dependency");
    check(isKnownDependency("libil2cpp.so"), "libil2cpp is a dependency, not an entry point");
    check(!isKnownDependency("libgame.so"), "libgame.so is not a dependency");
    check(!isKnownDependency("libsomethingnobodyknows.so"), "an unknown library is not assumed to be one");

    // ── Entry-library selection ─────────────────────────────────────────────
    const char* why = nullptr;

    // Hill Climb Racing: the game is the largest anyway, so this is the case
    // the old size rule already got right — it must not regress.
    const Lib hcrLibs[] = {
        {"libapplovin-native-crash-reporter.so", 200000},
        {"libquack.so", 900000},
        {"libgame.so", 24000000},
    };
    check(strcmp(pick("com.fingersoft.hillclimb", hcrLibs, &why), "libgame.so") == 0,
          "Hill Climb Racing enters libgame.so");

    // Unity: libil2cpp.so is far larger than the entry point. This is the case
    // the size rule gets wrong.
    const Lib unityLibs[] = {
        {"libmain.so", 30000},
        {"libunity.so", 18000000},
        {"libil2cpp.so", 60000000},
    };
    check(strcmp(pick("eu.bandainamcoent.verylittlenightmares", unityLibs, &why), "libmain.so") == 0,
          "a Unity game enters libmain.so, not the 60MB libil2cpp.so");
    check(strcmp(pick("com.unknown.unitygame", unityLibs, &why), "libmain.so") == 0,
          "an unrecognised Unity game still enters libmain.so, by library name alone");

    // After Burner Climax: FMOD Studio outweighing the game is exactly the
    // shape that makes "largest wins" pick an audio backend.
    const Lib abcLibs[] = {
        {"libacb.so", 4000000},
        {"libfmod.so", 5000000},
        {"libfmodstudio.so", 9000000},
    };
    check(strcmp(pick("com.sega.afterburnerclimax", abcLibs, &why), "libacb.so") == 0,
          "After Burner Climax enters libacb.so, not FMOD Studio");
    check(strcmp(pick(nullptr, abcLibs, &why), "libacb.so") == 0,
          "...and does so even with no package id, because libacb.so is a known entry point");

    // A game nobody has written a row for: the rule must still avoid the
    // dependency and land on the game.
    const Lib unknownLibs[] = {
        {"libc++_shared.so", 1500000},
        {"libmysterygame.so", 800000},
    };
    check(strcmp(pick("com.unknown.game", unknownLibs, &why), "libmysterygame.so") == 0,
          "an unknown game is entered rather than its (larger) C++ runtime");
    check(why && strstr(why, "isn't a known dependency"),
          "...and the log line says which rule answered");

    // Degenerate sets.
    const Lib onlyDeps[] = {{"libc++_shared.so", 1500000}, {"libfmod.so", 3000000}};
    check(strcmp(pick("com.unknown.game", onlyDeps, &why), "libfmod.so") == 0,
          "a set with nothing but dependencies still returns something (the old rule)");
    check(chooseEntrySo("com.fingersoft.hillclimb", nullptr, nullptr, 0) == -1,
          "an empty library set returns -1");

    // A documented entry library that isn't actually present must not stop the
    // load: a stripped or repacked APK falls through to the generic rules.
    const Lib missingEntry[] = {{"libunity.so", 18000000}, {"libil2cpp.so", 60000000}};
    check(strcmp(pick("eu.bandainamcoent.verylittlenightmares", missingEntry, &why), "libil2cpp.so") == 0,
          "a missing documented entry library falls back instead of failing");

    // ── Table hygiene ───────────────────────────────────────────────────────
    for (size_t i = 0; i < kTitleCount; i++) {
        const Title& t = kTitles[i];
        if (!t.name || !*t.name) { check(false, "every row has a name"); break; }
        // A row that claims an engine Viridite can drive should say what to
        // enter; the Source rows deliberately don't, and say so by engine.
        if (t.engine != Engine::Source && !t.entrySo) {
            printf("  FAIL row '%s' has no entry library\n", t.name);
            g_fail++;
        }
    }
    check(true, "every runnable row names an entry library");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
