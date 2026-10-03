/* minx - the kernel's first allocator.
 *
 * A first-fit free-list allocator over page-aligned regions that are handed to
 * it explicitly.  Phase 1 attaches a static arena so the console can allocate
 * before any firmware is parsed; the physical memory manager attaches the rest
 * of RAM later through kmalloc_attach_region().
 *
 * Blocks are prefixed with a header holding the payload size in bytes and a
 * free flag.  The free list is a doubly linked list threaded through the free
 * blocks themselves, so no external bookkeeping is needed.
 */
#ifndef MINX_KMALLOC_H
#define MINX_KMALLOC_H

#include <stdbool.h>
#include <stddef.h>

void  kmalloc_init(void);

/* Hand a page-aligned, page-sized-multiple region to the allocator. */
void  kmalloc_attach_region(void *base, size_t size);

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);
char *kstrdup(const char *s);

size_t kmalloc_free_bytes(void);
size_t kmalloc_used_bytes(void);
size_t kmalloc_total_bytes(void);
size_t kmalloc_largest_free(void);
unsigned kmalloc_region_count(void);

/* Walk the free list and print every block.  Compiled in unconditionally
 * because it is the only practical way to debug coalescing from a serial log,
 * but nothing in the kernel calls it in normal operation. */
void kmalloc_dump(void);

#endif /* MINX_KMALLOC_H */