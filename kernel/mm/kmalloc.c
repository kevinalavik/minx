#include "kmalloc.h"

#include <stdalign.h>

#include "kprintf.h"
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

#define KM_ALIGN       16u
#define KM_HEADER_SIZE ALIGN_UP(sizeof(km_block_t), KM_ALIGN)

typedef struct km_block km_block_t;

struct km_block {
    km_block_t *prev;        /* physical neighbour, NULL at a region's start */
    km_block_t *next;        /* physical neighbour                       */
    km_block_t *free_next;   /* free-list linkage                        */
    km_block_t *free_prev;
    size_t      size;        /* payload bytes, excluding this header      */
    bool        free;
};

static km_block_t *free_list;
static size_t total_bytes;
static size_t used_bytes;
static size_t free_bytes;
static unsigned region_count;

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
}

void kmalloc_init(void) {
    free_list = NULL;
    total_bytes = 0;
    used_bytes = 0;
    free_bytes = 0;
    region_count = 0;

    if (!arena_used) {
        kmalloc_attach_region(boot_arena, sizeof(boot_arena));
        arena_used = true;
    }
}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;

    size_t need = align_up_km(size);

    for (km_block_t *b = free_list; b; b = b->free_next) {
        if (b->size < need)
            continue;

        free_list_remove(b);

        /* Split when the remainder can hold a header plus a usable payload.
         * The new tail has to be spliced *into* the chain in front of whatever
         * used to follow b: dropping b->next here would orphan the blocks after
         * it, and a later coalesce would then merge across a live header. */
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

        used_bytes += b->size;
        memset(payload_of(b), 0, b->size);
        return payload_of(b);
    }

    return NULL;
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
    if (b->size >= align_up_km(size))
        return ptr;

    void *fresh = kmalloc(size);
    if (fresh == NULL)
        return NULL;
    size_t copy = b->size < size ? b->size : size;
    memcpy(fresh, ptr, copy);
    kfree(ptr);
    return fresh;
}

void kfree(void *ptr) {
    if (ptr == NULL)
        return;

    km_block_t *b = block_of(ptr);
    if (b->free)
        return;   /* double free: refuse rather than corrupt the list */

    /* Remember what this allocation itself accounted for: coalescing below can
     * grow the block, and used_bytes must track payload handed out, not the
     * size of whatever the merged block happens to be. */
    size_t own = b->size;

    free_list_insert(b);
    coalesce(b);
    used_bytes -= own;
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
    return free_bytes;
}

size_t kmalloc_used_bytes(void) {
    return used_bytes;
}

size_t kmalloc_total_bytes(void) {
    return total_bytes;
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