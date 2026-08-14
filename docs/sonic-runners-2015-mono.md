# The original 2015 Sonic Runners is Mono, not IL2CPP — Route B needs correcting

## Why this doc exists

`sonic-runners.md`'s Route B (the original 2015 APK) opens by correctly
flagging the real fork in the road — *"if `assets/bin/Data/Managed/` holds
`.dll` files rather than a `global-metadata.dat`, it is Unity 5.x Mono... a
completely different problem (a Mono runtime and JIT, not a CPU
interpreter)"* — but never actually resolves that question, and the rest of
Route B (reuse `source/arm32/`'s CPU interpreter, budget for "a 30 MB IL2CPP
binary interpreted instruction by instruction") proceeds as if the answer
were IL2CPP anyway. It isn't. This doc resolves it and corrects the plan.

## The shape, confirmed

[MattKC's RunnersDecomp](https://github.com/itsmattkc/RunnersDecomp) (full
credit to **MattKC**, with **Ramen2X** — a source-level decompilation of the
real, shipped v2.0.3 client) states its own target directly: **Unity 4.6.9**.
IL2CPP did not exist as a Unity scripting backend until 5.x — a 4.6.9 build
cannot be IL2CPP, full stop. This is also consistent with *how* the
decompilation itself was done: standard Mono/.NET decompilers (the
`ILSpy`/`dnSpy` family) turn a Mono `Assembly-CSharp.dll` directly back into
near-original, readable C# — which is exactly what RunnersDecomp is, and
exactly why MattKC's own video/write-ups describe the process as
comparatively straightforward. IL2CPP decompilation is a different, much
more involved job (dumping a stripped native binary against synthetic
metadata, e.g. via Il2CppDumper) and isn't what happened here.

So: **the original 2015 Sonic Runners' game logic ships as portable .NET IL
(`Assembly-CSharp.dll`), not compiled ARM machine code.** There is no "30 MB
binary" to CPU-interpret for the game logic. That framing belongs to the
IL2CPP case (Route A / the Revival remake) and doesn't apply here.

## What this actually changes

This is **not** a CPU-interpretation problem the way Route A/Hill Climb
Racing's `arm32`/`arm64` work is. It's an embedding problem, architecturally
closer to `unity-runtime`'s own IL2CPP approach than to `source/arm32/`:

- Mono's own runtime (`libmono.so` in a real Mono-Android APK) **is itself a
  native ARM `.so`** — loadable through the Core's existing ELF loader the
  same way `libunity.so`/`libil2cpp.so` already are. The difference from the
  IL2CPP path is which embedding API gets called after `dlopen` —
  `mono_jit_init`/`mono_domain_assembly_open`/`mono_runtime_invoke` instead
  of `il2cpp_init` — not a different loading mechanism.
- Once that's up, `Assembly-CSharp.dll` runs through **Mono's own JIT**,
  which is what it's for and is not slow the way externally single-stepping
  someone else's native code is. The "budget honestly, this is a project not
  a task" warning in Route B was costing the wrong operation.
- What's actually still open and unverified (this pass had no APK, no
  hardware, no Unity/Mono toolchain in the sandbox to confirm any of it):
  whether an original Sonic Runners APK truly ships `libmono.so` + `.dll`s
  in this exact shape (RunnersDecomp proves the *game* is Mono; it doesn't
  hand us the *APK's* file layout — `unzip -l` a real copy before assuming),
  and what Mono runtime version/ABI it embeds.

## A real, source-verified fact worth knowing: the game logic has almost no native interop of its own

Checked directly against RunnersDecomp's ~1,896 `.cs` files under
`Assets/Scripts/`: exactly **one** `[DllImport]` outside of NGUI/CriWare's
own bundled plugin code, and it's an unused `TestMultiply`/`TestDivide`
stub — boilerplate left over from a Unity native-plugin tutorial, not real
gameplay code. The overwhelming majority of actual gameplay logic (`App`,
`Player`, `Boss`, `Chao`, `Mission`, `GameScore`, `Tutorial`, `UI`, ~17
top-level namespaces) is plain C# against Unity's standard
`MonoBehaviour`/`Update` API surface, plus NGUI (a mostly-C# UI framework
bundled the same way).

This matters a lot: unlike Hill Climb Racing's IL2CPP path (which needed
signature-gated, per-crash-log binary instruction patches — see
`source/compat/games/game_hillclimb.cpp`), **once a Mono + Unity-4.x engine
shim foundation exists, this specific game's logic shouldn't need per-title
binary patching at all** — it's ordinary managed code, not compiled-and-then-
patched native code. The hard part moves entirely into building that one
foundation (Mono embedding + enough of the Unity 4.x native API), not into
per-game archaeology afterward.

## This is generic engine capability, not a one-game fix

Worth being explicit about, since it changes how this should be prioritized:
Mono embedding isn't Sonic-Runners-specific work any more than
`unity-runtime`'s IL2CPP support is Hill-Climb-Racing-specific. Every
Android game built on Unity 4.x/early-5.x with the (then-default) Mono
scripting backend hits the same wall and would clear it the same way. That's
a large slice of the pre-2018-ish Unity mobile catalogue — this is worth
treating as "add a second engine path alongside IL2CPP" in its own right,
not as a Sonic-Runners feature branch.

## The real remaining native dependency: CriWare

