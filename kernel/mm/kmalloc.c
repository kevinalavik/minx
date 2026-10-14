#include "kmalloc.h"

#include <stdalign.h>

#include "kprintf.h"
#include "pmm.h"
#include "spinlock.h"
#include "string.h"
#include "types.h"

/* First-fit free list over page-aligned regions handed to us explicitly.
 *
 * Blocks form a doubly linked physical chain within each region, and free
 * blocks additionally thread a singly linked free list through themselves, so
 * coalescing only has to look at the immediate neighbours.  Only the very
 * first block of a region has prev == NULL, which is what stops a merge from
 * running off the front of a region -- no separate "is a region head" flag is
 * needed, and every block, including the first, can be freed.
 *
 * Headers are 64 bytes so that payloads stay 16-byte aligned, which matters for
 * the atomic 128-bit FPU/SSE context areas and for the x87 control word.
 */

static km_block_t *free_list;
static size_t total_bytes;
static size_t used_bytes;
static size_t free_bytes;
static unsigned region_count;
static bool pmm_backed;   /* the heap has taken real frames at least once */

/* The heap's lock, and why it is not optional.
 *
 * Every processor allocates: a page table for a fault, a `task_t`, a pipe, a console
 * context.  The free list is a singly linked chain threaded through the blocks themselves,
 * so an allocation is a read-modify-write of `free_list` plus a read-modify-write of two
 * neighbouring headers -- and two processors doing that at once produce a list with a block
 * on it twice and a block on it not at all.
 *
 * What that looks like is nothing like a race.  A `kmalloc()` that walks into the cycle
 * spins for ever holding no lock and printing nothing, and the machine is silent: there is
 * no fault, no panic, and the last line of the log is the line before whatever asked for
 * memory.  A block handed out twice is two subsystems writing over each other's data.  And
 * `free_bytes` drifts away from the truth, so the heap eventually either refuses to give
 * out memory it has or hands out memory it has not got.
 *
 * That is not hypothetical here: it is what `make WITH_SHELL=1 test` was doing, at a
 * different place in the run each time.
 *
 * Irq-saving, because a frame is allocated from a page fault, which is an exception and
 * not an interrupt -- and an interrupt that spins on a lock the interrupted code holds
 * never returns at all. */
static ticket_lock_t heap_lock;

/* Payloads must stay 16-byte aligned for the atomic FPU/SSE context areas, and
 * a header has to be a whole number of those so that the next block's header
 * lands aligned too. */
_Static_assert(sizeof(km_block_t) <= KM_HEADER_SIZE, "block header too large");
_Static_assert((KM_HEADER_SIZE % KM_ALIGN) == 0, "header must be KM_ALIGN sized");

/* Static arena used before the physical memory manager exists. */
static uint8_t boot_arena[4u * 1024u * 1024u] __attribute__((aligned(4096)));
static bool arena_used;

static size_t align_up_km(size_t v) {
    return ALIGN_UP(v, KM_ALIGN);
}

static inline km_block_t *block_of(void *ptr) {
    return (km_block_t *)((uint8_t *)ptr - KM_HEADER_SIZE);
}

static inline void *payload_of(km_block_t *b) {
    return (uint8_t *)b + KM_HEADER_SIZE;
}

static void free_list_insert(km_block_t *b) {
    b->free = true;
    b->free_prev = NULL;
    b->free_next = free_list;
    if (free_list)
        free_list->free_prev = b;
    free_list = b;
    free_bytes += b->size;
}

static void free_list_remove(km_block_t *b) {
    if (b->free_prev)
        b->free_prev->free_next = b->free_next;
    else
        free_list = b->free_next;
    if (b->free_next)
        b->free_next->free_prev = b->free_prev;
    b->free_next = NULL;
    b->free_prev = NULL;
    b->free = false;
    free_bytes -= b->size;
}

/* Absorb *victim* into *b*, which must be the physically adjacent neighbour. */
static void absorb(km_block_t *b, km_block_t *victim) {
    free_list_remove(victim);
    b->size += KM_HEADER_SIZE + victim->size;
    b->next = victim->next;
    if (b->next)
        b->next->prev = b;
    /* The victim's storage -- header included -- is back in the free pool. */
    free_bytes += KM_HEADER_SIZE + victim->size;
}

static void coalesce(km_block_t *b) {
    if (b->next && b->next->free)
        absorb(b, b->next);
    if (b->prev && b->prev->free)
        absorb(b->prev, b);
}

void kmalloc_attach_region(void *base, size_t size) {
    if (base == NULL || size < KM_HEADER_SIZE * 2)
        return;

    uint64_t lflags = spin_lock_irqsave(&heap_lock);

    uint8_t *start = (uint8_t *)base;
    /* Trim the region so it is header-aligned. */
    start += (KM_ALIGN - ((uintptr_t)start & (KM_ALIGN - 1))) & (KM_ALIGN - 1);
    size -= (size_t)(start - (uint8_t *)base);
    size = ALIGN_DOWN(size, KM_ALIGN);
    if (size < KM_HEADER_SIZE * 2)
        return;

    km_block_t *head = (km_block_t *)start;
    head->prev = NULL;
    head->next = NULL;
    head->free_next = NULL;
    head->free_prev = NULL;
    head->size = size - KM_HEADER_SIZE;
    head->free = false;

    free_list_insert(head);
    total_bytes += size;
    region_count++;

    spin_unlock_irqrestore(&heap_lock, lflags);
}

