#include "dynheap.h"

#include <malloc.h>
#include <stdlib.h>

#define HEAP_GRANULARITY 0x200000UL /* svcSetHeapSize works in 2 MiB units. */
#define BLOCK_ALIGN      0x1000UL
/* Allocations at least this big go to the transient block. Only framebuffers are that large
 * (the biggest other allocations are decoder buffers of a few tens of KiB). It was 1 MiB,
 * which sent a 640x360 double-buffered framebuffer (960 KiB) to the 384 KiB inner heap ->
 * framebufferCreate failed with 0x559 (out of memory) on hardware, 2026-11-14. */
#define BIG_ALLOC_THRESHOLD (256 * 1024)

/* The block is a bump allocator: framebuffer first (via the libnx hook), then any extra
 * buffers (GIF canvas) via dynheapAlloc. Nothing is freed individually; the whole block
 * goes back to the kernel in dynheapRelease. */
static u8    *g_base = NULL;
static size_t g_size = 0;
static size_t g_used = 0;

Result dynheapAcquire(size_t size) {
    if (g_base)
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);

    size = (size + HEAP_GRANULARITY - 1) & ~(HEAP_GRANULARITY - 1);

    void *addr = NULL;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
        return rc;

    g_base = addr;
    g_size = size;
    g_used = 0;
    return 0;
}

void dynheapRelease(void) {
    if (!g_base)
        return;

    void *addr = NULL;
    svcSetHeapSize(&addr, 0);
    g_base = NULL;
    g_size = 0;
    g_used = 0;
}

size_t dynheapSize(void) {
    return g_size;
}

void *dynheapAlloc(size_t size) {
    if (!g_base)
        return NULL;
    const size_t start = (g_used + BLOCK_ALIGN - 1) & ~(BLOCK_ALIGN - 1);
    if (start + size > g_size)
        return NULL;
    g_used = start + size;
    return g_base + start;
}

static bool inBlock(const void *p) {
    return g_base && (const u8 *)p >= g_base && (const u8 *)p < g_base + g_size;
}

/* ---- libnx allocator hooks (weak in libnx) ---- */

void *__libnx_alloc(size_t size) {
    return malloc(size);
}

void *__libnx_aligned_alloc(size_t alignment, size_t size) {
    /* The kernel heap base is at least page aligned. libnx asks for 0x1000 alignment. */
    if (size >= BIG_ALLOC_THRESHOLD && alignment <= BLOCK_ALIGN) {
        void *p = dynheapAlloc(size);
        if (p)
            return p;
    }
    return memalign(alignment, size);
}

void __libnx_free(void *p) {
    if (!p || inBlock(p))
        return; /* block memory is released all at once */
    free(p);
}
