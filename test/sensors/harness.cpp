// Host test harness for sensor acquisition. Which IMU a game ends up reading
// is the whole of issue #5, and getting it wrong on hardware is invisible: a
// game with no motion data looks exactly like a game that doesn't use motion
// controls. So the acquisition walk, the unit conversion and the console
// payload mapping are all exercised here against a libnx HID mock.
//
//   g++ -std=c++17 -I test/sensors/mock -I include test/sensors/harness.cpp source/compat/sensors.cpp -o /tmp/sensorstest && /tmp/sensorstest
#include <switch.h>
#include "compat/sensors.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cmath>
#include <string>

// The log the module writes to; captured so a test can assert on what it said.
static std::string g_log;
void compatLog(const char* m) { g_log += m; g_log += "\n"; }
void compatLogFmt(const char* f, ...) {
    char b[1024]; va_list a; va_start(a, f); vsnprintf(b, sizeof(b), f, a); va_end(a);
    g_log += b; g_log += "\n";
}

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* what) {
    if (ok) { g_pass++; printf("  ok   %s\n", what); }
    else    { g_fail++; printf("  FAIL %s\n", what); }
}
static bool logged(const char* needle) { return g_log.find(needle) != std::string::npos; }

// The ASensorEvent layout lives inside sensors.cpp; a test only needs the two
// vectors, and both sit at the same offset in the union.
struct TestEvent { char raw[104]; };
static const int   kAccel = 1, kGyro = 4;
static const float G = 9.80665f, TWO_PI = 6.283185307f;

static float evX(const void* ev) { return *(const float*)((const char*)ev + 24); }
static float evY(const void* ev) { return *(const float*)((const char*)ev + 28); }
static int32_t evType(const void* ev) { return *(const int32_t*)((const char*)ev + 8); }

// One full cycle: a game gets the manager, enables both sensors, polls once.
static ssize_t runOnce(TestEvent* evs, size_t n) {
    void* mgr = ASensorManager_getInstance();
    void* q   = ASensorManager_createEventQueue(mgr, nullptr, 0, nullptr, nullptr);
    ASensorEventQueue_enableSensor(q, ASensorManager_getDefaultSensor(mgr, kAccel));
    ASensorEventQueue_enableSensor(q, ASensorManager_getDefaultSensor(mgr, kGyro));
    ssize_t got = ASensorEventQueue_getEvents(q, evs, n);
    ASensorManager_destroyEventQueue(mgr, q);
    return got;
}

// The module reads its mapping from a fixed SD-card path. On a dev machine
// that is just a relative directory, so the harness makes it and cleans up.
static const char* kMapDir  = "sdmc:/Viridite";
static const char* kMapPath = "sdmc:/Viridite/console_imu.txt";
static void writeMapping(const char* text) {
    if (system("mkdir -p 'sdmc:/Viridite'") != 0) { printf("  !! cannot make %s\n", kMapDir); exit(2); }
    FILE* f = fopen(kMapPath, "w");
    if (!f) { printf("  !! cannot write %s\n", kMapPath); exit(2); }
    fputs(text, f);
    fclose(f);
}
static void resetAll() { mockHidReset(); g_log.clear(); remove(kMapPath); }