void kmalloc_init(void) {
    free_list = NULL;
    total_bytes = 0;
    used_bytes = 0;
    free_bytes = 0;
    region_count = 0;
    pmm_backed = false;

    if (!arena_used) {
        kmalloc_attach_region(boot_arena, sizeof(boot_arena));
        arena_used = true;
    }
}

int kmalloc_attach_pmm(size_t bytes) {
    if (!pmm_ready())
        return -1;

    size_t frames = pmm_frames_for(bytes);
    if (frames == 0)
        return -1;

    uint8_t *region = pmm_alloc_pages(frames);
    if (region == NULL) {
        kprintf("[warn] heap: no %lu contiguous frames for the heap\n",
                (unsigned long)frames);
        return -1;
    }

    /* The frames come back through the direct map, which the virtual memory
     * manager has to have built; without it phys_to_virt is not a valid
     * address. */
    kmalloc_attach_region(region, frames * page_size);
    pmm_backed = true;

    kprintf("heap: attached %lu KiB from the physical allocator at %p, "
            "%lu KiB free\n", (unsigned long)(frames * page_size / 1024),
            region, (unsigned long)kmalloc_free_bytes() / 1024);
    return 0;
}

bool kmalloc_has_pmm(void) { return pmm_backed; }

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;

    /* What the caller asked for, and what the block is.  The difference is the redzone,
     * which is inside the block's extent: it is never handed out and nothing is allocated
     * across it, so coalescing and the physical chain keep treating `size` as the whole
     * payload. */
    size_t want = align_up_km(size);
    size_t need = want + KM_REDZONE;
    void  *got = NULL;
    uint64_t lflags = spin_lock_irqsave(&heap_lock);

    for (km_block_t *b = free_list; b; b = b->free_next) {
        if (b->size < need)
            continue;

        free_list_remove(b);

        /* Split when the remainder can hold a header plus a usable payload.
         *
         * The new tail has to be spliced *into* the chain in front of whatever used to
         * follow b: dropping b->next here would orphan the blocks after it, and a later
         * coalesce would then merge across a live header.
         *
         * When there is not enough room, `b->size` is deliberately left at its capacity.
         * Shrinking it to the request would strand the few bytes between them inside a live
         * block where no block describes them: with a 48-byte header and a 16-byte
         * alignment that is up to 63 bytes per allocation, and a walk of the arena found
         * 896 of them gone after one fuzz run.  A block is allowed to be bigger than what
         * was asked of it -- that is what `want` is for. */
        if (b->size >= need + KM_HEADER_SIZE + KM_ALIGN) {
            km_block_t *tail = (km_block_t *)((uint8_t *)payload_of(b) + need);
            km_block_t *follows = b->next;

            tail->prev = b;
            tail->next = follows;
            tail->free_next = NULL;
            tail->free_prev = NULL;
            tail->size = b->size - need - KM_HEADER_SIZE;
            tail->free = false;

            b->next = tail;
            b->size = need;
            if (follows)
                follows->prev = tail;

            free_list_insert(tail);
        }

        b->want = want;

        used_bytes += b->size;
        memset(payload_of(b), 0, b->size);
        memset((uint8_t *)payload_of(b) + want, KM_PATTERN, KM_REDZONE);
        got = payload_of(b);
        break;
    }

    spin_unlock_irqrestore(&heap_lock, lflags);
    return got;
}

void *kzalloc(size_t size) {
    return kmalloc(size);
}

void *kcalloc(size_t count, size_t size) {
    if (count != 0 && size > (size_t)-1 / count)
        return NULL;
    return kmalloc(count * size);
}

void *krealloc(void *ptr, size_t size) {
    if (ptr == NULL)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    km_block_t *b = block_of(ptr);
    if (b->size >= align_up_km(size) + KM_REDZONE)
        return ptr;

    void *fresh = kmalloc(size);
    if (fresh == NULL)
        return NULL;
    size_t have = b->size > KM_REDZONE ? b->size - KM_REDZONE : 0;
    size_t copy = have < size ? have : size;
    memcpy(fresh, ptr, copy);
    kfree(ptr);
    return fresh;
}

