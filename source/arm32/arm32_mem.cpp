// ARM32 guest memory: one host-backed region, base+offset translation, and a
// simple guest heap allocator (the game's malloc must return 32-bit guest
// addresses, so it can't use the native newlib heap).
#include "arm32/arm32_internal.h"
#include "compat/loader.h"
#include <malloc.h>
#include <cstring>

namespace a32 {

uint8_t*  g_base = nullptr;   // host address of guest 0
size_t    g_region = 0;

// Guest layout (all within [0, A32_REGION_SIZE)).
static uint32_t s_heap_ptr = A32_HEAP_START;

bool memInit() {
    if (g_base) return true;
    // Reserve the whole guest window on the host heap. Fall back to smaller
    // sizes if the full 512 MiB can't be had.
    size_t want = A32_REGION_SIZE;
    while (want >= 128ull * 1024 * 1024) {
        g_base = (uint8_t*)memalign(0x1000, want);
        if (g_base) { g_region = want; break; }
        want /= 2;
    }
    if (!g_base) { compatLog("arm32: FAILED to reserve guest region"); return false; }
    memset(g_base, 0, 0x1000);   // zero the guard page
    compatLogFmt("arm32: guest region host=%p size=0x%zx", (void*)g_base, g_region);
    return true;
}

uint8_t* toHost(uint32_t guest) { return g_base + guest; }

uint32_t toGuest(const void* host) {
    return (uint32_t)((const uint8_t*)host - g_base);
}

bool guestValid(uint32_t addr, uint32_t len) {
    return g_base && (uint64_t)addr + len <= g_region;
}

// Bump allocator + tiny free list. Good enough to bring the game up; a real
// dlmalloc-in-guest can come later if fragmentation matters.
struct FreeBlock { uint32_t next; uint32_t size; };
static uint32_t s_free_head = 0;   // guest addr of first free block, 0 = none

uint32_t guestAlloc(uint32_t size) {
    if (size == 0) size = 1;
    size = (size + 15) & ~15u;                 // 16-byte align
    // First-fit over the free list.
    uint32_t prev = 0, cur = s_free_head;
    while (cur) {
        FreeBlock* b = (FreeBlock*)toHost(cur);
        if (b->size >= size) {
            if (prev) ((FreeBlock*)toHost(prev))->next = b->next;
            else      s_free_head = b->next;
            return cur + 16;                    // payload after the 16-byte header
        }
        prev = cur; cur = b->next;
    }
    // Bump.
    uint32_t hdr = s_heap_ptr;
    uint32_t need = 16 + size;
    if ((uint64_t)hdr + need > A32_HEAP_END) { compatLog("arm32: guest heap OOM"); return 0; }
    s_heap_ptr += need;
    FreeBlock* h = (FreeBlock*)toHost(hdr);
    h->size = size; h->next = 0;
    return hdr + 16;
}

void guestFree(uint32_t payload) {
    if (!payload) return;
    uint32_t hdr = payload - 16;
    FreeBlock* h = (FreeBlock*)toHost(hdr);
    h->next = s_free_head;
    s_free_head = hdr;
}

uint32_t guestRealloc(uint32_t payload, uint32_t size) {
    if (!payload) return guestAlloc(size);
    uint32_t hdr = payload - 16;
    uint32_t old = ((FreeBlock*)toHost(hdr))->size;
    if (size <= old) return payload;
    uint32_t n = guestAlloc(size);
    if (n) memcpy(toHost(n), toHost(payload), old);
    guestFree(payload);
    return n;
}

}  // namespace a32
