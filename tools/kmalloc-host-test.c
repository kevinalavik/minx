/* Host-side driver for the kernel allocator, so its bookkeeping can be checked
 * without booting QEMU.  Built by tools/test-kmalloc.sh; not shipped in the
 * kernel image.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "kmalloc.h"
#include "spinlock.h"

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

/* The kernel's heap can be grown out of the physical allocator, and
 * kmalloc.c calls into it whether or not anything asks it to.  The host has no
 * physical memory manager, so these two are the whole of the interface the
 * allocator needs from one -- and they say so, rather than quietly handing out
 * memory the fuzz cannot account for. */
int pmm_ready(void) { return 0; }
uint8_t *pmm_alloc_pages(size_t frames) { (void)frames; return NULL; }

/* The block header is the kernel's own, not a mirror of it.
 *
 * The mirror -- a struct written field for field here, with a `_Static_assert` that it had
 * not drifted -- was the right idea and the wrong mechanism.  It caught the header growing
 * only when the test was rebuilt, and it had already been wrong once: it said the header
 * was 48 bytes and knew nothing about the redzone, so the arithmetic that checks an
 * allocation was 16 bytes short and failed a block whose header was exactly right.
 *
 * The test includes `kmalloc.h` and uses `km_block_t`.  There is one definition of the
 * layout now, so there is nothing to drift. */

typedef km_block_t host_block;
#define HOST_HEADER_SIZE KM_HEADER_SIZE


static void report_corrupt(const char *where, int slot, const unsigned char *p,
                           size_t len, size_t at) {
    km_block_t *b = (km_block_t *)(void *)(p - HOST_HEADER_SIZE);
    printf("FUZZ CORRUPTION (%s): slot %d len=%zu byte=%zu value=%02x "
           "expected=%02x\n",
           where, slot, len, at, p[at], (unsigned char)0);
    printf("  block=%p size=%zu free=%d prev=%p next=%p\n",
           (void *)b, b->size, b->free, (void *)b->prev, (void *)b->next);
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

            /* The header must describe this allocation: at least the rounded request
             * *plus the redzone*, and never so much more that a split was skipped when
             * one was clearly possible.
             *
             * The redzone is the whole of what was wrong here.  This computed `need` as
             * the rounded request alone, so it was 16 short of what the allocator
             * promises, and a block of exactly `need + header + align` -- 832 + 48 + 16 =
             * 896 -- failed the "no more than that" test while being precisely right: the
             * allocator declined a split that would have left a zero-length remainder.
             *
             * `KM_REDZONE` and `KM_PATTERN` come from `kmalloc.h` rather than from a
             * second copy here.  That copy is what let the two drift apart, and it drifted
             * the moment a redzone was added to the kernel allocator and not to the
             * arithmetic that checks it. */
            km_block_t *b =
                (km_block_t *)(void *)(p - HOST_HEADER_SIZE);
            size_t need = ((len + (KM_ALIGN - 1u)) & ~(size_t)(KM_ALIGN - 1u)) +
                          KM_REDZONE;
            if (b->free || b->size < need ||
                b->size >= need + HOST_HEADER_SIZE + 16u) {
                printf("FUZZ BAD HEADER after alloc: slot %d len=%zu "
                       "size=%zu free=%d\n", i, len, b->size, b->free);
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

/* --- the kernel's output and its locks, absent from a host harness ------------------
 *
 * `kprintf()` grew a console lock and a "how long have we been silent" check, and this
 * harness links `kprintf.c` directly -- so it needs those symbols even though there is
 * nothing here to lock.
 *
 * The stubs are here rather than `#ifdef`-ed out of `kprintf.c` for two reasons.  The real
 * definitions live in a driver that pulls in the framebuffer, a PS/2 font and a keyboard,
 * none of which link on a host.  And a guarded-out call site is one build flag away from
 * compiling in the wrong place, in the one file every program prints through.
 *
 * A missing symbol the harness supplies is a link error naming the harness.  A missing
 * symbol a macro hides is a kernel that was never built the way anyone reads it.
 *
 * One processor, so every one of these is a no-op.  The one that is *not* a no-op is
 * `panic()`: it must not return, because everything after it assumes the machine stopped. */
static unsigned long harness_now_ms;

uint64_t timer_uptime_ms(void) {
    return harness_now_ms++;
}

void console_lock_init(void) {
}

void console_lock(void) {
}

void console_unlock(void) {
}

void console_write_locked(const char *data, size_t len) {
    fwrite(data, 1, len, stdout);
}

void panic(const char *fmt, ...) {
    va_list ap;

    fputs("panic: ", stderr);

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
    exit(70);            /* the shell's convention for "stopped"; anything non-zero will do */
}

void ticket_lock(ticket_lock_t *l) {
    (void)l;
}

void ticket_unlock(ticket_lock_t *l) {
    (void)l;
}
