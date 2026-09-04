# Identifying the console IMU's payload

## Why this file exists

The Switch console has its own motion sensor, separate from the Joy-Cons —
[issue #5](https://github.com/Viridite/Viridite/issues/5), reported by
@hippydave, who confirmed it the practical way: Labo VR tracks head movement
with the console strapped to your face and the Joy-Cons detached.

libnx exposes it as the SevenSixAxisSensor, and Viridite now starts it when no
controller with a six-axis sensor is available (`source/compat/sensors.cpp`).
What libnx does *not* say is what the sensor reports:

```c
typedef struct {
    u64 timestamp0;
    u64 sampling_number;
    u64 unk_x10;
    float unk_x18[10];      // ← ten floats, none of them named
} HidSevenSixAxisSensorState;
```

Ten floats and no documentation, in libnx or anywhere else that could be
checked against. A plausible guess — a fused quaternion, then acceleration,
then angular velocity — is still a guess, and feeding a game numbers read from
the wrong offsets is worse than reporting none: a tilt control that drifts is
indistinguishable from a broken game.

So the mapping is a setting, not a constant in the source. Identifying it takes
one hardware session and no rebuild.

## Doing it

1. Launch anything through Viridite with no controller attached to the console
   and none paired — the console sensor is the last resort, so a Joy-Con in
   your hands will be used instead and you'll see `(handheld)` in the log.
2. The log takes 40 raw samples and stops:

   ```
   sensors: console raw (at rest) [0]=… [1]=… [2]=… … [9]=…
   ```

3. Hold the console still and flat on a table for the first few samples, then
   roll it onto each edge in turn. Read `sdmc:/Viridite/compat_log.txt`:

   - **Acceleration** is the triple that reads roughly `(0, 0, ±1)` at rest and
     swings to ±1 on a different component as you tip the console — gravity
     moving between axes. Units are probably G, as they are for every other
     Switch IMU reading.
   - **Angular velocity** is the triple that sits near zero at rest and spikes
     only *while* you are turning, returning to zero when you stop.
   - A quaternion, if one is in there, is the four components that stay near
     unit length and change smoothly. It is not what is wanted here.

4. Write the two starting indices to `sdmc:/Viridite/console_imu.txt`:

   ```
   4 7
   ```

   meaning acceleration is `unk_x18[4..6]` and angular velocity is
   `unk_x18[7..9]`. Each index must be 0–7, so three floats fit.

5. Relaunch. The log should now read `sensors: console IMU mapping accel=… gyro=…`
   and events start flowing. If the axes are mirrored or swapped relative to
   what a game expects, that is the next thing to pin down — Android's axes are
   right-handed with +X right, +Y up and +Z out of the screen.

## Once it's confirmed

Put the indices in `consoleMapLoad()` in `source/compat/sensors.cpp` as the
default, keep the file as an override, and note here which firmware it was
confirmed on. Until then nothing is hardcoded, because nothing has been
observed.