`Assets/Plugins/CriWare details.txt` and the empty
`GET AND PLACE CRIWARE v1.20.3.0 HERE` placeholder in RunnersDecomp confirm
what its README says outright: **CriWare Unity SDK v1.20.3.0 is a
proprietary, separately-licensed plugin**, not includable in an open
decompilation and not includable here either. It's the game's audio/video
middleware. Two honest options, neither resolved by this doc: get the real
`libcri_ware_unity.so` from a legally-owned APK and load it the same way the
IL2CPP path already plans to (see `sonic-runners.md`'s own note that IL2CPP
`dlopen`s this exact library too — CriWare is shared infrastructure between
both Sonic Runners generations), or treat CriWare-dependent audio as a gap
to stub gracefully, the same way Route A already plans to no-op a missing
`discord-rpc.so` rather than treat it as fatal.

## Also worth knowing: a private-server project already exists

RunnersDecomp's own credits list **fluofoxxo**'s
[server reimplementation](https://github.com/fluofoxxo/outrun) for the
original game's now-dead backend. `sonic-runners.md` already makes the right
point that a network failure and an engine failure look identical from a
black screen — worth confirming the client can actually reach a live server
*before* spending time on any of the above, for the same reason.

## Update: a real, working Mono-on-Switch port already exists

This should have been checked before the "what's still not done" section
below was originally written as if Mono-on-Switch were untried. It isn't.
**Full credit to [exelix11](https://github.com/exelix11)** for
[mono-nx](https://github.com/exelix11/mono-nx): a real, working, documented
port of the Mono runtime to the devkitA64/libnx homebrew toolchain, running
real .NET 9 DLLs/EXEs on real Switch hardware via Mono's interpreter (plus
an AOT path). It's explicitly proof-of-concept quality and the author has
paused active development, but it *works*, and — more valuably than the
code itself — the author wrote up the actual engineering journey in
[`notes/writeup.md`](https://github.com/exelix11/mono-nx/blob/master/notes/writeup.md),
531 lines of exactly the kind of hard-won, specific findings this doc could
only speculate about before.

Two caveats before over-crediting this as a drop-in solution:

1. mono-nx embeds a **modern Mono** (the Mono component inside
   `dotnet/runtime`, via [the author's own libnx-patched fork](https://github.com/exelix11/dotnet_runtime/tree/libnx)),
   not the old, standalone, Unity-specific Mono fork that Unity 4.6.9
   actually embeds. It's the same lineage and proves the *category* of
   problem is solvable, but it is not the same runtime Sonic Runners ships
   — don't assume API/build-system compatibility without checking.
2. The author is explicit that arbitrary P/Invoke doesn't work (only
   statically-linked-in native entrypoints do — no `dlopen`), and that full
   JIT (as opposed to interpreter-only) hits real, only-partially-solved
   blockers. Relevant here because a real game's native interop surface
   (however small — see the single-DllImport finding above) still needs
   *some* story for calling into native code.

### The single most valuable finding, and it's not Mono-specific at all

Switch's actual JIT memory model is not simple W^X the way it is on most
platforms (allocate RW, write code, flip to RX at the *same address*).
Quoting the mechanism directly because it's exactly right and worth having
verbatim: on the default modern-firmware backend (`JitType_CodeMemory`),
**the RW and RX views of a JIT region are two different mapped addresses
for the same underlying memory** (not classic same-address RWX, but not
strictly the naive "swap protection bits" model either) — see
[libnx's jit.c](https://github.com/switchbrew/libnx/blob/master/nx/source/kernel/jit.c)
and the [Atmosphère kernel implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_code_memory.cpp)
that exelix11 dug into directly to confirm this, rather than guessing from
higher-level docs.

This is **directly relevant to this Core's own dynamic-code work**, not
just to a hypothetical Mono path — `sonic-runners.md`'s own "D7/B2.10, cache
maintenance" section already deals with writing patched instructions into
a page before it's executable, which is exactly the class of problem this
is. Worth reading `notes/writeup.md`'s "JIT? In my interpreter?" section in
full before this Core does any new work in that area, rather than
re-deriving the same lessons the hard way:

- The two-view model breaks the common "allocate once, write, flip
  protection" JIT idiom outright — code needs a codeman-style abstraction
  that tracks separate RW/RX addresses for the same logical region, with
  offset-preserving translation between them.
- The kernel enforces a small, fixed limit on the number of live JIT code
  regions (mono-nx hit exhaustion — error `0xce01` — after roughly 10
  allocations using a naive "one region per chunk" strategy). One big
  reserved region, filled incrementally, is the workaround mono-nx landed
  on — worth designing for from the start rather than discovering this the
  same way.
- There's an unmap/remap trick to get a true single-address RW↔RX toggle
  via raw `svcControlCodeMemory`, but it reintroduces a real correctness
  hazard if any other thread might be executing code in that region while
  it happens — genuinely needs a suspend-all-managed-threads story, not
  something to reach for casually.

## What's still not done here

No code changes. This is a corrected/extended research doc, the same as
`sonic-runners.md` was. Real next steps, in order, none of them possible
from a sandbox with no APK, no hardware, and no Mono/Unity toolchain:

1. Get an actual, legally-owned Sonic Runners (2015) APK and confirm its
   real file layout (`libmono.so` version, `.dll` set, ABI) before assuming
   anything above holds exactly.
2. Confirm a Mono runtime can be embedded in the Core's process the way
   `unity-runtime` is scoped to embed IL2CPP — this is genuinely new
   engineering, not a variant of existing `arm32`/`arm64` work, though
   mono-nx meaningfully de-risks it (real precedent it's possible at all)
   and its codeman/JIT-memory findings above should save real debugging
   time regardless of which Mono generation ends up used.
3. Only then does "does the actual game logic just run" become testable —
   which, per the DllImport count above, is the more optimistic bet of the
   two Sonic Runners generations once the foundation exists.
