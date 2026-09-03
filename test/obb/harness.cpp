// Host test harness for OBB discovery, install and path remapping. The module
// is plain stdio and string work for exactly this reason: every title it
// exists for is one nobody here has an APK for, so a real directory tree built
// in /tmp is the only way any of it gets checked before hardware.
//
//   g++ -std=c++17 -I include test/obb/harness.cpp source/compat/obb.cpp -o /tmp/obbtest && /tmp/obbtest
#include "compat/obb.h"
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* what) {
    if (ok) { g_pass++; printf("  ok   %s\n", what); }
    else    { g_fail++; printf("  FAIL %s\n", what); }
}

static std::string g_root;

static void mkdirs(const std::string& p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur += p[i];
        if (p[i] == '/' || i + 1 == p.size()) mkdir(cur.c_str(), 0777);
    }
}
static void writeFile(const std::string& p, size_t bytes) {
    mkdirs(p.substr(0, p.find_last_of('/')));
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) { printf("  !! cannot write %s\n", p.c_str()); exit(2); }
    for (size_t i = 0; i < bytes; i++) fputc('x', f);
    fclose(f);
}
static long long sizeOf(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (long long)st.st_size : -1;
}

int main() {
    printf("obb\n");
    char tmpl[] = "/tmp/obbtestXXXXXX";
    g_root = mkdtemp(tmpl);

    const std::string pkg = "com.square_enix.android_googleplay.FFIII_GP";

    // ── Name parsing ────────────────────────────────────────────────────────
    obb::File f;
    check(obb::parseName("main.86." + pkg + ".obb", pkg, &f) && !f.patch && f.version == 86,
          "main.<ver>.<pkg>.obb parses, version and role read off it");
    check(obb::parseName("patch.87." + pkg + ".obb", pkg, &f) && f.patch && f.version == 87,
          "patch.<ver>.<pkg>.obb is recognised as a patch");
    check(!obb::parseName("main.86.com.other.game.obb", pkg, &f),
          "another game's OBB in the same folder is not claimed");
    check(obb::parseName("main.obb", pkg, &f) && f.canonicalName.empty(),
          "a bare main.obb parses but carries no package, so it names nothing on its own");
    check(!obb::parseName("main.obb.txt", pkg, &f), "a non-.obb file is not an OBB");
    check(!obb::parseName("expansion.86." + pkg + ".obb", pkg, &f),
          "an OBB that is neither main nor patch is ignored");
    check(!obb::parseName("main.notanumber." + pkg + ".obb", pkg, &f),
          "a non-numeric version is not a version");

    // ── Install order ───────────────────────────────────────────────────────
    // Kingdom Hearts Union chi ships main.76 and patch.87; the patch must land
    // after the main file or it isn't an overlay.
    std::vector<obb::File> order = {
        {"", "patch.87.x.obb", true, 87}, {"", "main.99.x.obb", false, 99},
        {"", "main.76.x.obb", false, 76},
    };
    obb::sortForInstall(order);
    check(order[0].canonicalName == "main.76.x.obb" &&
          order[1].canonicalName == "main.99.x.obb" &&
          order[2].canonicalName == "patch.87.x.obb",
          "main files come before patches, older before newer");

    // ── Discovery, against a real tree ──────────────────────────────────────
    // The Android layout copied off a phone, which is what someone who backed
    // up their own game actually has.
    const std::string apkDir = g_root + "/games";
    const std::string apk    = apkDir + "/ff3.apk";
    writeFile(apk, 16);
    writeFile(apkDir + "/Android/obb/" + pkg + "/main.86." + pkg + ".obb", 1024);
    auto found = obb::find(apk, pkg);
    check(found.size() == 1 && found[0].canonicalName == "main.86." + pkg + ".obb",
          "an OBB in Android/obb/<pkg>/ is found next to the APK");

    // Loose beside the APK, which is what someone who downloaded both has.
    writeFile(apkDir + "/patch.87." + pkg + ".obb", 512);
    found = obb::find(apk, pkg);
    check(found.size() == 2 && found[1].patch, "a loose patch beside the APK is found too, after the main");

    // A bare main.obb beside the APK is ambiguous — it could belong to any of
    // several games in the same folder — so it is only taken from a folder
    // that is already this game's.
    writeFile(apkDir + "/main.obb", 32);
    found = obb::find(apk, pkg);
    check(found.size() == 2, "a bare main.obb loose beside the APK is not claimed");
    writeFile(apkDir + "/ff3/main.obb", 64);
    found = obb::find(apk, pkg);
    check(found.size() == 3, "...but the same name in a folder named for the APK is");

    // Another game's OBB sitting in the same folder must not be picked up.
    writeFile(apkDir + "/main.12.com.someone.else.obb", 8);
    found = obb::find(apk, pkg);
    check(found.size() == 3, "another game's OBB in the same folder is left alone");

    // ── Install ─────────────────────────────────────────────────────────────
    const std::string dataDir = g_root + "/data";
    mkdirs(dataDir);
    const std::string obbDir = obb::dirFor(dataDir);
    int n = obb::install(found, obbDir);
    check(n == (int)found.size(), "every found OBB is installed");
    check(sizeOf(obbDir + "/main.86." + pkg + ".obb") == 1024,
          "the installed OBB keeps its canonical Android name and its contents");

    // A relaunch must not re-copy a gigabyte.
    FILE* marker = fopen((obbDir + "/main.86." + pkg + ".obb").c_str(), "r+b");
    if (marker) { fputc('Z', marker); fclose(marker); }   // same size, different byte
    obb::install(found, obbDir);
    char first = 0;
    if (FILE* r = fopen((obbDir + "/main.86." + pkg + ".obb").c_str(), "rb")) {
        first = (char)fgetc(r); fclose(r);
    }
    check(first == 'Z', "an OBB already in place at the same size is not copied again");

    check(obb::install({}, obbDir) == 0, "installing nothing succeeds and does nothing");

    // ── Path remapping ──────────────────────────────────────────────────────
    const std::string want = obbDir + "/main.86." + pkg + ".obb";
    check(obb::remapPath("/storage/emulated/0/Android/obb/" + pkg + "/main.86." + pkg + ".obb",
                         pkg, obbDir) == want,
          "a hardcoded /storage/emulated/0 OBB path resolves to the installed file");
    check(obb::remapPath("/sdcard/Android/obb/" + pkg + "/main.86." + pkg + ".obb",
                         pkg, obbDir) == want,
          "...and so does the /sdcard spelling of the same path");
    check(obb::remapPath("/sdcard/Android/obb/com.other.game/main.obb", pkg, obbDir).empty(),
          "another package's OBB path is not rewritten");
    check(obb::remapPath("/sdcard/DCIM/photo.jpg", pkg, obbDir).empty(),
          "an unrelated external-storage path is left alone");
    check(obb::remapPath("sdmc:/Viridite/games/x/assets/a.png", pkg, obbDir).empty(),
          "a path that is already ours is left alone");
    check(obb::remapPath("/sdcard/Android/obb/" + pkg + "/sub/dir/main.obb", pkg, obbDir).empty(),
          "a path reaching below the OBB directory is not rewritten");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    std::string rm = "rm -rf " + g_root;
    if (system(rm.c_str()) != 0) printf("  (could not clean up %s)\n", g_root.c_str());
    return g_fail ? 1 : 0;
}