int main() {
    printf("sensors\n");
    TestEvent evs[4];

    // ── Handheld, the case that always worked ───────────────────────────────
    resetAll();
    mockHid().handheldPresent = true;
    mockHid().accel = {0.0f, 0.5f, 1.0f};
    mockHid().gyro  = {0.25f, 0.0f, 0.0f};
    ssize_t got = runOnce(evs, 4);
    check(got == 2, "handheld Joy-Cons give an accelerometer and a gyroscope event");
    check(logged("(handheld)"), "...and the log says which sensor it started");
    check(evType(&evs[0]) == kAccel && fabsf(evY(&evs[0]) - 0.5f * G) < 0.001f,
          "acceleration is converted from G to m/s^2");
    check(evType(&evs[1]) == kGyro && fabsf(evX(&evs[1]) - 0.25f * TWO_PI) < 0.001f,
          "angular velocity is converted from rev/s to rad/s");

    // ── A Pro Controller, which used to be unreachable ──────────────────────
    // Joy-Cons off the console: the old code asked only for Handheld, got
    // nothing, and reported no motion at all from a pad that has a gyro in it.
    resetAll();
    mockHid().styleSet[0] = HidNpadStyleTag_NpadFullKey;
    mockHid().gyro = {1.0f, 0, 0};
    got = runOnce(evs, 4);
    check(got == 2, "a Pro Controller's sensor is found when nothing is in handheld mode");
    check(logged("(Pro Controller)"), "...and is named as the source");

    // A pad paired into a later slot is still someone's pad.
    resetAll();
    mockHid().styleSet[5] = HidNpadStyleTag_NpadJoyDual;
    got = runOnce(evs, 4);
    check(got == 2 && logged("(Joy-Con pair)"), "a pad in player slot 6 is found");
    check(mockHid().lastTotalHandles == 2,
          "a Joy-Con pair is asked for two handles, the only style that allows it");

    resetAll();
    mockHid().styleSet[0] = HidNpadStyleTag_NpadJoyRight;
    got = runOnce(evs, 4);
    check(got == 2 && logged("(right Joy-Con)"), "a single sideways Joy-Con is enough");
    check(mockHid().lastTotalHandles == 1, "...and is asked for exactly one handle");

    // ── The console's own IMU — the point of issue #5 ───────────────────────
    // Nothing in anyone's hands. Before, that was the end of it.
    resetAll();
    mockHid().consoleAvailable = true;
    got = runOnce(evs, 4);
    check(mockHid().consoleInited && mockHid().consoleStarted,
          "with no controller at all, the console's own sensor is started");
    check(got == 0, "...but reports nothing while its payload has no known mapping");
    check(logged("console raw"), "...and logs the raw floats so the axes can be identified");
    check(logged("no mapping yet"), "...saying plainly why it is not reporting");

    // A controller always wins: the console sensor is the last resort, not a
    // replacement for the pad in someone's hands.
    resetAll();
    mockHid().handheldPresent = true;
    mockHid().consoleAvailable = true;
    runOnce(evs, 4);
    check(!mockHid().consoleStarted, "a controller in hand is preferred over the console sensor");

    // Once the mapping is known, the console sensor reports like any other.
    resetAll();
    mockHid().consoleAvailable = true;
    for (int i = 0; i < 10; i++) mockHid().consoleFloats[i] = (float)i / 10.0f;
    writeMapping("4 7\n");
    got = runOnce(evs, 4);
    check(got == 2, "with a mapping, the console sensor reports events");
    check(fabsf(evX(&evs[0]) - 0.4f * G) < 0.001f,
          "...reading acceleration from the configured offset");
    check(fabsf(evX(&evs[1]) - 0.7f * TWO_PI) < 0.001f,
          "...and angular velocity from its own");

    // A nonsense mapping would read past the end of a ten-float array.
    resetAll();
    mockHid().consoleAvailable = true;
    writeMapping("8 9\n");
    got = runOnce(evs, 4);
    check(got == 0 && logged("out of range"),
          "an index leaving no room for three floats is rejected, not read past");
    remove(kMapPath);

    // ── Teardown ────────────────────────────────────────────────────────────
    resetAll();
    mockHid().consoleAvailable = true;
    runOnce(evs, 4);
    check(mockHid().consoleStopped, "destroying the queue stops the console sensor");
    check(!mockHid().consoleFinalized, "...but does not finalize it — that is process-wide");
    sensorsShutdown();
    check(mockHid().consoleFinalized, "shutdown finalizes it, as libnx requires before hidExit");

    // Nothing anywhere: no crash, no events, and a line saying so.
    resetAll();
    got = runOnce(evs, 4);
    check(got == 0 && logged("no six-axis sensor available"),
          "a console with no sensors at all reports nothing and says so");

    remove(kMapPath);
    if (system("rm -rf 'sdmc:'") != 0) printf("  (could not clean up %s)\n", kMapDir);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
