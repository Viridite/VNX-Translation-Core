// Minimal libnx stand-in: only the HID surface source/compat/sensors.cpp uses,
// with the exact signatures from libnx's nx/include/switch/services/hid.h, plus
// hooks a test can drive. Enough to compile and exercise sensor acquisition on
// a dev machine — which matters more here than usual, because the thing being
// tested is "which IMU do we end up on", and getting that wrong on hardware
// looks identical to a game that just doesn't use motion controls.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32; typedef uint8_t u8;
typedef uint32_t Result;
#define R_FAILED(rc) ((rc) != 0)
#define MOCK_RC_FAIL 0xDEAD

typedef enum {
    HidNpadIdType_No1 = 0, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
    HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8,
    HidNpadIdType_Other = 0x10, HidNpadIdType_Handheld = 0x20,
} HidNpadIdType;

typedef enum {
    HidNpadStyleTag_NpadFullKey  = 1u << 0,
    HidNpadStyleTag_NpadHandheld = 1u << 1,
    HidNpadStyleTag_NpadJoyDual  = 1u << 2,
    HidNpadStyleTag_NpadJoyLeft  = 1u << 3,
    HidNpadStyleTag_NpadJoyRight = 1u << 4,
} HidNpadStyleTag;

typedef struct { u32 type_value; } HidSixAxisSensorHandle;
typedef struct { float x, y, z; } HidVector;
typedef struct {
    u64 delta_time; u64 sampling_number;
    HidVector acceleration; HidVector angular_velocity; HidVector angle;
} HidSixAxisSensorState;
typedef struct {
    u64 timestamp0; u64 sampling_number; u64 unk_x10; float unk_x18[10];
} HidSevenSixAxisSensorState;

// ── What the fake hardware looks like, for a test to set up ────────────────
struct MockHid {
    u32  styleSet[8]      = {0};      // per player slot
    bool handheldPresent  = false;    // HidNpadIdType_Handheld answers handles
    bool consoleAvailable = false;    // SevenSixAxisSensor initializes
    // What the sensors report.
    HidVector accel = {0, 0, 1.0f};   // 1G on Z, console resting flat
    HidVector gyro  = {0, 0, 0};
    float consoleFloats[10] = {0};
    bool consoleAtRest = true;
    // What actually happened, for assertions.
    int  handleRequests  = 0;
    s32  lastTotalHandles = 0;
    HidNpadIdType   lastId    = HidNpadIdType_Handheld;
    HidNpadStyleTag lastStyle = HidNpadStyleTag_NpadHandheld;
    bool padStarted = false, padStopped = false;
    bool consoleInited = false, consoleStarted = false;
    bool consoleStopped = false, consoleFinalized = false;
};
inline MockHid& mockHid() { static MockHid m; return m; }
inline void mockHidReset() { mockHid() = MockHid(); }

inline u32 hidGetNpadStyleSet(HidNpadIdType id) {
    if (id == HidNpadIdType_Handheld) return mockHid().handheldPresent ? HidNpadStyleTag_NpadHandheld : 0;
    int i = (int)id - (int)HidNpadIdType_No1;
    return (i >= 0 && i < 8) ? mockHid().styleSet[i] : 0;
}
inline void hidInitializeNpad(void) {}

inline Result hidGetSixAxisSensorHandles(HidSixAxisSensorHandle* handles, s32 total,
                                         HidNpadIdType id, HidNpadStyleTag style) {
    MockHid& m = mockHid();
    m.handleRequests++;
    m.lastTotalHandles = total; m.lastId = id; m.lastStyle = style;
    // libnx: only NpadJoyDual supports total_handles == 2.
    if (total == 2 && style != HidNpadStyleTag_NpadJoyDual) return MOCK_RC_FAIL;
    if (total != 1 && total != 2) return MOCK_RC_FAIL;
    if (id == HidNpadIdType_Handheld) {
        if (!m.handheldPresent) return MOCK_RC_FAIL;
    } else {
        int i = (int)id - (int)HidNpadIdType_No1;
        if (i < 0 || i >= 8 || !(m.styleSet[i] & style)) return MOCK_RC_FAIL;
    }
    for (s32 k = 0; k < total; k++) handles[k].type_value = 0x1000 + k;
    return 0;
}
inline Result hidStartSixAxisSensor(HidSixAxisSensorHandle) { mockHid().padStarted = true; return 0; }
inline Result hidStopSixAxisSensor(HidSixAxisSensorHandle)  { mockHid().padStopped = true; return 0; }
inline size_t hidGetSixAxisSensorStates(HidSixAxisSensorHandle, HidSixAxisSensorState* out, size_t count) {
    if (!count) return 0;
    memset(out, 0, sizeof(*out));
    out->acceleration = mockHid().accel;
    out->angular_velocity = mockHid().gyro;
    return 1;
}

inline Result hidInitializeSevenSixAxisSensor(void) {
    if (!mockHid().consoleAvailable) return MOCK_RC_FAIL;
    mockHid().consoleInited = true; return 0;
}
inline Result hidStartSevenSixAxisSensor(void) {
    if (!mockHid().consoleAvailable) return MOCK_RC_FAIL;
    mockHid().consoleStarted = true; return 0;
}
inline Result hidStopSevenSixAxisSensor(void)     { mockHid().consoleStopped = true; return 0; }
inline Result hidFinalizeSevenSixAxisSensor(void) { mockHid().consoleFinalized = true; return 0; }
inline Result hidGetSevenSixAxisSensorStates(HidSevenSixAxisSensorState* states, size_t count, size_t* total) {
    if (!count) { if (total) *total = 0; return 0; }
    memset(states, 0, sizeof(*states));
    memcpy(states->unk_x18, mockHid().consoleFloats, sizeof(states->unk_x18));
    if (total) *total = 1;
    return 0;
}
inline Result hidIsSevenSixAxisSensorAtRest(bool* out) { if (out) *out = mockHid().consoleAtRest; return 0; }

inline u64 armGetSystemTick(void) { static u64 t = 0; return t += 1000; }
