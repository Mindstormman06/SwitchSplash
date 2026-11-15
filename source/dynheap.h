#pragma once
#include <switch.h>

/*
 * Transient "big block" memory for the framebuffer (DESIGN.md §3).
 *
 * The newlib heap is a small static array (see __libnx_initheap in main.c), so the
 * kernel heap region is unused. While a splash is showing we grow it with
 * svcSetHeapSize and hand it out as one block; afterwards we shrink it back to 0.
 *
 * libnx's weak __libnx_alloc/__libnx_aligned_alloc/__libnx_free are overridden so that
 * framebufferCreate's single large allocation lands in that block automatically.
 */

/* Grows the kernel heap to `size` (rounded up to 2 MiB). */
Result dynheapAcquire(size_t size);

/* Shrinks the kernel heap back to 0. The block must no longer be in use. */
void dynheapRelease(void);

size_t dynheapSize(void);

/* Carves `size` bytes (page aligned) out of the acquired block, after anything already
 * handed out. NULL if it doesn't fit. Freed only by dynheapRelease. */
void *dynheapAlloc(size_t size);
