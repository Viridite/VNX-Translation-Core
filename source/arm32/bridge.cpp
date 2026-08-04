// Import bridge: a guest call that lands on a sentinel PC runs the native arm64
// shim it stands for. Each wrapper marshals the AArch32 AAPCS (r0-r3 + stack)
// into a real call, translating guest pointers to host pointers. Unhandled
// imports are logged (this is how the set grows, log-driven).
#include "arm32/arm32_internal.h"
#include "compat/loader.h"
#include <GLES2/gl2.h>
#include <utility>
#include <cerrno>
#include <sys/stat.h>
#include <switch.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <strings.h>
#include <vector>
#include <string>

namespace a32 {

struct Import { std::string name; };
static std::vector<Import> s_imports;

// Guest FILE* handle table: a guest "FILE*" is (index into s_files)+1, so 0 is
// a clean NULL. gfile() maps a handle back to the host FILE*.
static std::vector<FILE*> s_files;
static FILE* gfile(uint32_t h) {
    // &__sF[0..2] are the standard streams. The game gets these by taking an
    // address inside the imported array rather than by calling fopen, so they
    // never pass through the handle table and have to be recognised here.
    if (g_a32_sF && h >= g_a32_sF && h < g_a32_sF + 3 * A32_FILE_STRIDE) {
        switch ((h - g_a32_sF) / A32_FILE_STRIDE) {
            case 0: return stdin;
            case 1: return stdout;
            default: return stderr;
        }
    }
    return (h && h <= s_files.size()) ? s_files[h-1] : nullptr;
}

// ── Imported data symbols ───────────────────────────────────────────────────
// Handing these a call sentinel is what produced the repeated
// "OOB rd32 at guest 0xe0000000" in the log: the game was dereferencing an
// address that only means anything as a jump target. They need storage with
// the right contents instead.
static uint32_t s_data_next = A32_DATA_BASE;

static uint32_t dataAlloc(uint32_t bytes) {
    bytes = (bytes + 15) & ~15u;
    if (s_data_next + bytes > A32_DATA_BASE + A32_DATA_SIZE) return 0;
    uint32_t g = s_data_next;
    s_data_next += bytes;
    memset(toHost(g), 0, bytes);
    return g;
}

// Guest addresses of the three standard streams inside our fake __sF, so the
// file bridge can recognise them when the game passes &__sF[n] to fprintf.
uint32_t g_a32_sF = 0;

uint32_t bridgeRegisterData(const char* name) {
    if (!name) return 0;

    if (!strcmp(name, "__sF")) {
        // bionic's FILE is opaque and the game only ever takes addresses out of
        // this array, so the contents do not matter — only that &__sF[0..2] are
        // distinct, stable, readable addresses the bridge can map back to
        // stdin, stdout and stderr.
        if (!g_a32_sF) {
            g_a32_sF = dataAlloc(3 * A32_FILE_STRIDE);
            compatLogFmt("arm32: __sF at guest 0x%x (stdin/stdout/stderr)", g_a32_sF);
        }
        return g_a32_sF;
    }
    if (!strcmp(name, "__stack_chk_guard")) {
        static uint32_t g = 0;
        if (!g) { g = dataAlloc(4); if (g) *(uint32_t*)toHost(g) = 0xDEADC0DE; }
        return g;
    }
    if (!strcmp(name, "_toupper_tab_")) {
        // Bionic indexes this from -128, so the table pointer sits 128 entries
        // in. Two bytes per entry, EOF plus 256 values either side.
        static uint32_t g = 0;
        if (!g) {
            uint32_t base = dataAlloc(2 * 385);
            if (base) {
                uint16_t* t = (uint16_t*)toHost(base);
                for (int i = 0; i < 385; i++) {
                    int ch = i - 128;
                    t[i] = (uint16_t)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch);
                }
                g = base + 128 * 2;
            }
        }
        return g;
    }
    return 0;   // not a data symbol we know — caller falls back to a sentinel
}

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
// A float argument, per the ABI the NDK builds armeabi-v7a with.
//
// That is softfp: floats travel in the core registers as raw bits, not in the
// VFP bank. So the value is already in r0-r3 (or on the stack) and only needs
// reinterpreting — reading s0-s15 instead would pick up whatever the guest last
// left there, which is the kind of wrong that produces plausible-looking
// geometry rather than an obvious failure.
static inline float fArg(CpuState& c, int i) {
    uint32_t raw = arg(c, i);
    float f; memcpy(&f, &raw, sizeof f); return f;
}
// ── printf for guest code ───────────────────────────────────────────────────
//
// Nothing in the printf family was bridged, which mattered more than the call
// count suggested: libc++abi formats its assertion text through vasprintf
// before aborting, so every failed assert reached abort() with its message
// thrown away. The log recorded that the game had given up but never why.
//
// Arguments come from a guest va_list, which on ARM EABI is a plain pointer
// into the caller's stack: 32-bit values occupy one slot, 64-bit values are
// aligned to 8 first. Rather than reimplement formatting, each conversion is
// re-emitted as a host format string and handed to snprintf with the one
// argument it consumes — so width, precision, flags and `*` all behave
// exactly as the guest expects without a second implementation to keep
// correct.
static uint32_t gStrlen(uint32_t g);

struct GuestVa {
    // Exactly one of these is used. A v-form (vfprintf, vasprintf) is handed a
    // va_list, which is just a guest pointer. A plain variadic call instead
    // spills its arguments across r0-r3 and then the stack, which is what
    // arg() already knows how to walk — so `c` selects that reading.
    CpuState* c = nullptr;
    int       i = 0;                             // argument word index, for `c`
    uint32_t  p = 0;                             // guest va_list, otherwise

    uint32_t u32(void) {
        if (c) return arg(*c, i++);
        if (!guestValid(p, 4)) return 0;
        uint32_t v = *(const uint32_t*)toHost(p); p += 4; return v;
    }
    uint64_t u64(void) {
        if (c) {
            if (i & 1) i++;                      // 64-bit starts on an even word
            uint32_t lo = arg(*c, i++), hi = arg(*c, i++);
            return (uint64_t)lo | ((uint64_t)hi << 32);
        }
        p = (p + 7u) & ~7u;                      // 64-bit values are 8-aligned
        if (!guestValid(p, 8)) return 0;
        uint64_t v = *(const uint64_t*)toHost(p); p += 8; return v;
    }
};

