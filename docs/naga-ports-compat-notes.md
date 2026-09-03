# Titles from NaGaa95's Switch ports

## What this is

[NaGaa95](https://github.com/NaGaa95) has independently built and published
working Switch ports of a long list of Android games, using the same basic
technique as Viridite: load the original Android arm64 `.so`, resolve its
imports against native Switch implementations, and run it inside a minimal
Android-shaped environment — no source, no shipped assets, no emulator. Their
ports are separate hand-built-per-title NROs rather than one generic layer, but
that difference is in the packaging, not the method.

**Full credit to NaGaa95** for the ports this document is drawn from. Every
technical fact below is transcribed from their public READMEs; none of it was
verified here against an actual APK. What their work gives Viridite is real
evidence that these titles are portable at all, plus the specific things their
READMEs name and no APK metadata does: **which library is actually the game**,
and **what data the game needs beyond its APK**.

That is what has been added to the code, in
[`include/compat/gamedb.h`](../include/compat/gamedb.h) — a table shared
byte-identically with the launcher, holding what is known about each title,
kept strictly apart from any claim that it runs.

## Resolved: the `unity-runtime/` question

The previous version of this document flagged that `docs/sonic-runners.md`
described a working `unity-runtime/src/unity_runtime.cpp` while `unity-runtime/`
appeared to be an empty directory, and asked which was right.

Neither was wrong. `unity-runtime/` is a **git submodule**
([VNX-Unity-Runtime](https://github.com/Viridite/VNX-Unity-Runtime), see
`.gitmodules`) and shows up empty in a checkout that hasn't run
`git submodule update --init`. The module exists and has reached its M5
milestone — graphics stood up, the registered `nativeRender` loop running. It is
still early: no Unity game runs end to end yet (Brain It On! loads and links
clean, then faults in libunity's static initialisers). So a Unity title being in
the table below means the Unity path will be taken, not that the title will
play.

## What was added to Viridite

**A title database** (`include/compat/gamedb.h`, mirrored at
`Viridite/include/gamedb.h`). Header-only and dependency-free, so keeping the
two repos in step is a file copy. It holds two tables:

- **Library roles**, keyed by library name. `libfmod.so` is an audio backend,
  `libc++_shared.so` is the NDK C++ runtime, `libil2cpp.so` is compiled C# —
  none of them is ever the game. True of every title that ships them, including
  ones nobody has written a row for.
- **Titles**, keyed by package id: name, version, entry library, engine, the
  data it needs beyond the APK, and — separately from all of that — how far it
  has actually got in Viridite.

**A better answer to "which library is the game."** The Core's rule was "the
largest `.so`". That is right for a game shipping one big binary and a few small
helpers, and wrong for several titles here:

- a Unity game's largest library is `libil2cpp.so`; the entry point is
  `libmain.so`, often a few tens of KB;
- After Burner Climax ships `libfmod.so` *and* `libfmodstudio.so` alongside
  `libacb.so`;
- Final Fantasy IV 3D ships `libff4proxy.so` and `libRMS.so`, which NaGaa95's
  README says outright are not needed.

The size heuristic is still there, as the last of four answers: the title's own
documented entry library, then the largest library whose *name* is a known
engine entry point, then the largest that isn't a known dependency, then the
largest. Which rule answered goes in the log. The rules are host-tested in
[`test/gamedb/harness.cpp`](../test/gamedb/harness.cpp) — this is exactly the
decision that can't be checked on hardware without owning every APK in the
table.

**Honest per-title messaging in the launcher.** A blocked launch used to say
"Hill Climb Racing is the only game confirmed to run" for everything. For a
title in the table it now names the game, its engine, and what it needs beyond
its APK; the list row shows `UNTESTED` or `ENGINE UNSUPPORTED` instead of a flat
`INCOMPATIBLE`. **Nothing new became launchable** — Hill Climb Racing is still
the only `Playable` row, and the host test fails if a second one appears.

## The titles

Package ids are quoted from the port's README, or taken from an OBB filename it
quotes (`main.<ver>.<package>.obb` carries the package id exactly). Rows marked
"—" have no documented package id; they are in the table for reference and are
never matched at runtime, because guessing a package id would put an unverified
string in the path that decides how a game is loaded.

| Title | Ver | Package | Entry library | Engine | Beyond the APK |
|---|---|---|---|---|---|
| [Kingdom Hearts Union χ Dark Road](https://github.com/NaGaa95/KHUx_nx) | 5.0.1 | `com.square_enix.android_googleplay.khuxww` | `libcocos2dcpp.so` | cocos2d-x | two OBBs, left packed |
| [Adventures of Mana](https://github.com/NaGaa95/aom_nx) | 1.1.4 | `com.square_enix.adventures` | `libmcfandroid.so` | native | `sk1/*.mpk`, `bgm*.ogg` |
| [Chrono Trigger](https://github.com/NaGaa95/ct_nx) | 2.1.5 | `com.square_enix.android_googleplay.chrono` | `libchrono.so` | cocos2d-x | whole `assets/` tree |
| [Final Fantasy III (3D)](https://github.com/NaGaa95/ff3_3d_nx) | 2.0.6 | `com.square_enix.android_googleplay.FFIII_GP` | `libff3.so` | native | `main.obb` |
| [Final Fantasy IV (3D)](https://github.com/NaGaa95/ff4_3d_nx) | 2.0.5 | `com.square_enix.android_googleplay.FFIV_GP` | `libff4.so` | native | `main.obb` |
| [FF IV: The After Years](https://github.com/NaGaa95/ff4tay_nx) | 1.0.13 | `com.square_enix.android_googleplay.FF4AY_GP` | `libff4a.so` | native | `main.obb` |
| [Final Fantasy Dimensions](https://github.com/NaGaa95/ffd_nx) | 1.1.9 | `com.square_enix.android_googleplay.ffl_gp` | `libjniproxy.so` | native | `main.obb`, `res/raw` |
| [Final Fantasy Dimensions II](https://github.com/NaGaa95/ffd2_nx) | 1.0.7 | `com.square_enix.android_googleplay.ffl2w` | `libchange.so` | native | `main.obb` |
| [Chaos Rings III](https://github.com/NaGaa95/cr3_nx) | 1.1.4 | `com.square_enix.chaosrings3gp` | `libcrx.so` | native | `.mvgl` archives, fonts |
| [Castle of Illusion](https://github.com/NaGaa95/coi_nx) | 1.4.5 | `com.disney.castleofillusion_goo` | `libViewer_GP.so` | native | `main.154.*.obb` |
| [After Burner Climax](https://github.com/NaGaa95/abc_nx) | 0.1.7 | `com.sega.afterburnerclimax` | `libacb.so` | native | OBB (+ two FMOD libs) |
| [LEGO Ninjago: Shadow of Ronin](https://github.com/NaGaa95/lnsor_nx) | 2.2.1.02 | `com.wb.goog.lnjgo` | `libLEGO_Pixel_Mobile.so` | TT Fusion | `.fib` packs, music, cutscenes |
| [LEGO Batman 3](https://github.com/NaGaa95/lbbg_nx) | 2.2.1.05 | `com.wb.goog.lbbg` | `libLEGO_Black_Mobile.so` | TT Fusion | `data01`, `data02` |
| [LEGO SW: The Force Awakens](https://github.com/NaGaa95/lswtfa_nx) | 2.2.1.06 | `com.wb.goog.legoswtfa` | `libProject_Douglas_HH.so` | TT Fusion | `assetpack1-3` |
| [Very Little Nightmares](https://github.com/NaGaa95/vln_nx) | 1.2.6 | `eu.bandainamcoent.verylittlenightmares` | `libmain.so` | Unity 2021.3 IL2CPP | `assets/bin/Data`, optional OBB |
| [Angry Birds Journey](https://github.com/NaGaa95/angrybirdsjourney_nx) | 3.8.4 | `com.rovio.abcasual` | `libmain.so` | Unity 2022.3 IL2CPP | `assets/bin/Data` |
| [Layton Brothers: Mystery Room](https://github.com/NaGaa95/laytonbmr_nx) | 1.1.3 | `com.Level_5.MysteryRoomENG` | `libmain.so` | Unity 6000.0 IL2CPP | base APK **and** asset pack |
| [Subway Surfers](https://github.com/NaGaa95/subwaysurfers_nx) | 3.66.1 | — | `libmain.so` | Unity 2022.3 IL2CPP | `assets/bin/Data` |
| [Geometry Dash](https://github.com/NaGaa95/gdash_nx) | 2.2.14x | — | `libcocos2dcpp.so` | cocos2d-x | `assets/`, `libfmod.so`, `cacert.pem` |
| [Angry Birds 2](https://github.com/NaGaa95/angrybirds2_nx) | 26.4.3 | — | `libmain.so` | Unity IL2CPP | `assets/bin/Data` |
| [Animal Crossing: Pocket Camp Complete](https://github.com/NaGaa95/acpc_nx) | 7.1.3 | — | `libmain.so` | Unity IL2CPP | `assets/bin/Data`, `libTone.so` |
| [Max Payne Mobile](https://github.com/NaGaa95/max_nx_v2.1.131) | 2.1.131 | — | `libGame.so` | native | `.msf` sound, `x_*.ras`, `data/`, `es2/` |
| [Cut the Rope](https://github.com/NaGaa95/ctr_nx) | 3.79.0 | — | `libctro.so` | native | `assets/`, `res/` |
| [Cut the Rope 2](https://github.com/NaGaa95/ctr2_nx) | 1.47.1 | — | `libctr2.so` | native | `assets/`, `res/` |
| [Jetpack Joyride](https://github.com/NaGaa95/jetpackjoyride_nx) | 1.104.1 | — | `libmortargame.so` | native | `assets/assets.zip` |
| [Swordigo](https://github.com/NaGaa95/swordigo_nx) | 1.4.12 | — | `libswordigo.so` | native | `assets/resources`, `res/*.mp3` |
| [Castlevania: SOTN](https://github.com/NaGaa95/sotn_nx) | 1.0.6 | — | `libsotn.so` | native | `assets/res2` — zip in OBB in zip |
| [LEGO SW: The Complete Saga](https://github.com/NaGaa95/lswtcs_nx) | 2.0.2.02 | — | `libTTapp.so` | TT Fusion | four `.dat` archives |
| [Layton: Curious Village HD](https://github.com/NaGaa95/layton_nx) | 1.0.8 | — | `libll1.so` | native | `assets/` language trees |
| [Layton: Pandora's Box HD](https://github.com/NaGaa95/layton2_nx) | 1.0.6 | — | `libll2.so` | native | `assets/` language trees |
| [Layton: Lost Future HD](https://github.com/NaGaa95/layton3_nx) | 1.0.3 | — | `libll3.so` | native | `assets/` language trees |
| [GTA: San Andreas](https://github.com/NaGaa95/gtasa_nx) | 2.11.311 | — | `libGame.so` | native | `main.*.obb` unpacked |
| [GTA: Liberty City Stories](https://github.com/NaGaa95/gtalcs_nx) | 2.4.379 | — | `libGame.so` | native | `.wad` archives, `intro.m4v` |
| [GTA: Chinatown Wars](https://github.com/NaGaa95/gtactw_nx) | 4.4.243 | — | `libGame.so` | native | `game.pak`, `dxt.bin`, text, music |
| [Half-Life 2](https://github.com/NaGaa95/hl2_nx) | 1.16.29+ | — | *(none)* | Source | VPKs + ~27 libraries |
| [Counter-Strike: Source](https://github.com/NaGaa95/css_nx) | 1.09_96 | — | *(none)* | Source | VPKs + engine libraries |
| [Team Fortress 2 ReClassic](https://github.com/NaGaa95/tf2_nx) | 1.0.0 | — | *(none)* | Source | VPKs + ~29 libraries |

Three of NaGaa95's `libGame.so` titles share that library name, which is why
titles are identified by package and never by library: identity comes from the
package, engine and entry-point knowledge comes from the library name, and the
two are kept in separate tables.

## Deliberately not in the table

- **Emulators and recompilations** — Vita3K, Dolphin, Cemu, RPCS3, NetherSX2,
  DrasticDS, the Sonic and Skate 3 static recompilations. These are native
  Switch programs, not Android games; there is nothing for a translation layer
  to translate.
- **Online-only titles** — Genshin Impact and similar. Viridite's stated focus
  is games that don't require connectivity, and a title that cannot start
  without a live service can't be assessed here.
- **Ports with no README yet** — `bully_nx` and `lbdcsh_nx` (LEGO Batman: DC
  Super Heroes) are empty repositories at the time of writing. There is nothing
  to transcribe, and a row invented from the game's name would be exactly the
  guesswork this table is arranged to avoid. Worth revisiting when they land.
- **Source engine games** — listed, but as `Unsupported`. A Source game is
  ~30 libraries whose entry point lives in the Java launcher rather than in any
  one of them, and the Android builds are themselves a port (nillerusr's)
  rather than a stock NDK game. They are in the table so they are recognised
  and refused with a reason, not loaded until something faults.

## What still needs hardware

Everything that would make one of these rows say `Playable`. The bar is
`source/compat/games/game_hillclimb.cpp`: signature-gated patches at addresses
found by watching the game fault on a real Switch. Nothing here has run. In
rough order of promise:

1. **Geometry Dash** — cocos2d-x 2.2, the same engine family already driving
   Hill Climb Racing, and the only one of these whose engine path is proven
   here end to end. Its `libfmod.so` also makes it the first real test of the
   entry-library rule against a live game.
2. **The Unity IL2CPP titles** — Very Little Nightmares, Angry Birds Journey,
   Layton Brothers, Subway Surfers, Angry Birds 2. These stand or fall with
   VNX-Unity-Runtime rather than with anything per-title, so the next Unity
   milestone moves all five at once. Subway Surfers' swipe synthesis from
   stick/d-pad is a design worth reusing rather than reinventing.
3. **Chrono Trigger and KH Union χ** — also cocos2d-x, but both need data
   Viridite currently has no path for: KHUx needs its OBBs mounted, Chrono
   Trigger needs the whole assets tree present.
4. **OBB support in general** — nine of these titles ship game data in an OBB.
   `compat/android.h` has an `obbPath` field and nothing behind it. Until that
   exists, those titles cannot get past their first asset read no matter how
   well the loader does.
