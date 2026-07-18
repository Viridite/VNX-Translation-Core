// Import bridge: a guest call that lands on a sentinel PC runs the native arm64
// shim it stands for. Each wrapper marshals the AArch32 AAPCS (r0-r3 + stack)
// into a real call, translating guest pointers to host pointers. Unhandled
// imports are logged (this is how the set grows, log-driven).
#include "arm32/arm32_internal.h"
#include "compat/loader.h"
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

namespace a32 {

struct Import { std::string name; };
static std::vector<Import> s_imports;

uint32_t bridgeRegister(const char* name) {
    for (size_t i = 0; i < s_imports.size(); i++)
        if (s_imports[i].name == name) return A32_SENTINEL_BASE + (uint32_t)i * 4;
    s_imports.push_back({name ? name : "?"});
    return A32_SENTINEL_BASE + (uint32_t)(s_imports.size() - 1) * 4;
}

// Convenience accessors over the guest CPU state / stack.
static inline uint32_t arg(CpuState& c, int i) {
    if (i < 4) return c.r[i];
    return *(uint32_t*)toHost(c.r[13] + (uint32_t)(i - 4) * 4);   // stacked args
}
static inline char*  hstr(uint32_t g) { return g ? (char*)toHost(g) : nullptr; }
static inline void*  hptr(uint32_t g) { return g ? (void*)toHost(g) : nullptr; }

// strlen bounded to the region — never scans past the guest window.
static uint32_t gStrlen(uint32_t g) {
    if (!g || g >= g_region) return 0;
    const char* p = (const char*)toHost(g);
    uint32_t max = (uint32_t)(g_region - g), n = 0;
    while (n < max && p[n]) n++;
    return n;
}
// Bounded copy: at most `n` bytes, and never past the region for either side.
static void gStrcpy(uint32_t d, uint32_t s, uint32_t n) {
    if (!d || !s) return;
    if (!guestValid(d, n) || !guestValid(s, n)) {
        uint32_t dmax = d < g_region ? g_region - d : 0;
        uint32_t smax = s < g_region ? g_region - s : 0;
        n = n < dmax ? n : dmax; n = n < smax ? n : smax;
    }
    if (n) memcpy(toHost(d), toHost(s), n);
}

// Returns true if it handled `name`; sets ret (r0) via out.
static bool dispatch(CpuState& c, const char* name, uint32_t& ret) {
    // ── allocator: must stay inside the guest region and return guest addrs ──
    if (!strcmp(name, "malloc"))  { ret = guestAlloc(arg(c,0)); return true; }
    if (!strcmp(name, "free"))    { guestFree(arg(c,0)); ret = 0; return true; }
    if (!strcmp(name, "calloc"))  { uint32_t n = arg(c,0)*arg(c,1); uint32_t p = guestAlloc(n); if (p) memset(toHost(p),0,n); ret = p; return true; }
    if (!strcmp(name, "realloc")) { ret = guestRealloc(arg(c,0), arg(c,1)); return true; }
    if (!strcmp(name, "memalign") || !strcmp(name, "aligned_alloc")) { ret = guestAlloc(arg(c,1)); return true; }
    if (!strcmp(name, "posix_memalign")) { uint32_t p = guestAlloc(arg(c,2)); if (arg(c,0)) *(uint32_t*)toHost(arg(c,0)) = p; ret = p ? 0 : 12; return true; }

    // ── mem/str: translate pointer args, return values may be guest pointers.
    //    Bounds-check every length against the guest region so a bad pointer or
    //    size can never write into the Core's own host memory. ──
    if (!strcmp(name, "memcpy") || !strcmp(name, "memmove")) {
        uint32_t d=arg(c,0), s=arg(c,1), n=arg(c,2);
        if (guestValid(d,n) && guestValid(s,n)) memmove(hptr(d), hptr(s), n);
        else compatLogFmt("arm32: %s OOB d=0x%x s=0x%x n=0x%x — skipped", name, d, s, n);
        ret = d; return true;
    }
    if (!strcmp(name, "memset")) {
        uint32_t d=arg(c,0), n=arg(c,2);
        if (guestValid(d,n)) memset(hptr(d), (int)arg(c,1), n);
        else compatLogFmt("arm32: memset OOB d=0x%x n=0x%x — skipped", d, n);
        ret = d; return true;
    }
    if (!strcmp(name, "memcmp")) {
        uint32_t a=arg(c,0), b=arg(c,1), n=arg(c,2);
        ret = (guestValid(a,n) && guestValid(b,n)) ? (uint32_t)memcmp(hptr(a), hptr(b), n) : 0;
        return true;
    }
    // String ops are bounded to the region so an unterminated guest string can't
    // walk off into host memory (gStrlen caps its scan at the region end).
    if (!strcmp(name, "strlen"))  { ret = gStrlen(arg(c,0)); return true; }
    if (!strcmp(name, "strcmp") || !strcmp(name, "strncmp")) {
        uint32_t a=arg(c,0), b=arg(c,1);
        uint32_t n = name[3]=='n' ? arg(c,2) : gStrlen(a)+1;
        if (!a || !b) ret = (a==b) ? 0 : 1;
        else ret = (uint32_t)strncmp(hstr(a), hstr(b), n);
        return true;
    }
    if (!strcmp(name, "strcpy"))  { gStrcpy(arg(c,0), arg(c,1), gStrlen(arg(c,1))+1); ret = arg(c,0); return true; }
    if (!strcmp(name, "strncpy")) { gStrcpy(arg(c,0), arg(c,1), arg(c,2)); ret = arg(c,0); return true; }
    if (!strcmp(name, "strcat"))  { uint32_t d=arg(c,0); gStrcpy(d + gStrlen(d), arg(c,1), gStrlen(arg(c,1))+1); ret = d; return true; }
    if (!strcmp(name, "strchr"))  { uint32_t L=gStrlen(arg(c,0)); void* p = arg(c,0)?memchr(hptr(arg(c,0)), (int)arg(c,1), L+1):nullptr; ret = p ? toGuest(p) : 0; return true; }

    // ── C++ static-init / exit registration: safe no-ops here ──
    if (!strcmp(name, "__cxa_atexit") || !strcmp(name, "__cxa_finalize") ||
        !strcmp(name, "__register_atfork") || !strcmp(name, "atexit")) { ret = 0; return true; }
    if (!strcmp(name, "__cxa_guard_acquire")) { uint8_t* g = (uint8_t*)hptr(arg(c,0)); ret = g && *g ? 0 : 1; return true; }
    if (!strcmp(name, "__cxa_guard_release")) { uint8_t* g = (uint8_t*)hptr(arg(c,0)); if (g) *g = 1; ret = 0; return true; }

    return false;
}

void bridgeCall(CpuState& c, uint32_t sentinel) {
    uint32_t idx = (sentinel - A32_SENTINEL_BASE) / 4;
    const char* name = (idx < s_imports.size()) ? s_imports[idx].name.c_str() : "?";

    uint32_t ret = 0;
    if (!dispatch(c, name, ret)) {
        static int warned = 0;
        if (warned < 200) { warned++; compatLogFmt("arm32: UNIMPL import %s (r0=0x%x r1=0x%x r2=0x%x)", name, c.r[0], c.r[1], c.r[2]); }
        ret = 0;   // best-effort stub
    }
    c.r[0] = ret;
    c.r[15] = c.r[14];                 // return to LR
    c.cpsr = (c.r[14] & 1) ? (c.cpsr | 0x20) : (c.cpsr & ~0x20u);  // LR bit0 sets Thumb
    c.r[15] &= ~1u;
}

}  // namespace a32