static size_t gvformat(char* out, size_t cap, uint32_t gfmt, GuestVa& va) {
    if (!cap) return 0;
    out[0] = 0;
    if (!gfmt || gfmt >= g_region) return 0;

    const char* f   = (const char*)toHost(gfmt);
    const char* end = f + gStrlen(gfmt);
    size_t      n   = 0;                          // bytes written (excl. NUL)

    auto put = [&](const char* src, size_t len) {
        if (n >= cap - 1) { n += len; return; }   // keep counting for the return
        size_t room = cap - 1 - n;
        size_t take = len < room ? len : room;
        memcpy(out + n, src, take);
        n += len;
        out[n < cap - 1 ? n : cap - 1] = 0;
    };

    while (f < end) {
        if (*f != '%') { put(f, 1); f++; continue; }
        if (f + 1 < end && f[1] == '%') { put("%", 1); f += 2; continue; }

        // Copy the conversion verbatim, resolving any `*` as we go, so the host
        // sees the same spec the guest wrote.
        char spec[64]; size_t sn = 0;
        spec[sn++] = *f++;
        int  star[2]; int stars = 0;
        int  len = 0;                              // 0=int 1=long 2=long long 3=size_t/ptrdiff
        while (f < end && sn < sizeof spec - 8) {
            char ch = *f;
            if (ch == '*') {
                if (stars < 2) star[stars++] = (int)va.u32();
                // splice the resolved number in place of the star
                sn += (size_t)snprintf(spec + sn, sizeof spec - sn, "%d", star[stars - 1]);
                f++;
                continue;
            }
            if (ch == 'l') { len = (len == 1 || len == 2) ? 2 : 1; f++; continue; }  // l / ll
            if (ch == 'h') { f++; continue; }                       // h/hh: still an int slot
            if (ch == 'z' || ch == 't' || ch == 'j') { len = 3; f++; continue; }
            if (ch == 'q' || ch == 'L') { len = 2; f++; continue; }
            spec[sn++] = ch; f++;
            if (strchr("diouxXeEfgGaAcspn", ch)) break;             // conversion ends here
        }
        spec[sn] = 0;
        const char conv = sn ? spec[sn - 1] : 0;

        char tmp[512];
        switch (conv) {
            case 'd': case 'i': {
                if (len >= 2) { char h[64]; snprintf(h, sizeof h, "%.*sll%c", (int)sn - 1, spec, conv);
                                snprintf(tmp, sizeof tmp, h, (long long)(int64_t)va.u64()); }
                else            snprintf(tmp, sizeof tmp, spec, (int)va.u32());
                put(tmp, strlen(tmp)); break;
            }
            case 'u': case 'o': case 'x': case 'X': {
                if (len >= 2) { char h[64]; snprintf(h, sizeof h, "%.*sll%c", (int)sn - 1, spec, conv);
                                snprintf(tmp, sizeof tmp, h, (unsigned long long)va.u64()); }
                else            snprintf(tmp, sizeof tmp, spec, (unsigned)va.u32());
                put(tmp, strlen(tmp)); break;
            }
            case 'c': snprintf(tmp, sizeof tmp, spec, (int)va.u32()); put(tmp, strlen(tmp)); break;
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
                // Variadic floats are promoted to double, so this is always a
                // 64-bit slot even when the guest wrote %f for a float.
                uint64_t bits = va.u64(); double d; memcpy(&d, &bits, sizeof d);
                snprintf(tmp, sizeof tmp, spec, d); put(tmp, strlen(tmp)); break;
            }
            case 's': {
                uint32_t gp = va.u32();
                const char* sv = (gp && gp < g_region) ? (const char*)toHost(gp) : "(null)";
                // Bound the copy: an unterminated guest string must not be
                // walked off the end of the region by the host's snprintf.
                uint32_t slen = gp ? gStrlen(gp) : 6;
                char bounded[512];
                uint32_t take = slen < sizeof bounded - 1 ? slen : sizeof bounded - 1;
                memcpy(bounded, sv, take); bounded[take] = 0;
                snprintf(tmp, sizeof tmp, spec, bounded); put(tmp, strlen(tmp)); break;
            }
            case 'p': {
                snprintf(tmp, sizeof tmp, "0x%08x", va.u32()); put(tmp, strlen(tmp)); break;
            }
            case 'n': { va.u32(); break; }                     // consumed, never written
            default:  put(spec, sn); break;
        }
    }
    out[n < cap - 1 ? n : cap - 1] = 0;
    return n;
}

static inline char*  hstr(uint32_t g) { return g ? (char*)toHost(g) : nullptr; }
// Same, but safe to hand a pointer the guest chose. hstr() only rejects null,
// so anything outside the region — a wild pointer, or a value that was never
// a pointer — resolved to host memory beyond the mapping and read it. Used
// wherever a string comes from the game rather than from us.
static inline const char* gstr(uint32_t g, const char* fallback = "(bad ptr)") {
    return (g && g < g_region) ? (const char*)toHost(g) : fallback;
}
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

// ─── OpenGL ES 2.0 ──────────────────────────────────────────────────────────
// The interpreter had no GL at all, which is why bring-up stopped after
// JNI_OnLoad: the constructors ran and there was nothing for the game to draw
// through. cocos2d-x calls GL directly from guest code, so every entry point it
// imports has to be forwarded.
//
// Most of these take scalars and forward unchanged. The ones that matter are
// the pointer arguments, which are guest addresses and have to be translated —
// a guest pointer handed straight to the driver is an address in the wrong
// process's terms and would either fault or, worse, read whatever happens to
// live at that host address.
//
// GLA(n) reads argument n as a scalar; GLP(n) translates it as a pointer,
// mapping guest 0 to a real null rather than to the base of the guest region.
#define GLA(n) arg(c, n)
#define GLP(n) hptr(arg(c, n))

