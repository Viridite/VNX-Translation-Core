# VNX Translation Core

The game-loading engine half of **[Viridite](https://github.com/Viridite/Viridite)** — an Android NDK compatibility layer that runs a game's real Android `.so` binary natively on Nintendo Switch, instead of emulating anything. If you're looking for the app you actually launch on your Switch, that's the [Viridite launcher repo](https://github.com/Viridite/Viridite); this repo is the engine it hands off to.

## What lives here

- **`VNX-Translation-Core-x64`** — the real engine: custom ARM64 ELF loader (JIT dual-mapping, relocation, symbol resolution), a JNI/JavaVM emulation layer, real pthreads, real audio (SDL2_mixer), real accelerometer/gyroscope/battery via libnx. Built from `source/` + `source/compat/` at the repo root.
- **`VNX-Translation-Core-x32`** (`core32/`) — a placeholder. 32-bit (`armeabi-v7a`) Android binaries aren't supported yet — running AArch32 code on Switch is possible in principle (real prior art exists for it), but the one precedent project we found for this depends on a 32-bit libnx build that isn't publicly available anywhere. This binary just explains that; it doesn't do anything else yet.

## Why this is a separate binary from the launcher

The launcher and this Core can't be one process: launching a specific game means chain-loading from the launcher into whichever Core matches that game's architecture (`envSetNextLoad`, the same mechanism homebrew forwarders use). A Switch process runs in one execution state for its whole lifetime, so a future real 32-bit engine has to be its own binary anyway — splitting the 64-bit engine out now means adding that later is "point the launcher at a new NRO," not "rewrite everything."

This binary always expects a package name (passed as `argv[1]` — `argv[0]` has to stay this binary's own real path, since libnx's `romfsInit()` depends on it to find and read this NRO's embedded assets). Launching it directly without a valid package argument just shows a message pointing back at the launcher — it isn't meant to be run standalone.

## Building

Requires [devkitPro](https://devkitpro.org/) with `devkitA64` and `libnx` installed.

```sh
export DEVKITPRO=/opt/devkitpro
make            # builds VNX-Translation-Core-x64.nro at the repo root
make -C core32  # builds the x32 placeholder
```

### Dependencies (via pacman/devkitPro)

```
switch-sdl2 switch-sdl2_image switch-sdl2_ttf switch-sdl2_mixer
switch-libpng switch-libjpeg-turbo switch-minizip switch-libvorbisidec
switch-libopus switch-opusfile switch-flac switch-mpg123 switch-libmodplug
switch-mesa switch-glad switch-curl switch-mbedtls
```

## Status

Only one game has ever actually been run against this engine — **Hill Climb Racing 1.67.0** (Fingersoft) — fully playable (touch, real audio, real threads, persistent saves, ~locked 60fps). Two deterministic crashes found via hardware testing and `.eh_frame` unwind-table analysis are patched, both signature-gated to this exact `libgame.so` build so a mismatched version is left untouched (see `source/compat/games/game_hillclimb.cpp`):
- **Shop/IAP crash** — the Google-Play shop builder populates its product list from a store backend that doesn't exist here, leaving it empty, then unconditionally reads past the end of it. Fixed with three coordinated patches: `getMarketVariation()` claims Google Play so the builder takes a sane branch, the doomed populate call is NOP'd, and the empty-vector virtual-call is branched over. That last patch initially targeted the element-load instruction the disassembly pointed at, but hardware kept crashing one instruction later — there's at least one more code path into that block than static analysis found — so the fix instead patches the actual faulting instruction directly, covering every path into it regardless of how many there turn out to be.
- **Racing null-deref crash** — a vehicle skin lookup (`SkinProvider::find`) returns null when the skin map is empty, and the caller dereferences it with no null check. Guarded to fall through to "no skin matched" instead.

## Expansion files (OBB)

Google Play caps an APK at 100MB, so a large game ships its data beside the APK in an expansion file — `main.<version>.<package>.obb`, living at `Android/obb/<package>/` on the device. Viridite copied the APK and nothing else, which meant every such title could only ever fail on its first asset read: the data had never been on the Switch. That is most of the Square Enix catalogue, GTA: San Andreas, Castle of Illusion, After Burner Climax and Kingdom Hearts Union χ.

`source/compat/obb.cpp` finds them (the Android tree copied off a phone, a folder named for the package or the APK, an `obb/` subfolder, or loose beside the APK), installs them under their canonical names, points `getObbDir()` and `ANativeActivity.obbPath` at them, and rewrites the external-storage paths a game may have baked in (`/sdcard/Android/obb/<pkg>/…`) so they resolve. It does **not** unpack them — Android doesn't either; the game is handed a path and reads the OBB itself, usually as a zip.

A title the database marks as keeping its data in an OBB, launched without one, is now refused with the name of the file it wants and where to put it, rather than loading into a fault in its own asset code several hundred frames later.

`res/` is extracted from the APK now as well. It used to be dropped on the reasoning that an NDK game's data is in `assets/` — true for Hill Climb Racing, not for Cut the Rope's art, the Square Enix intro movies in `res/raw`, or Swordigo's music.

## Finding a game's entry points

A cocos2d-x game exports its engine hooks as JNI long names — `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender`. The Core looked for exactly that string, which is what Hill Climb Racing exports and what stock cocos2d-x produces. A game that ships the engine under its own package exports a different spelling, so it could load, link with nothing unresolved, and then have no render loop and nothing in the log explaining why.

Entry points now resolve in four steps — the stock name, the `org.cocos2dx.cpp` spelling, any exported `Java_` symbol whose class and method match whatever the package (`include/compat/jnisym.h` handles the mangling, including `_1`-escaped underscores and `__signature` overloads), and finally the JNI registration table for a game that registers natives through `JNI_OnLoad` instead of exporting them. The stock name is tried first, so Hill Climb Racing resolves exactly as it did.

## Hill Climb Racing's shop

HCR's shop is not filled in by the game. Java fills it: at startup `NewBillingHandle.Init()` calls the game's own native `setInAppItem()` once per product — 69 times — before Google Play is contacted. Only then is there a product vector to index.

Viridite runs no Dalvik bytecode, so those calls never happened and the vector stayed empty. The native builder read `items[size-1]` on it (a load from -8), and elsewhere walked the same empty vector and called a virtual method on what it found. Both were patched out at fixed addresses: the Shop opened empty instead of taking the app down, and every patch was pinned to one exact build, because that is what an address is.

The cause was never the instructions — it was the 69 calls that never came. `source/compat/games/game_hillclimb_shop.cpp` makes them, in the window Android does (after `nativeSetPaths`, before `nativeInit`). The shop then works rather than merely surviving, and product ids are content rather than addresses, so the same replay serves **1.67.0 and 1.71.1** alike. The address-pinned crash patches now apply only to a build that does not export `setInAppItem` — decided at load time by looking for that name in the staged image.

The catalogue, and the `gcEventDetails` server-event cache the shop loader assumes exists (synthesised as `{}` when absent, since nothing fetches it here), both come from [xflipperkast's HCR_NX](https://github.com/xflipperkast/HCR_NX), an independent Switch port that read them out of the game's own `classes2.dex`. Full credit for identifying the mechanism and recovering the data. Host-tested in [`test/hcrshop/`](test/hcrshop/) — a mistake in 69 twelve-argument calls is not a crash on hardware, it is a wrong price on a screen nobody screenshots.

## Motion sensors

`ASensorManager`/`ASensorEventQueue` are implemented against the real NDK ABI and backed by a Switch six-axis sensor. Which one is no longer a single answer: acquisition walks the handheld Joy-Cons, then every connected pad (a Pro Controller, a detached pair, a single sideways Joy-Con — all of which have sensors that were previously never asked for), and finally the console's own IMU, the one Labo VR head-tracks with ([issue #5](https://github.com/Viridite/Viridite/issues/5)).

The console sensor is started but does not report yet: libnx describes its sample as ten unnamed floats and the layout is not documented anywhere checkable, so instead of guessing it logs raw samples and reads its mapping from `sdmc:/Viridite/console_imu.txt`. [docs/console-imu.md](docs/console-imu.md) is the ten-minute hardware procedure to identify it — no rebuild needed to try a mapping.

Acquisition, unit conversion and the mapping parser are host-tested against a libnx HID mock in [`test/sensors/`](test/sensors/), because "which IMU did we end up on" is invisible on hardware: a game with no motion data looks exactly like a game that doesn't use motion controls.

## Knowing a title before loading it

`include/compat/gamedb.h` holds what is known about each Android title this Core can be pointed at: its engine, **which of its libraries is actually the game**, and what data it needs beyond its APK — all kept strictly apart from whether it runs here (one title does; see Status above).

Most of it is transcribed from the READMEs of [NaGaa95](https://github.com/NaGaa95)'s independent Switch ports, credited per row — their ports name, per title, the two things an APK's own metadata never tells you. See [docs/naga-ports-compat-notes.md](docs/naga-ports-compat-notes.md) for the full table and provenance.

The load-path consequence is which library gets entered. "The largest `.so` is the game" is right for a game shipping one big binary and a few helpers, and wrong for a Unity title (largest is `libil2cpp.so`; the entry point is `libmain.so`) or one shipping FMOD Studio. The rule is now: the title's documented entry library → the largest library whose *name* is a known engine entry point → the largest that isn't a known dependency → the largest. The answer and the rule that gave it go in `compat_log.txt`; the rules are host-tested in [`test/gamedb/harness.cpp`](test/gamedb/harness.cpp):

```
g++ -std=c++17 -I include test/gamedb/harness.cpp -o /tmp/gamedbtest && /tmp/gamedbtest
g++ -std=c++17 -I include test/obb/harness.cpp source/compat/obb.cpp -o /tmp/obbtest && /tmp/obbtest
g++ -std=c++17 -I include test/jnisym/harness.cpp -o /tmp/jnisymtest && /tmp/jnisymtest
g++ -std=c++17 -I test/sensors/mock -I include test/sensors/harness.cpp source/compat/sensors.cpp -o /tmp/sensorstest && /tmp/sensorstest
g++ -std=c++17 -I test/arm32/mock -I include test/hcrshop/harness.cpp source/compat/games/game_hillclimb_shop.cpp -o /tmp/hcrshoptest && /tmp/hcrshoptest
```

The file is header-only and dependency-free because the launcher carries a byte-identical copy at `Viridite/include/gamedb.h` — the two must never disagree about a title, so keeping them in step is a file copy rather than a merge.

See the [launcher repo's README](https://github.com/Viridite/Viridite) for the full project history, changelog, roadmap, and game compatibility list.

## License

Licensed under the Viridite Free & Source-Available License v1.0 — see [LICENSE](LICENSE). Free to use, copy, modify, and share; selling this code or derivatives isn't permitted, forks must credit Aaronateataco and stay under this same license and publicly available.

## About

Built by [Aaron](https://aaronworld.uk) with [Claude](https://anthropic.com) — an experiment in AI-assisted Nintendo Switch homebrew development. Aaron tests on real hardware and reports what's broken; Claude writes the fixes.
