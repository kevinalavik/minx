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

#include "types.h"

/* The block geometry: header size, alignment, and the bytes kept past every payload that
 * the caller is never told about.
 *
 * These were private to `kmalloc.c`, and `tools/kmalloc-host-test.c` grew its own copy of
 * the arithmetic that decides how big a block has to be -- without the redzone.  Its check
 * was therefore one redzone short, and it failed on a block of 896 bytes whose header said
 * exactly what the allocator had promised: 819 bytes of request, rounded up to 832, plus the
 * 16-byte redzone, plus a 48-byte header for a split that would have left nothing behind.
 * The allocator was right and the test was wrong, which is the worst way for a test to be
 * wrong because it takes the implementation's word for nothing.
 *
 * One definition, in a header both sides include.  `sizeof(km_block_t)` is what makes this
 * a header at all: it needs the struct, and the struct is the layout both sides already had
 * to agree on.
 */
#define KM_ALIGN          16u

/* 16 bytes past the end of every payload, poisoned on allocation and checked on free.
 *
 * A write past the end of a block lands in the *next* block's header, or in the next
 * allocation's payload, and neither of them is anywhere the writer can see.  What it looks
 * like is memory corruption with no witness: a task that came back from `fork()` holding
 * somebody else's number, a free list holding an address in the program's own text, a test
 * that fails in a different place each run.
 *
 * This kernel hit exactly that, and the interesting part is *which* write it was: the
 * shell's `cmd_free()` released `c->stages` and `c->arms` a second time, and the second
 * release merged the wrong neighbour into a block that was already on the free list.  That
 * is a user-space bug, and it is in the shell rather than here.
 *
 * What made it findable was giving the user-space allocator the same redzone, which named
 * the caller at the `free()` instead of leaving a corrupt free list to be explained months
 * later.  The kernel heap gets the same thing, because it is the allocator that has no
 * other way of saying "that was not yours": the shell's corruption is fixed, and a kernel
 * one would be invisible in exactly the same way. */
#define KM_REDZONE        16u

/* What the redzone is filled with, so that "was written past the end" is a question with
 * an answer rather than a guess. */
#define KM_PATTERN        0xA5

/* The block header, and the size it occupies.
 *
 * This was in `kmalloc.c`, and `tools/kmalloc-host-test.c` mirrored it field for field so
 * that the test could look at the block behind an allocation -- with a `_Static_assert`
 * that the mirror had not drifted.  That is a workable arrangement and this one is better,
 * because there is now a single definition instead of a mirror that is checked after the
 * fact.
 *
 * `want` is what *this* allocation asked for, rounded up, and it is not the same as `size`.
 * A block whose leftover could not hold a header of its own keeps its capacity, so `size`
 * may be larger than the request; the redzone sits immediately after the request, and only
 * `want` says where that is. */
typedef struct km_block km_block_t;

struct km_block {
    km_block_t *prev;        /* physical neighbour, NULL at a region's start */
    km_block_t *next;        /* physical neighbour                       */
    km_block_t *free_next;   /* free-list linkage                        */
    km_block_t *free_prev;
    size_t      size;        /* capacity in payload bytes, excluding this header */
    size_t      want;        /* what this allocation asked for, rounded up      */
    bool        free;
};

#define KM_HEADER_SIZE ALIGN_UP(sizeof(km_block_t), KM_ALIGN)

void  kmalloc_init(void);

/* Hand a page-aligned, page-sized-multiple region to the allocator. */
void  kmalloc_attach_region(void *base, size_t size);

/* Take `bytes` worth of real frames from the physical allocator and attach them,
 * mapped through the direct map.  Called once the virtual memory manager is
 * live; before that the kernel has nothing but the static arena. */
int kmalloc_attach_pmm(size_t bytes);

/* Whether the heap has ever taken frames from the physical allocator. */
bool kmalloc_has_pmm(void);

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