static bool dispatchGL(CpuState& c, const char* name, uint32_t& ret) {
    ret = 0;
    // ── state ────────────────────────────────────────────────────────────────
    if (!strcmp(name,"glClear"))          { glClear(GLA(0)); return true; }
    if (!strcmp(name,"glClearColor"))     { glClearColor(fArg(c,0),fArg(c,1),fArg(c,2),fArg(c,3)); return true; }
    if (!strcmp(name,"glClearDepthf"))    { glClearDepthf(fArg(c,0)); return true; }
    if (!strcmp(name,"glClearStencil"))   { glClearStencil((GLint)GLA(0)); return true; }
    if (!strcmp(name,"glViewport"))       { glViewport(GLA(0),GLA(1),GLA(2),GLA(3)); return true; }
    if (!strcmp(name,"glScissor"))        { glScissor(GLA(0),GLA(1),GLA(2),GLA(3)); return true; }
    if (!strcmp(name,"glEnable"))         { glEnable(GLA(0)); return true; }
    if (!strcmp(name,"glDisable"))        { glDisable(GLA(0)); return true; }
    if (!strcmp(name,"glIsEnabled"))      { ret = glIsEnabled(GLA(0)); return true; }
    if (!strcmp(name,"glBlendFunc"))      { glBlendFunc(GLA(0),GLA(1)); return true; }
    if (!strcmp(name,"glBlendFuncSeparate")) { glBlendFuncSeparate(GLA(0),GLA(1),GLA(2),GLA(3)); return true; }
    if (!strcmp(name,"glBlendEquation"))  { glBlendEquation(GLA(0)); return true; }
    if (!strcmp(name,"glBlendEquationSeparate")) { glBlendEquationSeparate(GLA(0),GLA(1)); return true; }
    if (!strcmp(name,"glDepthFunc"))      { glDepthFunc(GLA(0)); return true; }
    if (!strcmp(name,"glDepthMask"))      { glDepthMask((GLboolean)GLA(0)); return true; }
    if (!strcmp(name,"glColorMask"))      { glColorMask(GLA(0),GLA(1),GLA(2),GLA(3)); return true; }
    if (!strcmp(name,"glCullFace"))       { glCullFace(GLA(0)); return true; }
    if (!strcmp(name,"glFrontFace"))      { glFrontFace(GLA(0)); return true; }
    if (!strcmp(name,"glLineWidth"))      { glLineWidth(fArg(c,0)); return true; }
    if (!strcmp(name,"glPolygonOffset"))  { glPolygonOffset(fArg(c,0),fArg(c,1)); return true; }
    if (!strcmp(name,"glPixelStorei"))    { glPixelStorei(GLA(0),(GLint)GLA(1)); return true; }
    if (!strcmp(name,"glStencilFunc"))    { glStencilFunc(GLA(0),GLA(1),GLA(2)); return true; }
    if (!strcmp(name,"glStencilFuncSeparate")) { glStencilFuncSeparate(GLA(0),GLA(1),GLA(2),GLA(3)); return true; }
    if (!strcmp(name,"glStencilMask"))    { glStencilMask(GLA(0)); return true; }
    if (!strcmp(name,"glStencilOp"))      { glStencilOp(GLA(0),GLA(1),GLA(2)); return true; }
    if (!strcmp(name,"glStencilOpSeparate")) { glStencilOpSeparate(GLA(0),GLA(1),GLA(2),GLA(3)); return true; }
    if (!strcmp(name,"glGetError"))       { ret = glGetError(); return true; }
    if (!strcmp(name,"glFinish"))         { glFinish(); return true; }
    if (!strcmp(name,"glFlush"))          { glFlush(); return true; }

    // ── textures ─────────────────────────────────────────────────────────────
    if (!strcmp(name,"glGenTextures"))    { glGenTextures(GLA(0),(GLuint*)GLP(1)); return true; }
    if (!strcmp(name,"glDeleteTextures")) { glDeleteTextures(GLA(0),(GLuint*)GLP(1)); return true; }
    if (!strcmp(name,"glBindTexture"))    { glBindTexture(GLA(0),GLA(1)); return true; }
    if (!strcmp(name,"glActiveTexture"))  { glActiveTexture(GLA(0)); return true; }
    if (!strcmp(name,"glTexParameteri"))  { glTexParameteri(GLA(0),GLA(1),(GLint)GLA(2)); return true; }
    if (!strcmp(name,"glTexImage2D"))     { glTexImage2D(GLA(0),GLA(1),GLA(2),GLA(3),GLA(4),GLA(5),GLA(6),GLA(7),GLP(8)); return true; }
    if (!strcmp(name,"glTexSubImage2D"))  { glTexSubImage2D(GLA(0),GLA(1),GLA(2),GLA(3),GLA(4),GLA(5),GLA(6),GLA(7),GLP(8)); return true; }
    if (!strcmp(name,"glCompressedTexImage2D")) { glCompressedTexImage2D(GLA(0),GLA(1),GLA(2),GLA(3),GLA(4),GLA(5),GLA(6),GLP(7)); return true; }
    if (!strcmp(name,"glCopyTexSubImage2D")) { glCopyTexSubImage2D(GLA(0),GLA(1),GLA(2),GLA(3),GLA(4),GLA(5),GLA(6),GLA(7)); return true; }
    if (!strcmp(name,"glGenerateMipmap")) { glGenerateMipmap(GLA(0)); return true; }
    if (!strcmp(name,"glReadPixels"))     { glReadPixels(GLA(0),GLA(1),GLA(2),GLA(3),GLA(4),GLA(5),GLP(6)); return true; }

    // ── shaders and programs ─────────────────────────────────────────────────
    if (!strcmp(name,"glCreateShader"))   { ret = glCreateShader(GLA(0)); return true; }
    if (!strcmp(name,"glDeleteShader"))   { glDeleteShader(GLA(0)); return true; }
    if (!strcmp(name,"glCompileShader"))  { glCompileShader(GLA(0)); return true; }
    if (!strcmp(name,"glCreateProgram"))  { ret = glCreateProgram(); return true; }
    if (!strcmp(name,"glDeleteProgram"))  { glDeleteProgram(GLA(0)); return true; }
    if (!strcmp(name,"glAttachShader"))   { glAttachShader(GLA(0),GLA(1)); return true; }
    if (!strcmp(name,"glLinkProgram"))    { glLinkProgram(GLA(0)); return true; }
    if (!strcmp(name,"glUseProgram"))     { glUseProgram(GLA(0)); return true; }
    if (!strcmp(name,"glGetShaderiv"))    { glGetShaderiv(GLA(0),GLA(1),(GLint*)GLP(2)); return true; }
    if (!strcmp(name,"glGetProgramiv"))   { glGetProgramiv(GLA(0),GLA(1),(GLint*)GLP(2)); return true; }
    if (!strcmp(name,"glGetShaderInfoLog")) { glGetShaderInfoLog(GLA(0),GLA(1),(GLsizei*)GLP(2),(GLchar*)GLP(3)); return true; }
    if (!strcmp(name,"glGetProgramInfoLog")){ glGetProgramInfoLog(GLA(0),GLA(1),(GLsizei*)GLP(2),(GLchar*)GLP(3)); return true; }
    if (!strcmp(name,"glBindAttribLocation")) { glBindAttribLocation(GLA(0),GLA(1),(const GLchar*)GLP(2)); return true; }
    if (!strcmp(name,"glGetAttribLocation"))  { ret = (uint32_t)glGetAttribLocation(GLA(0),(const GLchar*)GLP(1)); return true; }
    if (!strcmp(name,"glGetUniformLocation")) { ret = (uint32_t)glGetUniformLocation(GLA(0),(const GLchar*)GLP(1)); return true; }

    // glShaderSource takes an array of guest string pointers, so the array
    // itself has to be rebuilt with host addresses before the driver sees it.
    if (!strcmp(name,"glShaderSource")) {
        GLuint sh = GLA(0); GLsizei n = (GLsizei)GLA(1);
        uint32_t* gstrs = (uint32_t*)GLP(2);
        uint32_t* glens = (uint32_t*)GLP(3);
        if (!gstrs || n <= 0) return true;
        std::vector<const GLchar*> hs((size_t)n);
        std::vector<GLint>         hl((size_t)n);
        for (GLsizei i = 0; i < n; i++) {
            hs[i] = (const GLchar*)hptr(gstrs[i]);
            hl[i] = glens ? (GLint)glens[i] : -1;
        }
        glShaderSource(sh, n, hs.data(), glens ? hl.data() : nullptr);
        return true;
    }

    // ── uniforms ─────────────────────────────────────────────────────────────
    if (!strcmp(name,"glUniform1i"))  { glUniform1i((GLint)GLA(0),(GLint)GLA(1)); return true; }
    if (!strcmp(name,"glUniform1f"))  { glUniform1f((GLint)GLA(0),fArg(c,1)); return true; }
    if (!strcmp(name,"glUniform2f"))  { glUniform2f((GLint)GLA(0),fArg(c,1),fArg(c,2)); return true; }
    if (!strcmp(name,"glUniform3f"))  { glUniform3f((GLint)GLA(0),fArg(c,1),fArg(c,2),fArg(c,3)); return true; }
    if (!strcmp(name,"glUniform4f"))  { glUniform4f((GLint)GLA(0),fArg(c,1),fArg(c,2),fArg(c,3),fArg(c,4)); return true; }
    if (!strcmp(name,"glUniform1iv")) { glUniform1iv((GLint)GLA(0),GLA(1),(const GLint*)GLP(2)); return true; }
    if (!strcmp(name,"glUniform1fv")) { glUniform1fv((GLint)GLA(0),GLA(1),(const GLfloat*)GLP(2)); return true; }
    if (!strcmp(name,"glUniform2fv")) { glUniform2fv((GLint)GLA(0),GLA(1),(const GLfloat*)GLP(2)); return true; }
    if (!strcmp(name,"glUniform3fv")) { glUniform3fv((GLint)GLA(0),GLA(1),(const GLfloat*)GLP(2)); return true; }
    if (!strcmp(name,"glUniform4fv")) { glUniform4fv((GLint)GLA(0),GLA(1),(const GLfloat*)GLP(2)); return true; }
    if (!strcmp(name,"glUniformMatrix3fv")) { glUniformMatrix3fv((GLint)GLA(0),GLA(1),(GLboolean)GLA(2),(const GLfloat*)GLP(3)); return true; }
    if (!strcmp(name,"glUniformMatrix4fv")) { glUniformMatrix4fv((GLint)GLA(0),GLA(1),(GLboolean)GLA(2),(const GLfloat*)GLP(3)); return true; }

    // ── vertex state and drawing ─────────────────────────────────────────────
    if (!strcmp(name,"glEnableVertexAttribArray"))  { glEnableVertexAttribArray(GLA(0)); return true; }
    if (!strcmp(name,"glDisableVertexAttribArray")) { glDisableVertexAttribArray(GLA(0)); return true; }
    if (!strcmp(name,"glVertexAttribPointer")) {
        // The last argument is either a client-memory pointer or a byte offset
        // into the bound buffer, told apart by whether a buffer is bound. Only
        // the first is a guest address; translating the second would turn an
        // offset into a wild pointer.
        GLint bound = 0;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound);
        const void* p = bound ? (const void*)(uintptr_t)GLA(5) : (const void*)GLP(5);
        glVertexAttribPointer(GLA(0),GLA(1),GLA(2),(GLboolean)GLA(3),GLA(4),p);
        return true;
    }
    if (!strcmp(name,"glDrawArrays"))   { glDrawArrays(GLA(0),GLA(1),GLA(2)); return true; }
    if (!strcmp(name,"glDrawElements")) {
        GLint bound = 0;
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &bound);
        const void* p = bound ? (const void*)(uintptr_t)GLA(3) : (const void*)GLP(3);
        glDrawElements(GLA(0),GLA(1),GLA(2),p);
        return true;
    }

    // ── buffers and framebuffers ─────────────────────────────────────────────
    if (!strcmp(name,"glGenBuffers"))     { glGenBuffers(GLA(0),(GLuint*)GLP(1)); return true; }
    if (!strcmp(name,"glDeleteBuffers"))  { glDeleteBuffers(GLA(0),(GLuint*)GLP(1)); return true; }
    if (!strcmp(name,"glBindBuffer"))     { glBindBuffer(GLA(0),GLA(1)); return true; }
    if (!strcmp(name,"glBufferData"))     { glBufferData(GLA(0),GLA(1),GLP(2),GLA(3)); return true; }
    if (!strcmp(name,"glBufferSubData"))  { glBufferSubData(GLA(0),GLA(1),GLA(2),GLP(3)); return true; }
    if (!strcmp(name,"glIsBuffer"))       { ret = glIsBuffer(GLA(0)); return true; }
    if (!strcmp(name,"glGenFramebuffers"))    { glGenFramebuffers(GLA(0),(GLuint*)GLP(1)); return true; }
    if (!strcmp(name,"glDeleteFramebuffers")) { glDeleteFramebuffers(GLA(0),(GLuint*)GLP(1)); return true; }
    if (!strcmp(name,"glBindFramebuffer"))    { glBindFramebuffer(GLA(0),GLA(1)); return true; }
    if (!strcmp(name,"glFramebufferTexture2D")){ glFramebufferTexture2D(GLA(0),GLA(1),GLA(2),GLA(3),GLA(4)); return true; }
    if (!strcmp(name,"glCheckFramebufferStatus")) { ret = glCheckFramebufferStatus(GLA(0)); return true; }
    if (!strcmp(name,"glGenRenderbuffers"))    { glGenRenderbuffers(GLA(0),(GLuint*)GLP(1)); return true; }
    if (!strcmp(name,"glDeleteRenderbuffers")) { glDeleteRenderbuffers(GLA(0),(GLuint*)GLP(1)); return true; }
    if (!strcmp(name,"glBindRenderbuffer"))    { glBindRenderbuffer(GLA(0),GLA(1)); return true; }
    if (!strcmp(name,"glRenderbufferStorage")) { glRenderbufferStorage(GLA(0),GLA(1),GLA(2),GLA(3)); return true; }
    if (!strcmp(name,"glFramebufferRenderbuffer")) { glFramebufferRenderbuffer(GLA(0),GLA(1),GLA(2),GLA(3)); return true; }
    if (!strcmp(name,"glIsRenderbuffer"))      { ret = glIsRenderbuffer(GLA(0)); return true; }

    // ── queries ──────────────────────────────────────────────────────────────
    if (!strcmp(name,"glGetIntegerv"))  { glGetIntegerv(GLA(0),(GLint*)GLP(1)); return true; }
    if (!strcmp(name,"glGetFloatv"))    { glGetFloatv(GLA(0),(GLfloat*)GLP(1)); return true; }
    if (!strcmp(name,"glGetBooleanv"))  { glGetBooleanv(GLA(0),(GLboolean*)GLP(1)); return true; }
    if (!strcmp(name,"glGetShaderPrecisionFormat")) { glGetShaderPrecisionFormat(GLA(0),GLA(1),(GLint*)GLP(2),(GLint*)GLP(3)); return true; }
    if (!strcmp(name,"glGetActiveAttrib"))  { glGetActiveAttrib(GLA(0),GLA(1),GLA(2),(GLsizei*)GLP(3),(GLint*)GLP(4),(GLenum*)GLP(5),(GLchar*)GLP(6)); return true; }
    if (!strcmp(name,"glGetActiveUniform")) { glGetActiveUniform(GLA(0),GLA(1),GLA(2),(GLsizei*)GLP(3),(GLint*)GLP(4),(GLenum*)GLP(5),(GLchar*)GLP(6)); return true; }

    // glGetString returns a host pointer the guest cannot dereference, so the
    // string is copied into guest memory and cached — the game holds onto these
    // (cocos2d-x parses GL_EXTENSIONS at startup), so a fresh copy per call
    // would leak steadily.
    if (!strcmp(name,"glGetString")) {
        static std::vector<std::pair<uint32_t,uint32_t>> cache;   // name -> guest
        uint32_t which = GLA(0);
        for (auto& e : cache) if (e.first == which) { ret = e.second; return true; }
        const char* v = (const char*)glGetString(which);
        if (!v) { ret = 0; return true; }
        uint32_t n = (uint32_t)strlen(v) + 1;
        uint32_t g = guestAlloc(n);
        if (g) { memcpy(toHost(g), v, n); cache.push_back({which, g}); }
        ret = g;
        return true;
    }
    return false;
}
#undef GLA
#undef GLP


