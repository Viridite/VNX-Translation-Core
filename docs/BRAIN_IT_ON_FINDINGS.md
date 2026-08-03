# Brain It On (com.orbital.brainiton) — root cause of the constructor faults

## Summary

The 155 "constructor faults" are not the game's constructors failing. They are
**heap corruption**: every one of them is the same instruction inside newlib's
`free()`, unlinking a chunk from the free list through a NULL forward pointer.

Any constructor that frees memory hits it, which is why the count is large and
why the addresses looked unrelated.

## Evidence

All 155 faults report one PC and one instruction window:

```
esr=0x92000046  far=0x18  (data abort, translation fault L2, WRITE to 0x18)
INSN: [pc-12]=eb01001f [pc-8]=54000940 [pc-4]=f9400cc1 [pc]=f9000c01 [pc+4]=f9000820
```

Searching that exact five-instruction sequence:

| Binary | Matches |
|---|---|
| libmain.so, libgpg.so, libunity.so, libil2cpp.so | **0** |
| Viridite-Translation-Core-x64 | **1** |

It is not in the game at all. In the Core it disassembles at `_free_r+0x68`:

```
ldr x0, [x6, #16]     ; chunk->fd
cmp x0, x1
b.eq  <bin empty>
ldr x1, [x6, #24]     ; chunk->bk
str x1, [x0, #24]     ; fd->bk = bk     <-- faults, x0 == NULL
str x0, [x1, #16]     ; bk->fd = fd
```

`far=0x18` is `offsetof(chunk, bk)` — a null `fd` plus 24. This is the classic
signature of a doubly-linked free-list whose links have been overwritten.

That also explains why the fault PC sat outside every module's reported
`code_exec` base: it was never in a module. The earlier symbolizer fix is what
made this visible — before it, the address was mis-attributed to
`UnitySendMessage+0x55d608ea4`, a symbol 23GB away, which sent the
investigation after the wrong code entirely.

## Why the loader then stalls

With the free list damaged, `launchApk` logs "loading complete" and never
returns: its locals' destructors call `free()`, which either faults again or
spins in a corrupted list. The loader thread therefore never sets `done`, the
main thread waits on it forever, and the UI sits on "running constructors".
The stall is a symptom, not a separate bug.

## What this rules out

- **Not a missing allocator.** `malloc`, `free`, `operator new` and
  `operator delete` all resolve; the unresolved list for this session contained
  no allocator symbols.
- **Not the game's own code.** The faulting instruction does not exist in any
  of its four shared objects.
- **Not the audio lock hazard.** That was a real bug and is fixed, but it can
  only stall, not corrupt a free list.

## Where to look next

Something writes past the end of a heap allocation before/while the game's
constructors run. The candidates are all in the Core, and all handle
attacker-shaped input (the APK's own data) at scale:

1. **ELF loading** — libil2cpp.so is 56MB with 9,846 symbols in libunity.so.
   Segment sizing, `p_memsz` vs `p_filesz` zero-fill, and the relocation
   loops are the highest-risk arithmetic in the project.
2. **Symbol/string table handling** — `elfNearestSym` and the symbol
   resolution path index into `strtab` using `st_name` with a bounds check;
   other readers of the same tables may not.
3. **The shim table's string helpers** — anything writing into a
   caller-supplied buffer without a length.

A practical next step is to build the Core with a heap-checking allocator (or
add guard bytes around loader allocations) and run Brain It On until the first
corrupted chunk is detected, rather than waiting for `free()` to trip over it
much later.
