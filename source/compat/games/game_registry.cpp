// ─── Per-title dispatcher ───────────────────────────────────────────────────
// Routes the shared translation layer's per-game hooks to the right title's
// game_<name>.cpp, and answers the questions whose answer is data rather than
// code out of compat/gamedb.h.
//
// This used to live at the bottom of game_hillclimb.cpp, with a note saying it
// could stay there while Hill Climb Racing was the only title with quirks.
// Splitting it out now means a new title is a new file plus one line here,
// instead of an edit inside another game's fixups — which is the arrangement
// the isolation in games.h was for in the first place.
#include "compat/games.h"
#include "compat/gamedb.h"
#include "compat/loader.h"
#include <cstring>

void gameApplyQuirks(const char* pkg, const char* soname,
                     uint8_t* stage_base, uint64_t min_vaddr, size_t alloc_size) {
    if (!soname) return;

    // Hill Climb Racing. Matched on either the package or the library name:
    // libgame.so is Fingersoft's naming, and a launch that lost its package id
    // still gets the fixups it needs. Every patch inside is signature-gated
    // against the exact build, so a same-named library from another game is
    // left untouched rather than mispatched.
    const bool isHcr = (pkg && strcmp(pkg, "com.fingersoft.hillclimb") == 0) ||
                       strcmp(soname, "libgame.so") == 0;
    if (isHcr && strcmp(soname, "libgame.so") == 0)
        hcrApplyQuirks(soname, stage_base, min_vaddr, alloc_size);
}

int gameMarketVariation(const char* pkg) {
    const gamedb::Title* t = gamedb::findByPackage(pkg);
    return t ? t->marketVariation : -1;
}