// ─── Batch: everything the game reaches after its constructors ──────────────
// The log named three unbridged imports — fmaxf, strtoll and __stack_chk_fail
// — and an unbridged import returns 0. strtoll answering 0 for every parse and
// fmaxf answering 0 for every comparison is not a stall, it is the game
// computing with wrong numbers, which is how a run ends up executing data.
//
// Discovering these one launch at a time would take as many builds as there
// are functions, so this covers what a cocos2d-x title predictably reaches
// next: the float maths, the string parsing, the fortified variants and the
// formatted output. Each is small; the reason they were missing is that the
// bridge was written far enough to load a game, not to run one.
//
// Floats arrive in core registers (softfp) and are returned the same way, so
// RETF packs the result back into r0 rather than leaving it in the VFP bank.
#define A(n)   arg(c, n)
#define F(n)   fArg(c, n)
#define RETF(x) do { float _v = (x); memcpy(&ret, &_v, 4); return true; } while (0)

static bool dispatchMore(CpuState& c, const char* name, uint32_t& ret) {
    ret = 0;
    // ── float maths ──────────────────────────────────────────────────────────
    if (!strcmp(name,"fmaxf"))  RETF(fmaxf(F(0), F(1)));
    if (!strcmp(name,"fminf"))  RETF(fminf(F(0), F(1)));
    if (!strcmp(name,"acosf"))  RETF(acosf(F(0)));
    if (!strcmp(name,"asinf"))  RETF(asinf(F(0)));
    if (!strcmp(name,"atanf"))  RETF(atanf(F(0)));
    if (!strcmp(name,"expf"))   RETF(expf(F(0)));
    if (!strcmp(name,"exp2f"))  RETF(exp2f(F(0)));
    if (!strcmp(name,"fmodf"))  RETF(fmodf(F(0), F(1)));
    if (!strcmp(name,"roundf")) RETF(roundf(F(0)));
    if (!strcmp(name,"tanf"))   RETF(tanf(F(0)));
    if (!strcmp(name,"sinhf"))  RETF(sinhf(F(0)));
    if (!strcmp(name,"coshf"))  RETF(coshf(F(0)));
    if (!strcmp(name,"tanhf"))  RETF(tanhf(F(0)));
    if (!strcmp(name,"lroundf")) { ret = (uint32_t)(int32_t)lroundf(F(0)); return true; }
    if (!strcmp(name,"sincosf")) {
        float sn, cs; sn = sinf(F(0)); cs = cosf(F(0));
        if (A(1)) memcpy(toHost(A(1)), &sn, 4);
        if (A(2)) memcpy(toHost(A(2)), &cs, 4);
        return true;
    }

    // ── string parsing ───────────────────────────────────────────────────────
    if (!strcmp(name,"strtoll") || !strcmp(name,"strtoull")) {
        // Returns 64 bits, which under AAPCS come back in r0:r1.
        char* end = nullptr;
        unsigned long long v = strtoull(hstr(A(0)), &end, (int)A(2));
        if (A(1) && end) *(uint32_t*)toHost(A(1)) = (uint32_t)(A(0) + (end - hstr(A(0))));
        c.r[1] = (uint32_t)(v >> 32);
        ret    = (uint32_t)v;
        return true;
    }
    if (!strcmp(name,"strtod")) {
        char* end = nullptr;
        double v = strtod(hstr(A(0)), &end);
        if (A(1) && end) *(uint32_t*)toHost(A(1)) = (uint32_t)(A(0) + (end - hstr(A(0))));
        uint64_t bits; memcpy(&bits, &v, 8);
        c.r[1] = (uint32_t)(bits >> 32);
        ret    = (uint32_t)bits;
        return true;
    }
    if (!strcmp(name,"atoll")) {
        long long v = atoll(hstr(A(0)));
        c.r[1] = (uint32_t)((unsigned long long)v >> 32);
        ret    = (uint32_t)v;
        return true;
    }
    if (!strcmp(name,"strcoll")) { ret = (uint32_t)strcmp(hstr(A(0)), hstr(A(1))); return true; }
    if (!strcmp(name,"strpbrk")) {
        char* r = strpbrk(hstr(A(0)), hstr(A(1)));
        ret = r ? (uint32_t)(A(0) + (r - hstr(A(0)))) : 0;
        return true;
    }
    if (!strcmp(name,"strerror")) {
        // Returns a pointer the guest must be able to read, so the text is
        // copied into guest memory and kept — callers treat it as static.
        static uint32_t g_err = 0;
        if (!g_err) g_err = guestAlloc(128);
        if (g_err) snprintf((char*)toHost(g_err), 128, "%s", strerror((int)A(0)));
        ret = g_err;
        return true;
    }

    // ── fortified variants ───────────────────────────────────────────────────
    // The trailing argument is the destination size, and honouring it is the
    // whole point of these — forwarding and dropping it would leave the game
    // with no bounds checking exactly where it asked for some.
    if (!strcmp(name,"__memcpy_chk"))  { uint32_t n=A(2), d=A(3); if (n>d) n=d; memcpy(toHost(A(0)), toHost(A(1)), n); ret=A(0); return true; }
    if (!strcmp(name,"__memmove_chk")) { uint32_t n=A(2), d=A(3); if (n>d) n=d; memmove(toHost(A(0)),toHost(A(1)),n); ret=A(0); return true; }
    if (!strcmp(name,"__strcpy_chk")) {
        // strncpy does not terminate when the source fills the buffer, so the
        // guest would be handed an unterminated string — the next strlen walks
        // off the end and the fault surfaces somewhere unrelated.
        char* d = hstr(A(0)); const char* sp = hstr(A(1)); uint32_t cap = A(2);
        if (d && sp && cap) { strncpy(d, sp, cap); d[cap - 1] = 0; }
        ret = A(0);
        return true;
    }
    if (!strcmp(name,"__strcat_chk")) {
        // strncat's third argument is how many characters to append, not the
        // size of the destination — passing the destination size lets it write
        // strlen(dst)+n+1 bytes, which is precisely the overflow this call
        // exists to prevent. Budget from what is left instead.
        char* d = hstr(A(0)); const char* sp = hstr(A(1)); uint32_t cap = A(2);
        if (d && sp && cap) {
            uint32_t dl = 0; while (dl < cap && d[dl]) dl++;
            if (dl + 1 < cap) strncat(d, sp, cap - dl - 1);
        }
        ret = A(0);
        return true;
    }
    if (!strcmp(name,"__strlen_chk"))  { { const char* p0=hstr(A(0)); uint32_t l=0; while (p0 && l<A(1) && p0[l]) l++; ret=l; } return true; }

    // ── stack protector ──────────────────────────────────────────────────────
    // Reaching this means a canary check failed, which is a real problem in the
    // guest and not something to paper over — say so loudly and stop this call
    // rather than returning 0 and letting it continue on a smashed stack.
    if (!strcmp(name,"__stack_chk_fail")) {
        // Two different faults produce this, and they need opposite fixes: the
        // guard variable itself being wrong (our storage, so our bug) versus
        // the copy on the guest's stack being overwritten (the guest scribbling,
        // or an instruction that moves SP wrongly). Print the guard so the log
        // says which — "still 0xDEADC0DE" means the stack copy was clobbered.
        uint32_t gaddr = bridgeRegisterData("__stack_chk_guard");
        uint32_t gval  = gaddr ? *(uint32_t*)toHost(gaddr) : 0;
        compatLogFmt("arm32: __stack_chk_fail — guard@0x%x is 0x%08x (%s), sp=0x%x",
                     gaddr, gval,
                     gval == 0xDEADC0DE ? "intact, so the stack copy was overwritten"
                                        : "CHANGED — the guard variable itself was written",
                     c.r[13]);
        cpuDumpBranches();
        c.halt = true; c.halt_pc = c.r[15];
        return true;
    }
    if (!strcmp(name,"__assert2")) {
        compatLogFmt("arm32: assert %s:%u %s — %s", gstr(A(0), "?"), A(1),
                     gstr(A(2), "?"), gstr(A(3), "(no message)"));
        c.halt = true; c.halt_pc = c.r[15];
        return true;
    }
    if (!strcmp(name,"__errno")) {
        static uint32_t g_errno = 0;
        if (!g_errno) g_errno = guestAlloc(4);
        if (g_errno) *(uint32_t*)toHost(g_errno) = (uint32_t)errno;
        ret = g_errno;
        return true;
    }

    // ── ctype / wide ─────────────────────────────────────────────────────────
    if (!strcmp(name,"towlower"))  { ret = (uint32_t)tolower((int)A(0)); return true; }
    if (!strcmp(name,"towupper"))  { ret = (uint32_t)toupper((int)A(0)); return true; }
    if (!strcmp(name,"iswalpha"))  { ret = isalpha((int)A(0))?1:0; return true; }
    if (!strcmp(name,"iswdigit"))  { ret = isdigit((int)A(0))?1:0; return true; }
    if (!strcmp(name,"iswspace"))  { ret = isspace((int)A(0))?1:0; return true; }
    if (!strcmp(name,"iswupper"))  { ret = isupper((int)A(0))?1:0; return true; }
    if (!strcmp(name,"iswlower"))  { ret = islower((int)A(0))?1:0; return true; }
    if (!strcmp(name,"iswpunct"))  { ret = ispunct((int)A(0))?1:0; return true; }
    if (!strcmp(name,"iswcntrl"))  { ret = iscntrl((int)A(0))?1:0; return true; }
    if (!strcmp(name,"iswprint"))  { ret = isprint((int)A(0))?1:0; return true; }
    if (!strcmp(name,"iswxdigit")) { ret = isxdigit((int)A(0))?1:0; return true; }
    if (!strcmp(name,"btowc"))     { ret = A(0); return true; }
    if (!strcmp(name,"wctob"))     { ret = (A(0) < 256) ? A(0) : (uint32_t)-1; return true; }

    // ── misc ─────────────────────────────────────────────────────────────────
    if (!strcmp(name,"sched_yield")) { svcSleepThread(0); return true; }
    if (!strcmp(name,"getcwd"))      { if (A(0) && A(1)) { snprintf(hstr(A(0)), A(1), "/"); ret = A(0); } return true; }
    if (!strcmp(name,"remove"))      { ret = (uint32_t)remove(hstr(A(0))); return true; }
    if (!strcmp(name,"rename"))      { ret = (uint32_t)rename(hstr(A(0)), hstr(A(1))); return true; }
    if (!strcmp(name,"mkdir"))       { ret = (uint32_t)mkdir(hstr(A(0)), 0777); return true; }
    if (!strcmp(name,"glHint"))              { glHint(A(0), A(1)); return true; }
    if (!strcmp(name,"glIsProgram"))         { ret = glIsProgram(A(0)); return true; }
    if (!strcmp(name,"glValidateProgram"))   { glValidateProgram(A(0)); return true; }
    if (!strcmp(name,"glUniformMatrix2fv"))  { glUniformMatrix2fv((GLint)A(0),A(1),(GLboolean)A(2),(const GLfloat*)hptr(A(3))); return true; }
    return false;
}
#undef A
#undef F
#undef RETF

