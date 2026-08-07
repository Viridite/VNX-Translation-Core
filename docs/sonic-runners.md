# Sonic Runners Revival on Viridite

## The headline: this build is not 32-bit

The task was framed as *"32-bit (x32) Unity support, starting with Sonic
Runners"* — AArch32 decoding, register-state mapping, a memory model built from
the ARMv8 manual. That work is real, and `source/arm32/` already exists for it.

It is not what `contribdocs/srr230-b6.apk` needs.

Measured from the APK itself:

| | |
|---|---|
| Package | `com.sonicrunners.beta` |
| Activity | `com.unity3d.player.UnityPlayerActivity` (stock Unity) |
| Unity | **2021.3.47f1** |
| Scripting backend | IL2CPP, `global-metadata.dat` **version 31** (6.8 MB) |
| ABIs shipped | `arm64-v8a`, `armeabi-v7a`, `x86_64` |
| `lib/arm64-v8a/libil2cpp.so` | 33.8 MB, **ELF64 / AArch64** |
| `lib/arm64-v8a/libunity.so` | 17.1 MB |
| `lib/arm64-v8a/libmain.so` | needs only `liblog`, `libm`, `libdl`, `libc` |

Sonic Runners Revival is a **modern Unity rebuild**, not the 2015 original. It
ships a full native arm64 build. The Switch is arm64. There is nothing to
emulate.

This matters because the two routes are not close in cost. Interpreting AArch32
for a 33 MB IL2CPP binary means every frame of game logic runs through a
software CPU; the arm64 route runs it natively at full speed through the code
path the Core **already has**. `unity-runtime/src/unity_runtime.cpp` already
`dlopen`s `libunity.so` and `libil2cpp.so`, resolves `il2cpp_init`, and drives
`initJni` / `nativeRender`. That is this game's loader, today, unmodified.

The 32-bit premise would be correct for the *original* 2015 Sonic Runners APK,
which is armeabi-v7a only and Unity 5.x. If that is the target, the roadmap in
the last section applies. For `srr230-b6.apk`, it does not.

## Route A — the one to actually take

### 1. Get it past the allowlist

`isCompatibleGame()` in the launcher's `source/main.cpp` currently admits only
Hill Climb Racing. Add `com.sonicrunners.beta`, or use the existing deep-test
escape hatch (hold Y, press X) to launch it without touching the list. Do the
latter first — it is what the escape hatch is for, and it keeps the allowlist
honest until the game actually runs.

### 2. Expect it to die in `il2cpp_init`, and read where

The Core's ELF loader already handles arm64 shared objects; the interesting
failures will be in what IL2CPP asks the platform for, not in loading it. Watch
`compat_log.txt`. The order of likely stops:

- **`global-metadata.dat` not found.** IL2CPP resolves it relative to the app's
  data dir via the `AndroidJavaClass` path lookup. Confirm the extractor put
  `assets/bin/Data/` under `sdmc:/Viridite/games/com.sonicrunners.beta/`.
- **Missing JNI methods.** Unity 2021 calls a wider `UnityPlayer` surface than
  Unity 2019. Each miss should appear in `jni_env.cpp`'s unresolved-method log
  rather than as a crash — that log is the work list.
- **`libcri_ware_unity.so`.** CRI middleware (audio/video). It is loaded by
  IL2CPP through `dlopen`, and its `NEEDED` set is worth checking before it
  fails at runtime.
- **`discord-rpc.so` is `armeabi-v7a` only** — there is no arm64 build of it in
  the APK. If the game `dlopen`s it, that call must be allowed to fail softly.
  It is Discord rich presence; nothing on a Switch wants it. Make the loader
  return null for it rather than treating a missing library as fatal.

### 3. Metadata version 31

Nothing in the Core parses `global-metadata.dat` — IL2CPP does that itself, and
version 31 is only relevant if we ever want to *inspect* the game's types (for
achievement hooks, or for patching). If that becomes necessary, Il2CppDumper
handles v31 and the dump is the map. It is not needed to boot.

### 4. Where the ARMv8 manual actually earns its place

`contribdocs/DDI0487G_a_armv8_arm.pdf` is genuinely needed here — just not for
instruction decoding. The chapters that matter to running someone else's arm64
code in our process:

- **B2, the memory model.** IL2CPP's GC and Unity's job system are multi-threaded
  and lock-free in places. If the Core ever relocates or patches code that these
  threads run, the barrier and cache-maintenance rules in B2 are what make the
  difference between "works on my console" and an intermittent fault.
- **D7 / B2.10, cache maintenance.** After writing instructions into a page —
  which the loader does, for every relocation — the sequence is `DC CVAU`, `DSB
  ISH`, `IC IVAU`, `DSB ISH`, `ISB`. Getting this wrong produces exactly the
  class of bug the project already hit once: pages made RX before their contents
  were visible to the I-cache.
- **B1.1, exception levels and `TPIDR_EL0`.** Thread-local storage. IL2CPP uses
  TLS heavily for its GC handles; anything the Core does to guest threads has to
  leave `TPIDR_EL0` alone.

That is where to spend the reading. Decoding tables (C4) are for Route B.

## What RunnersDecomp is, and is not, for

MattKC's [RunnersDecomp](https://github.com/itsmattkc/RunnersDecomp) is a
source-level reconstruction of the **original 2015** game. It is not a way to
run this APK, and nothing in it needs to be compiled for Viridite.

Where it is worth reading:

- **Server protocol.** Sonic Runners was an always-online title; the Revival
  project stands up a private server. If the Revival client cannot reach its
  server, it will not get past the title screen no matter how well the engine
  runs. Establish where the client points and whether it is reachable **before**
  spending time on engine bugs — a network failure and an engine failure look
  identical from a black screen.
- **Game-logic reference,** if a specific behaviour needs to be understood.

Treat it as documentation, not as a dependency.

## Route B — the original 32-bit APK

If the target is genuinely the 2015 build, this is the plan, and it is a much
longer one:

1. **Confirm the shape first.** `unzip -l` the APK: if `lib/` has only
   `armeabi-v7a` and `assets/bin/Data/Managed/` holds `.dll` files rather than a
   `global-metadata.dat`, it is Unity 5.x **Mono**, not IL2CPP — a completely
   different problem (a Mono runtime and JIT, not a CPU interpreter).
2. **Reuse `source/arm32/`.** `cpu.cpp`, `arm32_mem.cpp`, `elf32.cpp` and
   `bridge.cpp` are 2,662 lines that already run Hill Climb Racing's 32-bit
   path. The gap for Unity is breadth of instruction coverage — particularly
   NEON, which Unity's math library leans on hard — not architecture.
3. **Here** the manual's C4 decode tables and F-chapter AArch32 sections are the
   working reference, and the existing `arm32` commits (`PC reads as +8`, `NEON
   is no longer decoded as a byte store`) show the shape of the work.
4. **Budget honestly.** A 30 MB IL2CPP binary interpreted instruction by
   instruction will not hit playable frame rates without a JIT. That is a
   project, not a task.

## Recommendation

Run `srr230-b6.apk` through the existing arm64 Unity path. Find out what breaks.
Only reach for the 32-bit interpreter if the target changes to the 2015 build —
and know that when you do, the interpreter is the easy half and the frame rate
is the hard one.
