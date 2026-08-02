// ─── Hill Climb Racing (com.fingersoft.hillclimb) — Cocos2d-x ───────────────
// All HCR-specific fixups live here, keyed by package + soname, so they can
// never fire for another title (e.g. the Unity path). Everything is
// signature-gated against this exact libgame.so (v1.67.0,
// sha256 542c25e5…c07b150a) so a mismatched build is left untouched.
#include "compat/games.h"
#include "compat/loader.h"
#include <cstring>

namespace {

constexpr char   kPkg[]   = "com.fingersoft.hillclimb";
constexpr char   kSoName[] = "libgame.so";
constexpr uint32_t kNop   = 0xd503201f;

bool inRange(uint64_t vaddr, uint64_t min_vaddr, size_t alloc_size) {
    return vaddr >= min_vaddr && vaddr + 4 <= min_vaddr + alloc_size;
}

// Quirk 1 — Shop/IAP crash.
// The Google-Play shop builder populates its product vector from a store
// backend that doesn't exist here, leaving it empty, then unconditionally reads
// items[size-1] — a load from -8, a hard crash the moment the Shop opens. The
// populate call and its result are otherwise discarded, so NOPing it skips the
// doomed populate: the Shop opens empty (nothing is purchasable here anyway)
// instead of taking the app down. Call site 0x22bbc8:
//   mov x0,x19 / bl 0x232240 / ldr x0,[x19,#0x360]
void patchShopPopulate(uint8_t* base, uint64_t min_vaddr, size_t alloc_size) {
    constexpr uint64_t site = 0x22bbc8;
    if (!inRange(site, min_vaddr, alloc_size)) return;
    uint32_t* at = (uint32_t*)(base + site);
    if (at[-1] != 0xaa1303e0u ||   // mov x0, x19
        at[ 0] != 0x9400199eu ||   // bl 0x232240 (shop-populate)
        at[ 1] != 0xf941b260u) {   // ldr x0, [x19, #0x360]
        return;
    }
    at[0] = kNop;
    compatLog("quirk[HCR]: NOP'd Shop product-list populate at +0x22bbc8 "
              "(empty-vector crash; shop opens empty, no billing backend)");
}

// Quirk 2 — racing crash on the vehicle skin lookup.
// SkinProvider::find("jeep") (bl 0x387350) returns null when the skin map is
// empty, and the caller reads the returned std::string's size byte with no null
// check (ldrb w8,[x0] at 0x31354c) — a null deref the instant a race starts.
// After the length check the string is never touched again (the rest uses the
// vehicle object in x19), and the len!=6 path just sets "no skin matched", so
// turning the deref into `cbz x0, 0x31358c` makes a missing skin fall through
// gracefully (worst case: no custom skin) instead of crashing. Signature:
//   add x0,sp,#0x10 / bl 0x387350 / ldrb w8,[x0] / ldr x9,[x0,#8]
void patchSkinLookupGuard(uint8_t* base, uint64_t min_vaddr, size_t alloc_size) {
    constexpr uint64_t site = 0x31354c;
    if (!inRange(site, min_vaddr, alloc_size)) return;
    uint32_t* at = (uint32_t*)(base + site);
    if (at[-1] != 0x9401cf82u ||   // bl 0x387350 (SkinProvider::find)
        at[ 0] != 0x39400008u ||   // ldrb w8, [x0]   ← the crashing load
        at[ 1] != 0xf9400409u) {   // ldr  x9, [x0, #8]
        return;
    }
    at[0] = 0xb4000200u;           // cbz x0, #0x31358c  (skip to "no skin" path)
    compatLog("quirk[HCR]: guarded null SkinProvider::find at +0x31354c "
              "(cbz x0 → +0x31358c; racing no longer crashes on a missing skin)");
}

// Quirk 3 — second Shop crash, further into the same builder.
// Quirk 1 stops the product vector being populated from a store backend that
// isn't there, but this site still walks that (now empty) vector and calls a
// virtual method on whatever it finds:
//   ldr x10,[x19,#1664] / ldr x9,[x19,#1656]   ; end, begin
//   sub/asr #3/sub #1                          ; (count - 1)
//   cmp x10,x8 / b.cs                          ; UNSIGNED bounds check
//   ldr x0,[x8]   (0x22c020)                   ; element pointer
//   ldr x8,[x0]   (0x22c024)                   ; ← vtable load, x0 is null
//   ldr x8,[x8,#728] / blr x8                  ; virtual call
// With the vector empty, count-1 underflows to 0xFFFF…, so the unsigned
// compare passes and it reads past the end — hardware logs show exactly this,
// far=0x0 at +0x22c024 killing a session after a few minutes.
//
// The whole block is shop-item work that cannot succeed without a billing
// backend, so it's branched over rather than guarded.
//
// The first version of this patched 0x22c020 (the element load) and hardware
// showed it applied — yet the crash repeated at 0x22c024 with the branch
// plainly present one instruction earlier:
//   INSN: [pc-4]=14000005 [pc]=f9400008
// So something reaches the dereference without passing through 0x22c020;
// there is at least one more branch into this block than the disassembly
// around it revealed. Patching the FAULTING instruction instead covers every
// path into it, whether or not each one has been found — 0x22c024 becomes
// `b +0x10`, skipping the vtable read and the call regardless of how it was
// entered. 0x22c020 is left alone so the element load still behaves normally
// for any path where it matters.
void patchShopItemVirtualCall(uint8_t* base, uint64_t min_vaddr, size_t alloc_size) {
    constexpr uint64_t site = 0x22c020;      // signature anchor
    if (!inRange(site, min_vaddr, alloc_size)) return;
    uint32_t* at = (uint32_t*)(base + site);
    if (at[ 0] != 0xf9400100u ||   // ldr x0, [x8]
        at[ 1] != 0xf9400008u ||   // ldr x8, [x0]      ← the crashing load
        at[ 2] != 0x52800021u ||   // mov w1, #1
        at[ 3] != 0xf9416d08u ||   // ldr x8, [x8, #728]
        at[ 4] != 0xd63f0100u) {   // blr x8
        return;
    }
    at[1] = 0x14000004u;           // 0x22c024: b #+0x10 → 0x22c034, past the call
    compatLog("quirk[HCR]: skipped empty-shop virtual call at +0x22c024 "
              "(null vtable deref; branches over the call from every path)");
}

}  // namespace

// ─── Registry entry points ──────────────────────────────────────────────────
// (The dispatcher lives here while HCR is the only title with quirks; add a
//  game_<title>.cpp and route to it here when another game needs fixups.)

void gameApplyQuirks(const char* pkg, const char* soname,
                     uint8_t* stage_base, uint64_t min_vaddr, size_t alloc_size) {
    const bool isHcr = (pkg && strcmp(pkg, kPkg) == 0) ||
                       (soname && strcmp(soname, kSoName) == 0);
    if (isHcr && soname && strcmp(soname, kSoName) == 0) {
        patchShopPopulate(stage_base, min_vaddr, alloc_size);
        patchSkinLookupGuard(stage_base, min_vaddr, alloc_size);
        patchShopItemVirtualCall(stage_base, min_vaddr, alloc_size);
    }
}

int gameMarketVariation(const char* pkg) {
    if (pkg && strcmp(pkg, kPkg) == 0)
        return 1;   // claim Google Play so the shop builder takes a sane branch
    return -1;       // no opinion — caller keeps its own default
}
