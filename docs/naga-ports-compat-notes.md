# Compatibility notes from NaGaa95's independent Switch ports

## What this is

[NaGaa95](https://github.com/NaGaa95) has independently built and published
working Switch ports of several Android games, using the same core technique
as Viridite: load the original Android game binary, resolve its imports
against native Switch implementations, and run it inside a minimal
Android-like environment — no source code, no assets from the original game.
Their ports are separate, hand-built-per-title NROs, not built on Viridite's
engine.

Their public work is real evidence these titles are portable to Switch at
all, and their READMEs document real technical facts about each game worth
recording here before any of this becomes actual Viridite `game_*.cpp` work.
**Full credit to NaGaa95** for the ports this doc is drawn from — everything
below is transcribed from their public READMEs, not decompiled or verified
independently against the actual APKs (this pass didn't have the APKs or
hardware to do that; see "What's actually needed" below).

Ports referenced: [subwaysurfers_nx](https://github.com/NaGaa95/subwaysurfers_nx),
[gdash_nx](https://github.com/NaGaa95/gdash_nx),
[sotn_nx](https://github.com/NaGaa95/sotn_nx).

## Per-game facts

### Subway Surfers (`com.kiloo.subwaysurf`, v3.66.1)

- **Engine: Unity 2022.3, IL2CPP scripting backend, arm64.** Ships
  `libmain.so`, `libunity.so`, `libil2cpp.so`.
- This is the same engine shape as `docs/sonic-runners.md`'s Sonic Runners
  Revival case (modern Unity, IL2CPP, native arm64 — "nothing to emulate").
- **Open question, found while writing this doc, not resolved here:**
  `sonic-runners.md` says `unity-runtime/src/unity_runtime.cpp` already
  `dlopen`s `libunity.so`/`libil2cpp.so` and drives `il2cpp_init`/`initJni`/
  `nativeRender` — i.e. that a working Unity IL2CPP path already exists. On
  the current `main`, `unity-runtime/` is an **empty directory**. Worth
  checking before assuming either doc or tree is right — did that file move,
  get reverted, or was the doc written ahead of the code landing? If the
  Unity path genuinely already works, Subway Surfers may be much closer to
  supportable than the rest of this doc assumes.
- Controls are swipe-gesture-driven; NaGaa95's port maps stick/d-pad
  directions to synthetic swipes rather than emulating a touchscreen swipe
  literally — a real design decision worth reusing if/when this gets built,
  not reinventing.
- Needs full game-override memory (not applet/album mode) per their README,
  consistent with Viridite's own Hill Climb Racing experience.

### Geometry Dash (+ Meltdown/SubZero/World variants) (v2.2.14x, arm64)

- **Engine: cocos2d-x 2.2 (RobTop's own fork) + `libfmod.so`.**
- This is the same engine family Hill Climb Racing already runs under in
  Viridite ("cocos2d-x / generic NDK path" per `compat_log.txt`) — of the
  three, this is the one most likely to be closest to already-working,
  though HCR needed several signature-gated instruction patches of its own
  (see `source/compat/games/game_hillclimb.cpp`), so "same engine family"
  is not the same claim as "will just run."
- NaGaa95's port implements the missing Switch socket ABI conversions for
  RobTop's embedded Boomlings HTTPS client (public leaderboards/level
  search), regenerates `cacert.pem` from the Switch firmware's trust store
  at load time, and deliberately leaves Google Play sign-in disabled/unused
  — a real reference for what a networked cocos2d-x title needs beyond what
  a purely offline game like current HCR does.
- File layout: `libcocos2dcpp.so` + `libfmod.so` + the APK's `assets/`
  tree, no OBB unwrapping needed (unlike SOTN below).

### Castlevania: Symphony of the Night (`com.dotemu.sotn`, v1.0.6)

- **Custom/proprietary engine** (DotEmu's own port, not Unity or cocos2d) —
  the hardest of the three to generalize, would need bespoke work the same
  way NaGaa95 did, not a shared engine path.
- NaGaa95 credits **TheOfficialFloW** for the underlying method (originally
  from PS Vita work) and **fgsfds and Andy Nguyen** for the so-loader their
  port is based on — worth reading that lineage before starting bespoke
  SOTN work, since the technique predates this specific port.
- Asset layout is more involved than the other two: the game ships
  `assets/res2`, a zip wrapping an OBB that is *itself* a zip, unwrapped
  twice to get the real `assets/` tree — a format quirk worth knowing before
  writing an installer/extractor for this title.

## What's actually needed before any of this is real Viridite support

Everything above is transcribed from public READMEs, not independently
verified. Real per-title support in this engine (see
`source/compat/games/game_hillclimb.cpp` for the bar) means
signature-gated, hardware-tested binary patches keyed to an exact APK
version's sha256 — that requires the actual APK and real hardware to
observe faults on, neither of which this pass had. This doc is scoping/
research, not implementation. Next real steps, in rough order:

1. Resolve the `unity-runtime/` discrepancy noted above — that changes how
   much of the Subway Surfers path is genuinely new work.
2. Get a legally-owned copy of each APK and run it through the existing
   loader to see what actually happens (same empirical approach as
   `BRAIN_IT_ON_FINDINGS.md` and the HCR quirks) rather than guessing at
   patch sites from outside.
3. Geometry Dash is the most promising near-term target given the shared
   cocos2d-x lineage with the already-working HCR path.
