/* How much the heap asks for the first time it grows past the static arena.
 * Phase 3 hands the kernel a real heap here; the static 4 MiB arena stays as the
 * floor so that a machine with very little memory still boots. */
#define HEAP_INITIAL_BYTES (64ull * 1024 * 1024)

/* minx - physical memory manager.
 *
 * One bit per 4 KiB frame, covering the whole physical address space the
 * firmware described.  A bitmap rather than a buddy allocator because it is
 * small enough to audit, its cost is O(1) per operation, and it cannot fail to
 * coalesce: fragmentation is handled once, when a caller asks for contiguous
 * pages and the free ones are not adjacent.
 *
 * The bitmap itself must be allocated before this allocator exists, so it comes
 * out of a static array.  It covers 64 GiB of physical address space, which is
 * 2 MiB of bitmap; a machine with more reports the clamp rather than quietly
 * miscounting.
 */
#ifndef MINX_PMM_H
#define MINX_PMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

/* Take the bootloader's memory map and build the frame database.  Must run
 * before anything else allocates, and only once. */
int  pmm_init(void);

/* Reserve everything the kernel is already standing on: its own image, the
 * framebuffer, the initrd and the bootloader's page tables.  Called by
 * pmm_init(), and again later to release the page tables once CR3 no longer
 * points at them. */
void pmm_reserve_boot_areas(void);
void pmm_release_boot_tables(void);

/* Allocate one 4 KiB frame.  Returns a physical address, or 0 when the pool is
 * exhausted.  The contents are not zeroed: use pmm_alloc_zeroed_page for that,
 * because zeroing 4 KiB that the caller is about to overwrite is pure waste. */
uint64_t pmm_alloc_phys_page(void);
uint8_t *pmm_alloc_page(void);          /* zeroed, via the direct map */
uint8_t *pmm_alloc_zeroed_page(void);

/* Allocate `count` contiguous frames, or 0 if they are not available as a
 * single run.  Contiguity is what page tables and large mappings need, so it is
 * part of the interface rather than something callers work around. */
uint8_t *pmm_alloc_pages(size_t count);
uint8_t *pmm_alloc_zeroed_pages(size_t count);

void pmm_free_page(void *virt);
void pmm_free_phys_page(uint64_t phys);
void pmm_free_pages(void *virt, size_t count);
void pmm_free_phys_pages(uint64_t phys, size_t count);

/* Release a whole physical range, rounded outwards to page boundaries.  Frames
 * that were already free are counted once, so releasing twice cannot inflate the
 * free-frame total -- which the leak check in phase 11 would then read as a
 * surplus of memory rather than a double free. */
void pmm_free_phys_range(uint64_t phys, size_t bytes);

/* Mark frames as used without changing the free/total counters, for regions the
 * kernel is already occupying. */
void pmm_reserve_phys_range(uint64_t phys, size_t bytes);

/* Number of frames between two addresses, for reserving a range. */
static inline size_t pmm_frames_for(size_t bytes) {
    return (bytes + page_size - 1) / page_size;
}

/* Statistics.  These are what /proc/meminfo and the phase-11 leak check report,
 * so free must be able to be trusted: every path that takes a frame has to go
 * through one of the alloc functions, and every path that returns one through a
 * free function. */
typedef struct {
    uint64_t total_frames;
    uint64_t free_frames;
    uint64_t used_frames;      /* allocated by anyone, including page tables */
    uint64_t reserved_frames;  /* the kernel image, the initrd, the arena    */
    uint64_t largest_free_run;
    uint64_t highest_allocated;  /* physical, for the direct map to cover */
    uint64_t total_bytes;
} pmm_stats_t;

void pmm_get_stats(pmm_stats_t *out);

/* True once pmm_init() has succeeded. */
bool pmm_ready(void);

/* Does the direct map cover this physical address?  Anything the kernel reads
 * through phys_to_virt() must be inside it. */
bool pmm_physical_is_mapped(uint64_t phys);

/* The physical address one past the last usable frame, which is how much of
 * physical memory the direct map has to cover. */
uint64_t pmm_highest_usable_phys(void);

#endif /* MINX_PMM_H */
