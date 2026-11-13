#include "dynheap.h"

#include <malloc.h>
#include <stdlib.h>

#define HEAP_GRANULARITY 0x200000UL /* svcSetHeapSize works in 2 MiB units. */
#define BIG_ALLOC_THRESHOLD (1024 * 1024)

static void  *g_base = NULL;
static size_t g_size = 0;
static bool   g_in_use = false;

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
    g_in_use = false;
    return 0;
}

void dynheapRelease(void) {
    if (!g_base)
        return;

    void *addr = NULL;
    svcSetHeapSize(&addr, 0);
    g_base = NULL;
    g_size = 0;
    g_in_use = false;
}

size_t dynheapSize(void) {
    return g_size;
}

/* ---- libnx allocator hooks (weak in libnx) ---- */

void *__libnx_alloc(size_t size) {
    return malloc(size);
}

void *__libnx_aligned_alloc(size_t alignment, size_t size) {
    /* The kernel heap base is at least page aligned. libnx asks for 0x1000 alignment. */
    if (g_base && !g_in_use && size >= BIG_ALLOC_THRESHOLD && size <= g_size && alignment <= 0x1000) {
        g_in_use = true;
        return g_base;
    }
    return memalign(alignment, size);
}

void __libnx_free(void *p) {
    if (!p)
        return;
    if (p == g_base) {
        g_in_use = false;
        return;
    }
    free(p);
}
