// ARM32 runtime orchestration: set up the guest, load the ELF32, run its static
// constructors, and call JNI_OnLoad — the first bring-up milestone. Graphics,
// input, and the full JNI surface come once the interpreter covers enough of the
// instruction set to reach them (log-driven).
#include "arm32/arm32_internal.h"
#include "compat/loader.h"
#include <cstdio>
#include <string>

namespace a32 {

static constexpr uint32_t CPSR_T = 0x20;   // Thumb bit
static std::string s_asset_dir;
const char* g_asset_dir = "";

bool isElf32Arm(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[20] = {0};
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n < 20) return false;
    bool elf = (h[0]==0x7f && h[1]=='E' && h[2]=='L' && h[3]=='F');
    return elf && h[4] == 1 /*ELFCLASS32*/ && h[18] == 40 /*EM_ARM*/;
}

// Call a guest function (Thumb or ARM per its low bit) with up to 4 int args,
// running the interpreter until it returns to the trap. Returns r0.
static uint32_t callGuest(CpuState& c, uint32_t func, uint32_t a0=0, uint32_t a1=0,
                          uint32_t a2=0, uint32_t a3=0) {
    c.r[0]=a0; c.r[1]=a1; c.r[2]=a2; c.r[3]=a3;
    c.r[14] = A32_RETURN_TRAP;
    c.cpsr = (func & 1) ? (c.cpsr | CPSR_T) : (c.cpsr & ~CPSR_T);
    c.r[15] = func & ~1u;
    c.halt = false;
    cpuRun(c);
    return c.r[0];
}

int run(const char* main_so_host_path, const char* pkg) {
    compatLogFmt("arm32: bring-up for %s (%s)", pkg ? pkg : "?", main_so_host_path);
    if (!memInit()) return -1;

    s_asset_dir = std::string("sdmc:/Viridite/games/") + (pkg ? pkg : "") + "/assets/";
    g_asset_dir = s_asset_dir.c_str();

    LoadedElf32 elf = elf32Load(main_so_host_path);
    if (!elf.ok) { compatLog("arm32: ELF32 load failed"); return -2; }

    CpuState cpu = {};
    cpu.r[13] = A32_STACK_TOP;          // SP
    cpu.cpsr  = 0x10;                    // user mode

    // DT_INIT
    if (elf.entry_init) {
        compatLogFmt("arm32: DT_INIT @0x%x", elf.entry_init);
        callGuest(cpu, elf.entry_init);
        if (cpu.halt) return -3;
    }
    // DT_INIT_ARRAY (static constructors)
    // Skip a constructor that halts rather than stopping the run.
    //
    // Stopping at the first one means every test cycle reveals exactly one
    // problem, and each costs a build, a copy to the card and a launch. The
    // 64-bit path already recovers from a faulting constructor and carries on,
    // which is how we learned that 155 of libunity's fail for one reason
    // rather than finding them one release at a time.
    //
    // A skipped constructor leaves its object uninitialised, so the game is
    // not in a good state afterwards — but it was not going to run either way,
    // and knowing whether the next four hundred hit the same instruction or
    // four hundred different ones is worth far more than a clean stop.
    uint32_t ran = 0, halted = 0;
    for (uint32_t i = 0; i < elf.init_count; i++) {
        uint32_t fn = *(uint32_t*)toHost(elf.init_array + i*4);
        if (!fn || fn == 0xffffffff) continue;
        compatLogFmt("arm32: ctor[%u/%u] @0x%x", i+1, elf.init_count, fn);
        callGuest(cpu, fn);
        if (cpu.halt) {
            halted++;
            compatLogFmt("arm32: ctor[%u] halted at 0x%x — skipped", i+1, cpu.halt_pc);
            cpu.halt = false;
            // Give the next one a clean stack. The halted call left whatever
            // depth it had reached, and 479 abandoned frames would run the
            // guest stack into the heap.
            cpu.r[13] = A32_STACK_TOP;
            if (halted >= 64) {
                compatLogFmt("arm32: %u constructors halted — stopping, the "
                             "problem is systemic rather than per-constructor", halted);
                break;
            }
        } else {
            ran++;
        }
    }
    compatLogFmt("arm32: constructors done — %u ran, %u halted", ran, halted);
    cpuDumpUnimplSummary();

    // JNI_OnLoad(JavaVM*, void*) — pass 0/0 for now; the JNI object model needs
    // wiring through the bridge before this does anything useful.
    if (elf.jni_onload) {
        compatLogFmt("arm32: calling JNI_OnLoad @0x%x", elf.jni_onload);
        uint32_t r = callGuest(cpu, elf.jni_onload, 0, 0);
        compatLogFmt("arm32: JNI_OnLoad returned 0x%x (halt=%d)", r, cpu.halt);
    } else {
        compatLog("arm32: no JNI_OnLoad export");
    }

    // ── Run the game ────────────────────────────────────────────────────────
    // Everything above was bring-up: load, relocate, construct, JNI_OnLoad. It
    // then reported success and returned, which is why launching a 32-bit game
    // said it had worked and showed nothing — the loader had done its part and
    // nobody had asked the game to draw.
    //
    // cocos2d-x is driven from Java: nativeInit once with the surface size,
    // then nativeRender every frame. Those are ordinary exports, so the
    // interpreter can call them directly and the GL the guest issues goes
    // through the bridge onto the same context the launcher is already using.
    uint32_t nativeInit = elf32Sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
    if (!nativeInit) nativeInit = elf32Sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit__II");
    uint32_t nativeRender = elf32Sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");

    if (!nativeInit || !nativeRender) {
        compatLogFmt("arm32: no cocos2d-x renderer exports (init=0x%x render=0x%x) — "
                     "loaded but nothing to drive", nativeInit, nativeRender);
        return -5;
    }

    compatLogFmt("arm32: nativeInit @0x%x 1280x720", nativeInit);
    callGuest(cpu, nativeInit, 0, 0, 1280, 720);
    if (cpu.halt) {
        compatLogFmt("arm32: nativeInit halted at 0x%x", cpu.halt_pc);
        cpuDumpBranches();
        cpu.halt = false;
        cpu.r[13] = A32_STACK_TOP;
    } else {
        compatLog("arm32: nativeInit OK");
    }

    compatLogFmt("arm32: nativeRender @0x%x — entering frame loop", nativeRender);
    uint32_t frames = 0, halted_frames = 0;
    while (!requestedAbort()) {
        callGuest(cpu, nativeRender, 0, 0);
        if (cpu.halt) {
            // A frame that halts is reported once and then counted. The
            // interpreter is missing something the game reaches every frame, so
            // logging each one would bury everything else in the file.
            if (halted_frames == 0) {
                compatLogFmt("arm32: nativeRender halted at 0x%x on frame %u",
                             cpu.halt_pc, frames);
                cpuDumpBranches();
            }
            halted_frames++;
            cpu.halt = false;
            cpu.r[13] = A32_STACK_TOP;
            if (halted_frames > 240) {
                compatLog("arm32: every frame is halting — stopping");
                break;
            }
        }
        a32FrameSwap();
        frames++;
        if ((frames % 300) == 0)
            compatLogFmt("arm32: %u frames (%u halted)", frames, halted_frames);
    }
    compatLogFmt("arm32: frame loop ended — %u frames, %u halted", frames, halted_frames);
    cpuDumpUnimplSummary();
    return 0;
}

}  // namespace a32