void kfree(void *ptr) {
    if (ptr == NULL)
        return;

    km_block_t *b = block_of(ptr);
    uint64_t lflags = spin_lock_irqsave(&heap_lock);

    if (b->free) {
        spin_unlock_irqrestore(&heap_lock, lflags);
        return;   /* double free: refuse rather than corrupt the list */
    }

    /* Written past the end of this block?
     *
     * `kmalloc()` painted the redzone at the end of the payload and nothing else ever
     * touches it, so a byte that is not the pattern is a writer that did not know where
     * its allocation stopped.  Reported here, where the block's owner is about to become
     * nobody: the alternative is the write surfacing as corruption in whichever block
     * happens to be allocated next, which can be seconds later and a different subsystem.
     *
     * A report rather than a refusal, because the block is *this* one's to give back
     * either way -- refusing would leak it and hide the fault behind an allocation
     * failure somewhere else. */
    {
        /* From `want`, not from `size`.
         *
         * A block may be bigger than the allocation it is holding -- when the leftover
         * could not hold a header of its own, the capacity stays where it was -- and the
         * redzone is poisoned immediately after the *request*, so those are two different
         * places.  Reading `b->size - KM_REDZONE` looked past the poison into memory that
         * was merely zeroed, and so reported a block written past its end on *every*
         * unsplit free: always offset zero, always a zero where the pattern should have
         * been.  A check that is always wrong teaches its reader to ignore it, which is
         * worse than having no check at all.
         *
         * `want` is only meaningful for a live block, and this is a live block: a free one
         * returns above. */
        const uint8_t *z = (const uint8_t *)payload_of(b) + b->want;
        size_t at = 0;

        while (at < KM_REDZONE && z[at] == KM_PATTERN)
            at++;

        if (at < KM_REDZONE) {
            kprintf("[warn] heap: block %p (%lu bytes, capacity %lu) was written past "
                    "its end: %#x at +%lu, freed from %p\n", ptr,
                    (unsigned long)b->want, (unsigned long)b->size, z[at],
                    (unsigned long)(b->want + at),
                    __builtin_return_address(0));
        }
    }

    /* Remember what this allocation itself accounted for: coalescing below can
     * grow the block, and used_bytes must track payload handed out, not the
     * size of whatever the merged block happens to be. */
    size_t own = b->size;

    free_list_insert(b);
    coalesce(b);
    used_bytes -= own;

    spin_unlock_irqrestore(&heap_lock, lflags);
}

char *kstrdup(const char *s) {
    size_t len = strlen(s);
    char *copy = (char *)kmalloc(len + 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

size_t kmalloc_free_bytes(void) {
    uint64_t lflags = spin_lock_irqsave(&heap_lock);
    size_t    n = free_bytes;

    spin_unlock_irqrestore(&heap_lock, lflags);
    return n;
}

size_t kmalloc_used_bytes(void) {
    uint64_t lflags = spin_lock_irqsave(&heap_lock);
    size_t    n = used_bytes;

    spin_unlock_irqrestore(&heap_lock, lflags);
    return n;
}

size_t kmalloc_total_bytes(void) {
    uint64_t lflags = spin_lock_irqsave(&heap_lock);
    size_t    n = total_bytes;

    spin_unlock_irqrestore(&heap_lock, lflags);
    return n;
}

unsigned kmalloc_region_count(void) {
    return region_count;
}

/* Walks the free list, so the answer is exact rather than an estimate that
 * would drift every time a large block is allocated again. */
size_t kmalloc_largest_free(void) {
    size_t best = 0;
    for (km_block_t *b = free_list; b; b = b->free_next) {
        if (b->size > best)
            best = b->size;
    }
    return best;
}

void kmalloc_dump(void) {
    kprintf("kmalloc: total=%zu free=%zu used=%zu regions=%u\n",
            total_bytes, free_bytes, used_bytes, region_count);

    /* Every byte of every region, accounted for by walking the physical chains.
     *
     * `free_bytes` and `used_bytes` are both maintained incrementally, and an incremental
     * counter is exactly the thing that cannot tell "the accounting drifted" from "the
     * bytes are gone" -- it says the same number either way.  Walking the blocks is the only
     * way to tell them apart, and it is what found the 896 bytes a too-eager split was
     * stranding inside live blocks where no block described them.
     *
     * A region's head is the one block whose `prev` is NULL.  There is no array of them:
     * `kmalloc_attach_region()` only links the head onto the free list, so the heads are
     * found there. */
    {
        size_t sum_free = 0, sum_live = 0, nblk = 0;

        for (km_block_t *h = free_list; h != NULL; h = h->free_next) {
            if (h->prev != NULL)
                continue;               /* not a region head */

            for (km_block_t *b = h; b != NULL; b = b->next) {
                nblk++;

                if (b->free)
                    sum_free += b->size;
                else
                    sum_live += b->size;
            }
        }

        kprintf("kmalloc: walk blocks=%zu free=%zu live=%zu headers=%zu sum=%zu\n",
                nblk, sum_free, sum_live, nblk * KM_HEADER_SIZE,
                sum_free + sum_live + nblk * KM_HEADER_SIZE);
    }

    unsigned i = 0;
    for (km_block_t *b = free_list; b != NULL; b = b->free_next) {
        kprintf("  free[%02u] %p size=%zu prev=%p next=%p\n",
                i, (void *)b, b->size, (void *)b->prev, (void *)b->next);
        if (++i >= 40) {
            kprintf("  ... (truncated)\n");
            break;
        }
    }
}