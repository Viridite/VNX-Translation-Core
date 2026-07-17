// ELF32 (EM_ARM) loader: map PT_LOAD into the guest region, apply REL
// relocations, and resolve imports to bridge sentinels.
#include "arm32/arm32_internal.h"
#include "compat/loader.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace a32 {

// Minimal ELF32 structures (little-endian ARM).
struct Ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };
struct Dyn  { int32_t tag; uint32_t val; };
struct Sym  { uint32_t name; uint32_t value, size; uint8_t info, other; uint16_t shndx; };
struct Rel  { uint32_t offset, info; };

enum { PT_LOAD = 1, PT_DYNAMIC = 2 };
enum { DT_NULL=0, DT_HASH=4, DT_STRTAB=5, DT_SYMTAB=6, DT_RELA=7, DT_RELASZ=8,
       DT_STRSZ=10, DT_SYMENT=11, DT_INIT=12, DT_REL=17, DT_RELSZ=18, DT_RELENT=19,
       DT_PLTREL=20, DT_JMPREL=23, DT_PLTRELSZ=2, DT_INIT_ARRAY=25, DT_INIT_ARRAYSZ=27 };
enum { R_ARM_ABS32=2, R_ARM_REL32=3, R_ARM_GLOB_DAT=21, R_ARM_JUMP_SLOT=22, R_ARM_RELATIVE=23 };

// Dynamic tables (guest-relative), kept for elf32Sym.
static const Sym*  s_symtab = nullptr;
static const char* s_strtab = nullptr;
static uint32_t    s_symcnt = 0;
static uint32_t    s_bias   = 0;

static const char* symName(const Sym& s) { return s_strtab ? s_strtab + s.name : ""; }

// Resolve a symbol needed by a relocation: our own defined syms first, then the
// native shim (as a bridge sentinel).
static uint32_t resolveSym(const Sym& s) {
    const char* nm = symName(s);
    if (s.shndx != 0 /*SHN_UNDEF*/ && s.value)      // defined here
        return s_bias + s.value;
    if (nm && nm[0]) return bridgeRegister(nm);     // import → sentinel
    return 0;
}

static void applyRels(const Rel* rels, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t type = rels[i].info & 0xff;
        uint32_t sidx = rels[i].info >> 8;
        uint32_t where_g = s_bias + rels[i].offset;
        if (!guestValid(where_g, 4)) continue;
        uint32_t* where = (uint32_t*)toHost(where_g);
        switch (type) {
            case R_ARM_RELATIVE:  *where += s_bias; break;
            case R_ARM_ABS32:     *where += (s_symtab ? resolveSym(s_symtab[sidx]) : 0); break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: *where  = (s_symtab ? resolveSym(s_symtab[sidx]) : 0); break;
            case R_ARM_REL32:     *where += (s_symtab ? resolveSym(s_symtab[sidx]) : 0) - where_g; break;
            default: break;
        }
    }
}

static LoadedElf32 g_loaded;

LoadedElf32 elf32Load(const char* host_path) {
    LoadedElf32 out;
    FILE* f = fopen(host_path, "rb");
    if (!f) { compatLogFmt("arm32: cannot open %s", host_path); return out; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* file = (uint8_t*)malloc(fsz);
    if (!file || fread(file, 1, fsz, f) != (size_t)fsz) { fclose(f); free(file); return out; }
    fclose(f);

    Ehdr* eh = (Ehdr*)file;
    if (memcmp(eh->ident, "\x7f""ELF", 4) != 0 || eh->ident[4] != 1 /*ELFCLASS32*/ || eh->machine != 40 /*EM_ARM*/) {
        compatLog("arm32: not an ELF32 ARM object"); free(file); return out;
    }

    const uint32_t bias = A32_LOAD_BASE;
    s_bias = bias;
    Phdr* ph = (Phdr*)(file + eh->phoff);
    uint32_t minv = 0xffffffff, maxv = 0;

    // 1) Map PT_LOAD segments.
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        uint32_t seg_g = bias + ph[i].vaddr;
        if (!guestValid(seg_g, ph[i].memsz)) { compatLogFmt("arm32: segment OOB vaddr=0x%x", ph[i].vaddr); free(file); return out; }
        memset(toHost(seg_g), 0, ph[i].memsz);
        memcpy(toHost(seg_g), file + ph[i].offset, ph[i].filesz);
        if (ph[i].vaddr < minv) minv = ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > maxv) maxv = ph[i].vaddr + ph[i].memsz;
        compatLogFmt("arm32: LOAD vaddr=0x%x filesz=0x%x memsz=0x%x -> guest 0x%x",
                     ph[i].vaddr, ph[i].filesz, ph[i].memsz, seg_g);
    }

    // 2) Parse PT_DYNAMIC (read from the now-mapped image).
    const Rel* rel = nullptr; uint32_t relsz = 0;
    const Rel* jmp = nullptr; uint32_t jmpsz = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_DYNAMIC) continue;
        Dyn* dyn = (Dyn*)toHost(bias + ph[i].vaddr);
        for (; dyn->tag != DT_NULL; dyn++) {
            switch (dyn->tag) {
                case DT_SYMTAB: s_symtab = (const Sym*)toHost(bias + dyn->val); break;
                case DT_STRTAB: s_strtab = (const char*)toHost(bias + dyn->val); break;
                case DT_REL:    rel = (const Rel*)toHost(bias + dyn->val); break;
                case DT_RELSZ:  relsz = dyn->val; break;
                case DT_JMPREL: jmp = (const Rel*)toHost(bias + dyn->val); break;
                case DT_PLTRELSZ: jmpsz = dyn->val; break;
                case DT_INIT:   out.entry_init = bias + dyn->val; break;
                case DT_INIT_ARRAY:   out.init_array = bias + dyn->val; break;
                case DT_INIT_ARRAYSZ: out.init_count = dyn->val / 4; break;
                default: break;
            }
        }
    }
    // A rough symbol count from the string table span isn't reliable; use the
    // hash table's nchain if present, else leave 0 (only used by elf32Sym scan).
    s_symcnt = 0;

    // 3) Relocations.
    if (rel) applyRels(rel, relsz / sizeof(Rel));
    if (jmp) applyRels(jmp, jmpsz / sizeof(Rel));

    // 4) Find JNI_OnLoad by scanning the dynsym (bounded by strtab start).
    out.jni_onload = elf32Sym("JNI_OnLoad");

    out.load_bias = bias;
    out.min_vaddr = minv; out.max_vaddr = maxv;
    out.ok = true;
    g_loaded = out;
    free(file);
    compatLogFmt("arm32: ELF32 loaded bias=0x%x init=0x%x init_array=0x%x(%u) JNI_OnLoad=0x%x",
                 bias, out.entry_init, out.init_array, out.init_count, out.jni_onload);
    return out;
}

uint32_t elf32Sym(const char* name) {
    if (!s_symtab || !s_strtab) return 0;
    // Scan until the symtab runs into the strtab (they're adjacent in .dynamic).
    const Sym* s = s_symtab;
    const char* symend = (const char*)s_strtab;
    while ((const char*)s < symend) {
        if (s->shndx != 0 && s->value && s->name) {
            const char* nm = s_strtab + s->name;
            if (strcmp(nm, name) == 0) return s_bias + s->value;
        }
        s++;
    }
    return 0;
}

}  // namespace a32