static bool dispatch(CpuState& c, const char* name, uint32_t& ret) {
    // GL first: it is the hottest path once the game is drawing.
    if (name[0]=='g' && name[1]=='l' && dispatchGL(c, name, ret)) return true;
    if (dispatchMore(c, name, ret)) return true;

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
    if (!strcmp(name, "memcpy") || !strcmp(name, "memmove") ||
        !strcmp(name, "__aeabi_memcpy")  || !strcmp(name, "__aeabi_memcpy4") ||
        !strcmp(name, "__aeabi_memcpy8") || !strcmp(name, "__aeabi_memmove") ||
        !strcmp(name, "__aeabi_memmove4")|| !strcmp(name, "__aeabi_memmove8") ||
        !strcmp(name, "mempcpy")) {
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

    // ── more string / mem (all bounded to the region) ──
    if (!strcmp(name,"strnlen")) { uint32_t L=gStrlen(arg(c,0)), n=arg(c,1); ret = L<n?L:n; return true; }
    if (!strcmp(name,"strrchr")) { uint32_t s=arg(c,0),L=gStrlen(s),f=0; uint8_t ch=(uint8_t)arg(c,1);
        for(uint32_t i=0;i<L;i++) if(((uint8_t*)hptr(s))[i]==ch) f=s+i; ret=f; return true; }
    if (!strcmp(name,"memchr")) { uint32_t s=arg(c,0),n=arg(c,2); void* p=(s&&guestValid(s,n))?memchr(hptr(s),(int)arg(c,1),n):nullptr; ret=p?toGuest(p):0; return true; }
    if (!strcmp(name,"strstr")) { uint32_t h=arg(c,0),ne=arg(c,1); if(!h||!ne){ret=0;return true;} gStrlen(h); gStrlen(ne); char* p=strstr(hstr(h),hstr(ne)); ret=p?toGuest(p):0; return true; }
    if (!strcmp(name,"strcasecmp")||!strcmp(name,"strncasecmp")) {
        uint32_t a=arg(c,0),b=arg(c,1); uint32_t n=name[6]=='n'?arg(c,2):(gStrlen(a)+1);
        ret=(!a||!b)?(a==b?0:1):(uint32_t)strncasecmp(hstr(a),hstr(b),n); return true; }
    if (!strcmp(name,"strdup")) { uint32_t s=arg(c,0),L=gStrlen(s),p=guestAlloc(L+1);
        if(p){ if(L) memcpy(hptr(p),hptr(s),L); ((char*)hptr(p))[L]=0; } ret=p; return true; }
    if (!strcmp(name,"strncat")) { uint32_t d=arg(c,0),s=arg(c,1),n=arg(c,2),sl=gStrlen(s),dl=gStrlen(d);
        uint32_t cp=sl<n?sl:n; gStrcpy(d+dl,s,cp); if(guestValid(d+dl+cp,1)) ((char*)hptr(d+dl+cp))[0]=0; ret=d; return true; }

    // ── ctype / conversion ──
    if (!strcmp(name,"toupper")) { ret=(uint32_t)toupper((int)arg(c,0)); return true; }
    if (!strcmp(name,"tolower")) { ret=(uint32_t)tolower((int)arg(c,0)); return true; }
    if (!strcmp(name,"isalpha")) { ret=isalpha((int)arg(c,0))?1:0; return true; }
    if (!strcmp(name,"isdigit")) { ret=isdigit((int)arg(c,0))?1:0; return true; }
    if (!strcmp(name,"isspace")) { ret=isspace((int)arg(c,0))?1:0; return true; }
    if (!strcmp(name,"atoi"))    { { char* s=hstr(arg(c,0)); ret=(uint32_t)atoi(s?s:""); } return true; }
    if (!strcmp(name,"strtol")||!strcmp(name,"strtoul")) {
        char* s=hstr(arg(c,0)); ret = s?(uint32_t)strtoul(s,nullptr,(int)arg(c,2)):0; return true; }

    // ── math: softfp ABI — double args in r0:r1 (and r2:r3), returned in r0:r1 ──
    {
        auto argD=[&](int r)->double{ uint64_t b=(uint64_t)arg(c,r)|((uint64_t)arg(c,r+1)<<32); double d; memcpy(&d,&b,8); return d; };
        auto retD=[&](double d){ uint64_t b; memcpy(&b,&d,8); ret=(uint32_t)b; c.r[1]=(uint32_t)(b>>32); };
        auto argF=[&](int r)->float{ uint32_t u=arg(c,r); float f; memcpy(&f,&u,4); return f; };
        auto retF=[&](float f){ uint32_t u; memcpy(&u,&f,4); ret=u; };
        double x=argD(0);
        if (!strcmp(name,"sqrt")) { retD(sqrt(x)); return true; }
        if (!strcmp(name,"sin"))  { retD(sin(x)); return true; }
        if (!strcmp(name,"cos"))  { retD(cos(x)); return true; }
        if (!strcmp(name,"tan"))  { retD(tan(x)); return true; }
        if (!strcmp(name,"asin")) { retD(asin(x)); return true; }
        if (!strcmp(name,"acos")) { retD(acos(x)); return true; }
        if (!strcmp(name,"atan")) { retD(atan(x)); return true; }
        if (!strcmp(name,"exp"))  { retD(exp(x)); return true; }
        if (!strcmp(name,"log"))  { retD(log(x)); return true; }
        if (!strcmp(name,"log10")){ retD(log10(x)); return true; }
        if (!strcmp(name,"floor")){ retD(floor(x)); return true; }
        if (!strcmp(name,"ceil")) { retD(ceil(x)); return true; }
        if (!strcmp(name,"round")){ retD(round(x)); return true; }
        if (!strcmp(name,"fabs")) { retD(fabs(x)); return true; }
        if (!strcmp(name,"atan2")){ retD(atan2(x, argD(2))); return true; }
        if (!strcmp(name,"pow"))  { retD(pow(x, argD(2))); return true; }
        if (!strcmp(name,"fmod")) { retD(fmod(x, argD(2))); return true; }
        if (!strcmp(name,"hypot")){ retD(hypot(x, argD(2))); return true; }
        // float variants (arg/ret in r0)
        float xf=argF(0);
        if (!strcmp(name,"sqrtf")){ retF(sqrtf(xf)); return true; }
        if (!strcmp(name,"sinf")) { retF(sinf(xf)); return true; }
        if (!strcmp(name,"cosf")) { retF(cosf(xf)); return true; }
        if (!strcmp(name,"fabsf")){ retF(fabsf(xf)); return true; }
        if (!strcmp(name,"floorf")){ retF(floorf(xf)); return true; }
        if (!strcmp(name,"ceilf")){ retF(ceilf(xf)); return true; }
        if (!strcmp(name,"powf")) { retF(powf(xf, argF(1))); return true; }
        if (!strcmp(name,"atan2f")){ retF(atan2f(xf, argF(1))); return true; }
    }

    // ── printf family ──
    //
    // Everything funnels through one formatter; the only differences are where
    // the arguments come from and where the result goes. Guest-visible output
    // goes to the log rather than a real stdout, which is the only place a
    // Switch can show it.
    {
        static char fbuf[4096];
        const bool is_v = name[0] == 'v' || (name[0] == '_' && strstr(name, "_vprint"));

        auto fmtInto = [&](uint32_t gfmt, GuestVa& va) -> int {
            return (int)gvformat(fbuf, sizeof fbuf, gfmt, va);
        };

        if (!strcmp(name,"printf") || !strcmp(name,"vprintf")) {
            GuestVa va; if (is_v) va.p = arg(c, 1); else { va.c = &c; va.i = 1; }
            int n = fmtInto(arg(c, 0), va);
            compatLogFmt("game: %s", fbuf);
            ret = (uint32_t)n; return true;
        }
        if (!strcmp(name,"fprintf") || !strcmp(name,"vfprintf")) {
            GuestVa va; if (is_v) va.p = arg(c, 2); else { va.c = &c; va.i = 2; }
            int n = fmtInto(arg(c, 1), va);
            FILE* f = gfile(arg(c, 0));
            if (f == stdout || f == stderr || !f) compatLogFmt("game: %s", fbuf);
            else fwrite(fbuf, 1, (size_t)n < sizeof fbuf ? (size_t)n : sizeof fbuf - 1, f);
            ret = (uint32_t)n; return true;
        }
        if (!strcmp(name,"sprintf") || !strcmp(name,"vsprintf")) {
            GuestVa va; if (is_v) va.p = arg(c, 2); else { va.c = &c; va.i = 2; }
            int n = fmtInto(arg(c, 1), va);
            uint32_t d = arg(c, 0);
            if (d && guestValid(d, (uint32_t)n + 1)) memcpy(toHost(d), fbuf, (size_t)n + 1);
            ret = (uint32_t)n; return true;
        }
        if (!strcmp(name,"snprintf") || !strcmp(name,"vsnprintf")) {
            GuestVa va; if (is_v) va.p = arg(c, 3); else { va.c = &c; va.i = 3; }
            int n = fmtInto(arg(c, 2), va);
            uint32_t d = arg(c, 0), cap = arg(c, 1);
            if (d && cap) {
                uint32_t take = ((uint32_t)n < cap - 1) ? (uint32_t)n : cap - 1;
                if (guestValid(d, take + 1)) { memcpy(toHost(d), fbuf, take); *((char*)toHost(d) + take) = 0; }
            }
            ret = (uint32_t)n;                      // the length it *would* have been
            return true;
        }
        if (!strcmp(name,"asprintf") || !strcmp(name,"vasprintf")) {
            GuestVa va; if (is_v) va.p = arg(c, 2); else { va.c = &c; va.i = 2; }
            int n = fmtInto(arg(c, 1), va);
            uint32_t buf = guestAlloc((uint32_t)n + 1);
            if (buf) memcpy(toHost(buf), fbuf, (size_t)n + 1);
            if (arg(c, 0) && guestValid(arg(c, 0), 4)) *(uint32_t*)toHost(arg(c, 0)) = buf;
            ret = buf ? (uint32_t)n : (uint32_t)-1;
            return true;
        }
        if (!strcmp(name,"__android_log_print") || !strcmp(name,"__android_log_vprint")) {
            GuestVa va; if (is_v) va.p = arg(c, 3); else { va.c = &c; va.i = 3; }
            int n = fmtInto(arg(c, 2), va);
            compatLogFmt("game[%s]: %s", gstr(arg(c, 1), "?"), fbuf);
            ret = (uint32_t)n; return true;
        }
        if (!strcmp(name,"__android_log_write") || !strcmp(name,"__android_log_assert")) {
            compatLogFmt("game[%s]: %s", gstr(arg(c, 1), "?"), gstr(arg(c, 2), ""));
            ret = 0; return true;
        }
    }

    // ── libc misc ──
    if (!strcmp(name,"abort") || !strcmp(name,"exit") || !strcmp(name,"_exit")) {
        compatLogFmt("arm32: guest called %s(%d) — stopping", name, arg(c,0));
        c.halt = true; c.halt_pc = c.r[15]; ret = 0; return true;
    }
    if (!strcmp(name,"getenv"))  { ret = 0; return true; }         // no env
    if (!strcmp(name,"rand"))    { ret = (uint32_t)rand(); return true; }
    if (!strcmp(name,"srand"))   { srand(arg(c,0)); ret = 0; return true; }
    if (!strcmp(name,"time"))    { uint32_t p=arg(c,0); if(p && guestValid(p,4)) *(uint32_t*)toHost(p)=0; ret = 0; return true; }
    if (!strcmp(name,"malloc_usable_size")) { ret = 0; return true; }

    // ── stdio file I/O — guest FILE* is an index into s_files; paths resolve
    //    against the game's asset dir (logged, so runs reveal what's opened). ──
    if (!strcmp(name,"fopen")) {
        const char* gp = hstr(arg(c,0)); const char* mode = hstr(arg(c,1));
        if (!gp) { ret = 0; return true; }
        std::string path = gp;
        if (path.rfind("sdmc:",0)!=0 && path.rfind("/",0)!=0) path = std::string(g_asset_dir) + gp;
        else if (path.rfind("assets/",0)==0) path = std::string(g_asset_dir) + (gp+7);
        FILE* f = fopen(path.c_str(), mode?mode:"rb");
        compatLogFmt("arm32: fopen(%s,%s) -> %s", gp, mode?mode:"?", f?"ok":"FAIL");
        if (!f) { ret = 0; return true; }
        s_files.push_back(f); ret = (uint32_t)s_files.size();   // handle = index+1
        return true;
    }
    if (!strcmp(name,"fclose")) { FILE* f=gfile(arg(c,0)); if(f){fclose(f); s_files[arg(c,0)-1]=nullptr;} ret=0; return true; }
    if (!strcmp(name,"fread")) {
        uint32_t p=arg(c,0), sz=arg(c,1)*arg(c,2); FILE* f=gfile(arg(c,3));
        ret = (f && guestValid(p,sz)) ? (uint32_t)(fread(hptr(p),1,sz,f)/(arg(c,1)?arg(c,1):1)) : 0; return true;
    }
    if (!strcmp(name,"fwrite")) {
        uint32_t p=arg(c,0), sz=arg(c,1)*arg(c,2); FILE* f=gfile(arg(c,3));
        ret = (f && guestValid(p,sz)) ? (uint32_t)(fwrite(hptr(p),1,sz,f)/(arg(c,1)?arg(c,1):1)) : 0; return true;
    }
    if (!strcmp(name,"fseek"))  { FILE* f=gfile(arg(c,0)); ret = f?(uint32_t)fseek(f,(long)arg(c,1),(int)arg(c,2)):(uint32_t)-1; return true; }
    if (!strcmp(name,"ftell"))  { FILE* f=gfile(arg(c,0)); ret = f?(uint32_t)ftell(f):(uint32_t)-1; return true; }
    if (!strcmp(name,"rewind")) { FILE* f=gfile(arg(c,0)); if(f) rewind(f); ret=0; return true; }
    if (!strcmp(name,"feof"))   { FILE* f=gfile(arg(c,0)); ret = f?(uint32_t)feof(f):1; return true; }
    if (!strcmp(name,"fgetc"))  { FILE* f=gfile(arg(c,0)); ret = f?(uint32_t)fgetc(f):(uint32_t)-1; return true; }
    // getc is fgetc by another name; the game reached it during constructors.
    if (!strcmp(name,"getc"))   { FILE* f=gfile(arg(c,0)); ret = f?(uint32_t)fgetc(f):(uint32_t)-1; return true; }
    if (!strcmp(name,"ungetc")) { FILE* f=gfile(arg(c,1)); ret = f?(uint32_t)ungetc((int)arg(c,0),f):(uint32_t)-1; return true; }
    if (!strcmp(name,"fputc"))  { FILE* f=gfile(arg(c,1)); ret = f?(uint32_t)fputc((int)arg(c,0),f):(uint32_t)-1; return true; }
    if (!strcmp(name,"fputs"))  { FILE* f=gfile(arg(c,1)); ret = (f&&arg(c,0))?(uint32_t)fputs(hstr(arg(c,0)),f):(uint32_t)-1; return true; }
    if (!strcmp(name,"ferror")) { FILE* f=gfile(arg(c,0)); ret = f?(uint32_t)ferror(f):1; return true; }
    if (!strcmp(name,"fflush")) { FILE* f=gfile(arg(c,0)); if(f) fflush(f); ret=0; return true; }

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
