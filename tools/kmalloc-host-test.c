/* Host-side driver for the kernel allocator, so its bookkeeping can be checked
 * without booting QEMU.  Built by tools/test-kmalloc.sh; not shipped in the
 * kernel image.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kmalloc.h"

/* The kernel build routes kprintf through the console; on the host the dump
 * goes to stdout instead. */
void console_write(const char *data, size_t len) {
    fwrite(data, 1, len, stdout);
}
void console_puts(const char *s) {
    fputs(s, stdout);
}

#define ARENA (4u * 1024u * 1024u)
static unsigned char arena[ARENA] __attribute__((aligned(4096)));

/* Layout of the kernel's block header, mirrored here so the fuzz can inspect
 * the block behind a corrupted allocation.  Must match kernel/mm/kmalloc.c. */
struct host_block {
    struct host_block *prev;
    struct host_block *next;
    struct host_block *free_next;
    struct host_block *free_prev;
    size_t             size;
    unsigned char      is_free;   /* bool in the kernel: one byte, at 0x28 */
};
#define HOST_HEADER_SIZE 48u

_Static_assert(sizeof(struct host_block) == HOST_HEADER_SIZE,
               "mirror header size drifted from kernel/mm/kmalloc.c");

static void report_corrupt(const char *where, int slot, const unsigned char *p,
                           size_t len, size_t at) {
    struct host_block *b = (struct host_block *)(void *)(p - HOST_HEADER_SIZE);
    printf("FUZZ CORRUPTION (%s): slot %d len=%zu byte=%zu value=%02x "
           "expected=%02x\n",
           where, slot, len, at, p[at], (unsigned char)0);
    printf("  block=%p size=%zu free=%d prev=%p next=%p\n",
           (void *)b, b->size, b->is_free, (void *)b->prev, (void *)b->next);
    printf("  payload=%p  header at %p\n", (const void *)p, (void *)b);
    kmalloc_dump();
}

/* Randomised churn with per-byte signatures, which is where coalescing bugs
 * that a tidy sequential pattern would hide show up. */
static unsigned rng_state = 12345;
static unsigned rng_next(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 8) & 0x7fffffffu;
}

#define FUZZ_SLOTS 96

static void fuzz(void) {
    struct {
        unsigned char *ptr;
        size_t len;
        unsigned char tag;
        int live;
    } slots[FUZZ_SLOTS];

    for (int i = 0; i < FUZZ_SLOTS; i++)
        slots[i].live = 0;

    size_t baseline = kmalloc_free_bytes();

    for (int round = 0; round < 20000; round++) {
        int i = (int)(rng_next() % FUZZ_SLOTS);
        if (slots[i].live) {
            for (size_t j = 0; j < slots[i].len; j++) {
                if (slots[i].ptr[j] != slots[i].tag) {
                    report_corrupt("check", i, slots[i].ptr, slots[i].len, j);
                    exit(1);
                }
            }
            kfree(slots[i].ptr);
            slots[i].live = 0;
        } else {
            size_t len = 1 + rng_next() % 9000;
            unsigned char tag = (unsigned char)(rng_next() | 1);
            unsigned char *p = (unsigned char *)kmalloc(len);
            if (p == NULL)
                continue;
            memset(p, tag, len);
            slots[i].ptr = p;
            slots[i].len = len;
            slots[i].tag = tag;
            slots[i].live = 1;

            /* The header must describe this allocation: at least the rounded
             * request, and never so much more that a split was skipped when one
             * was clearly possible. */
            struct host_block *b =
                (struct host_block *)(void *)(p - HOST_HEADER_SIZE);
            size_t need = (len + 15u) & ~(size_t)15u;
            if (b->is_free || b->size < need ||
                b->size >= need + HOST_HEADER_SIZE + 16u) {
                printf("FUZZ BAD HEADER after alloc: slot %d len=%zu "
                       "size=%zu free=%d\n", i, len, b->size, b->is_free);
                kmalloc_dump();
                exit(1);
            }
        }
    }

    for (int i = 0; i < FUZZ_SLOTS; i++) {
        if (slots[i].live) {
            for (size_t j = 0; j < slots[i].len; j++) {
                if (slots[i].ptr[j] != slots[i].tag) {
                    printf("FUZZ CORRUPTION on drain: slot %d byte %zu\n", i, j);
                    exit(1);
                }
            }
            kfree(slots[i].ptr);
        }
    }

    size_t now = kmalloc_free_bytes();
    if (now != baseline) {
        printf("FUZZ LEAK: free was %zu, now %zu (delta %ld)\n",
               baseline, now, (long)now - (long)baseline);
        kmalloc_dump();
        exit(1);
    }
    if (kmalloc_used_bytes() != 0) {
        printf("FUZZ: used bytes is %zu after draining everything\n",
               kmalloc_used_bytes());
        exit(1);
    }
    printf("fuzz: 20000 rounds clean, %zu bytes free and %zu used\n",
           kmalloc_free_bytes(), kmalloc_used_bytes());
}

int main(void) {
    void *blocks[64];
    size_t sizes[64];

    memset(arena, 0, sizeof(arena));
    kmalloc_init();
    kmalloc_attach_region(arena, sizeof(arena));

    size_t free_before = kmalloc_free_bytes();
    printf("free before:            %zu\n", free_before);

    for (size_t i = 0; i < 64; i++) {
        sizes[i] = 1 + i * 37;
        blocks[i] = kmalloc(sizes[i]);
        if (!blocks[i]) {
            printf("allocation %zu failed\n", i);
            return 1;
        }
        memset(blocks[i], (int)(i & 0xff), sizes[i]);
    }
    printf("free after allocs:      %zu (expected %zu)\n",
           kmalloc_free_bytes(), free_before - 74656);

    for (size_t i = 0; i < 64; i++) {
        unsigned char *p = blocks[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (p[j] != (unsigned char)(i & 0xff)) {
                printf("CORRUPTION in block %zu at byte %zu\n", i, j);
                return 1;
            }
        }
    }
    printf("readback ok\n");

    for (size_t i = 0; i < 64; i++)
        kfree(blocks[i]);

    size_t free_after = kmalloc_free_bytes();
    printf("free after frees:       %zu (expected %zu)\n",
           free_after, free_before);
    kmalloc_dump();

    void *big = kmalloc(256 * 1024);
    printf("256 KiB alloc:          %s\n", big ? "ok" : "FAILED");
    if (big)
        kfree(big);

    printf("regions=%u largest=%zu used=%zu\n", kmalloc_region_count(),
           kmalloc_largest_free(), kmalloc_used_bytes());

    fuzz();

    return free_after >= free_before ? 0 : 1;
}