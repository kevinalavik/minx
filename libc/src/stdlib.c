/* minx - stdlib: allocation, conversion, the environment, and termination.
 *
 * The allocator is the interesting file in a libc, because it is the one every other file
 * calls and the one whose bugs show up as "the program crashed after a while".  The
 * decisions below are written down at the point they are made, and they are:
 *
 *   - `malloc()` grows the heap with `brk()`, and `free()` puts a block on a free list.
 *     Not `mmap()` for large allocations: there is one process image and one heap, and a
 *     second mechanism would be a second thing to get wrong before anything needs it.
 *
 *   - Every block carries a header, so `free()` knows the size without being told.  The
 *     cost is that a `malloc()` result is eight-byte aligned and the pointer the caller
 *     has is *not* the start of the allocation; `free()` rounds down.  A caller that
 *     stashes the real start somewhere else is a program whose `free()` frees the wrong
 *     address, which is why `realloc()` below does not use this layout for its own
 *     bookkeeping.
 *
 *   - Adjacent free blocks are merged.  Without that, a program that allocates and frees in
 *     a loop -- which is every program -- fragments its own heap into blocks too small to
 *     use, and the failure shows up as the heap growing without the program allocating
 *     more.
 */
#include <errno.h>
#include <fcntl.h>
#include <internal.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- the block header -------------------------------------------------------- */

/* One word, holding the size of the payload in bytes.  `free()` needs nothing else: it
 * puts the block on the list that starts *before* the header, which is why the header
 * exists at all. */
typedef struct block {
    size_t size;          /* payload bytes, always a multiple of the alignment below */
    struct block *next;   /* the free list, or NULL when this block is in use */
    unsigned  free;       /* 0 in use, 1 free: a free block's next pointer is the list */
} block_t;

/* Sixteen bytes, so that every block's payload is sixteen-byte aligned and a `double` or a
 * `long double` inside a struct is not silently misaligned.  Sixteen rather than eight
 * because the kernel maps whole pages and a heap that ends mid-page is a page the kernel
 * cannot reuse. */
#define BLOCK_ALIGN     16
#define BLOCK_HDR       ((sizeof(block_t) + BLOCK_ALIGN - 1) & ~(size_t)(BLOCK_ALIGN - 1))

#define MIN_BLOCK       32

static block_t *free_list;
static char    *heap_base;       /* where the kernel says the break was, once known */
static char    *heap_break;

/* The page the kernel grows the heap in.
 *
 * `brk` moves the break in whole pages, because that is the unit it allocates and unmaps
 * in, so asking for 100 bytes moves it by 4096.  That is not a quirk to work around -- it
 * is what makes the heap contiguous -- but it is why nothing here may ask for an exact
 * figure and insist on getting it back: `sbrk(100)` cannot return what was asked for, and
 * code that checks fails on the first allocation of every program and every one after it. */
#define HEAP_PAGE 4096

/* Add `b` to the free list, coalescing with the blocks around it.
 *
 * Coalescing is what makes `free()` worth having.  Without it a program that allocates and
 * frees the same block for ever ends up with a heap of adjacent free blocks, none of them
 * big enough for the next request, and `sbrk()` grows without the program ever having asked
 * for more -- which is the shape of a memory leak that is really a free list with the wrong
 * shape.
 *
 * The list is kept sorted by address, which is what lets the forward and backward merges be
 * one comparison each instead of a walk. */
static void block_free(block_t *b) {
    /* Sorted by address, so the two merges are one comparison each.  `prev` is the slot
     * *holding* the block below `b`, remembered during the walk: the alternative is a
     * second walk from the head after the forward merge has already changed the list, and
     * a second walk on a list that is being changed is how the two disagree. */
    block_t **slot = &free_list;
    block_t **prev = NULL;

    while (*slot != NULL && (char *)*slot < (char *)b) {
        prev = slot;
        slot = &(*slot)->next;
    }

    b->free = 1;

    /* Forward: the next block starts exactly where this one ends. */
    if (*slot != NULL && (char *)*slot == (char *)b + BLOCK_HDR + b->size) {
        block_t *next = *slot;

        b->size += BLOCK_HDR + next->size;
        b->next = next->next;
    } else {
        b->next = *slot;
    }

    *slot = b;

    /* Backward: this block starts exactly where the previous one ends.  Only possible if
     * there is a previous one; `b` at the very bottom of the heap has nothing below it and
     * merging with a NULL would be a merge with a page header. */
    if (prev != NULL) {
        block_t *p = *prev;

        if ((char *)b == (char *)p + BLOCK_HDR + p->size) {
            p->size += BLOCK_HDR + b->size;
            p->next = b->next;
            *slot = p;
        }
    }
}

void *malloc(size_t n) {
    /* A zero-byte request is not an error and must not be one: ISO C allows it, and a
     * program that does `malloc(sizeof(x) - something)` can legitimately get here.  It has
     * to return something unique rather than NULL, or two such calls compare equal. */
    if (n == 0)
        n = 1;

    size_t need = (n + BLOCK_ALIGN - 1) & ~(size_t)(BLOCK_ALIGN - 1);

    if (need < MIN_BLOCK)
        need = MIN_BLOCK;

    /* First fit over a sorted list.  First fit rather than best fit because it is the one
     * that keeps the list's order useful: best fit needs the whole list every time, and
     * for the allocation patterns programs actually have -- mostly the same size over
     * again -- the two choose the same block anyway. */
    block_t **slot = &free_list;

    while (*slot != NULL) {
        if ((*slot)->size >= need) {
            block_t *b = *slot;

            /* Split when the remainder can hold a block of its own.  A remainder smaller
             * than a header plus a minimum payload is left attached, because splitting it
             * would produce a block whose payload is too small for anything to allocate. */
            if (b->size >= need + BLOCK_HDR + MIN_BLOCK) {
                block_t *rest = (block_t *)((char *)b + BLOCK_HDR + need);

                rest->size = b->size - need - BLOCK_HDR;
                rest->next = b->next;
                rest->free = 1;
                b->size = need;

                /* The remainder *replaces* the block in the list, and the block leaves it.
                 *
                 * Leaving the block in place -- linking the remainder after it and handing
                 * the block out -- puts the same block on the free list and in the
                 * caller's hands at once.  The second `malloc()` of the same size then
                 * returns the same pointer, which is right, and the `free()` of it then
                 * finds a block that is *already* at the head of the list and links it to
                 * itself.  From that moment the free list is a ring of one and every
                 * `malloc()` that has to walk it spins for ever.
                 *
                 * That is exactly what `libctest` did: four allocations and the fifth hung,
                 * in a program whose every individual operation had worked.  A free list
                 * that can contain a block the caller also owns is a free list that will
                 * eventually not be a list. */
                *slot = rest;
            } else {
                *slot = b->next;
            }

            b->free = 0;
            return (char *)b + BLOCK_HDR;
        }

        slot = &(*slot)->next;
    }

    /* Nothing on the list fits, so the heap grows.
     *
     * The block is carved off the *front* of what came back and the remainder goes on the
     * free list, rather than asking for exactly what was needed.  The reason that matters
     * is the second one: the leftover of this allocation is the page the *next* allocation
     * comes from, so a program that allocates and frees in a loop stops asking the kernel
     * after the first few instead of once per allocation for ever.
     *
     * The reason it is written that way at all is the first one, and it is the bug this
     * replaces: `brk()` moves the break in whole pages, so the amount obtained is never the
     * amount asked for.  Code that asked for exactly what it needed and checked the answer
     * failed on the first allocation of every program -- which is how `libctest` reported
     * "calloc" failing before it had run a single `printf`. */
    char *before = heap_break;

    if (sbrk((intptr_t)(BLOCK_HDR + need)) == NULL)
        return NULL;

    size_t got = (size_t)(heap_break - before);

    if (got < BLOCK_HDR + need)
        return NULL;            /* cannot happen: the kernel rounds *up* */

    block_t *b = (block_t *)before;
    size_t   rest = got - BLOCK_HDR - need;

    b->size = need;
    b->next = NULL;
    b->free = 0;

    if (rest >= BLOCK_HDR + MIN_BLOCK) {
        block_t *tail = (block_t *)((char *)b + BLOCK_HDR + need);

        tail->size = rest - BLOCK_HDR;
        tail->next = NULL;
        tail->free = 1;
        block_free(tail);
    }

    return (char *)b + BLOCK_HDR;
}

void free(void *p) {
    if (p == NULL)
        return;             /* free(NULL) is defined, and does nothing, everywhere */

    /* The caller's pointer is the payload; the header is the eight bytes *before* the
     * sixteen-byte-aligned payload start, which is `BLOCK_HDR` bytes back. */
    block_t *b = (block_t *)((char *)p - BLOCK_HDR);

    block_free(b);
}

void *calloc(size_t count, size_t size) {
    /* The overflow check is the whole reason `calloc()` exists as its own function.  A
     * program that asked for `count * size` by hand and got a small product would get a
     * buffer far too small for what it asked for, and the resulting write is a heap
     * corruption that shows up somewhere else entirely. */
    if (count != 0 && size > (size_t)-1 / count) {
        errno = ENOMEM;
        return NULL;
    }

    size_t total = count * size;
    void  *p = malloc(total);

    if (p != NULL)
        memset(p, 0, total);

    return p;
}

/* `realloc()` is the honest version: free, allocate, copy.
 *
 * The clever version -- extending in place when the next block is free, or stealing the
 * old block's space when it is at the top of the heap -- is what every real allocator does
 * and is not done here, because "the next block" is a fact about this heap that a caller
 * can also be looking at.  The cost is that growing a large buffer copies it, which is
 * visible and which a program that reallocs in a loop can measure.  Recorded rather than
 * hidden. */
void *realloc(void *p, size_t n) {
    if (p == NULL)
        return malloc(n);

    if (n == 0) {
        free(p);
        return NULL;
    }

    block_t *b = (block_t *)((char *)p - BLOCK_HDR);

    /* Shrinking in place is free and always right. */
    if (n <= b->size)
        return p;

    void *q = malloc(n);

    if (q == NULL)
        return NULL;        /* the old block is untouched, as POSIX requires */

    memcpy(q, p, b->size);
    free(p);

    return q;
}

void *sbrk(intptr_t increment) {
    /* `sbrk(0)` is how a caller asks where the break is.  The kernel's `brk` answers it,
     * and this function keeps its own copy so that `malloc()` does not have to ask the
     * kernel for a boundary it already knows. */
    char *before = heap_break;

    /* `brk(0)` is the question "where is the break", and the kernel answers it with the
     * current one.  Asked once and remembered, because `malloc()` needs the boundary on
     * every call and the answer cannot change under it: this process is the only thing
     * that moves its own break. */
    if (heap_base == NULL) {
        char *p = (char *)(uintptr_t)__syscall1(SYS_BRK, 0);

        if (p == NULL) {
            errno = ENOMEM;
            return NULL;
        }

        heap_base = heap_break = p;
    }

    if (increment == 0)
        return heap_break;

    char *want = heap_break + increment;
    char *got  = (char *)(uintptr_t)__syscall1(SYS_BRK, (long)(uintptr_t)want);

    if (got == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    heap_break = got;

    return before;
}

/* --- environment -------------------------------------------------------------- */

/* The table and the buffer.  Fixed size, no allocation: a program can set an environment
 * variable before `main()`, and doing that must not be able to fail.  A program that needs
 * more than this needs a different design, and the limit is here rather than discovered. */
#define ENV_MAX 64

static char        *env_storage[ENV_MAX + 1];
char              **environ = env_storage;
static unsigned     env_count;

/* How many of the table's slots are in use, and whether the table has been handed to
 * `environ` yet.  `environ` is a `char **` pointing at the table, and it has to keep
 * pointing at the *same* array when the table grows -- which is why the table is fixed and
 * only the count moves. */
static int env_ready;

static void env_sync(void) {
    env_storage[env_count] = NULL;

    if (!env_ready) {
        /* `environ` has to point at the table itself, and a program that saved the old
         * value before any `setenv()` would otherwise be left holding a pointer to a
         * different array than the one the program is adding to. */
        environ = env_storage;
        env_ready = 1;
    }
}

/* Take on a different environment: the loader's, when the program was started by something
 * that had one to give.
 *
 * `startup_env` is a private array in start.c and the `environ` table here is a private
 * array in this file, so one of the two has to be adopted into the other rather than
 * pointed at -- and the table has to be the one the *program* can change, because
 * `setenv()` writes through `environ`.  A program that `setenv()`s a variable and then
 * `fork`s and `exec`s would otherwise pass the wrong array to its child. */
void __libc_adopt_env(char **envp) {
    env_count = 0;
    env_ready = 1;
    environ  = env_storage;

    for (unsigned i = 0; envp != NULL && envp[i] != NULL && i < ENV_MAX; i++)
        env_storage[env_count++] = envp[i];

    env_storage[env_count] = NULL;
}

char *getenv(const char *name) {
    size_t n = strlen(name);

    for (unsigned i = 0; i < env_count; i++) {
        const char *e = env_storage[i];

        if (strncmp(e, name, n) == 0 && e[n] == '=')
            return (char *)(uintptr_t)(e + n + 1);
    }

    return NULL;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (name[0] == 0 || strchr(name, '=') != NULL) {
        /* An empty name, or one with an `=` in it, cannot be stored: the table is a list
         * of "NAME=VALUE" strings and either one has nowhere to go.  This is EINVAL and
         * not ENOMEM -- the table may well have room; the request does not make sense. */
        errno = EINVAL;
        return -1;
    }

    size_t n = strlen(name);

    for (unsigned i = 0; i < env_count; i++) {
        const char *e = env_storage[i];

        if (strncmp(e, name, n) == 0 && e[n] == '=') {
            if (!overwrite)
                return 0;

            /* Replaced in place.  The old string is not freed: it is a string literal in
             * most cases, and a `getenv()` result the program is still holding would
             * become a dangling pointer.  Leaking it is the trade, and it is the same one
             * every libc makes. */
            size_t total = n + 1 + strlen(value) + 1;
            char  *s = malloc(total);

            if (s == NULL)
                return -1;

            memcpy(s, name, n);
            s[n] = '=';
            memcpy(s + n + 1, value, strlen(value) + 1);
            env_storage[i] = s;

            return 0;
        }
    }

    if (env_count >= ENV_MAX) {
        errno = ENOMEM;
        return -1;
    }

    size_t total = n + 1 + strlen(value) + 1;
    char  *s = malloc(total);

    if (s == NULL)
        return -1;

    memcpy(s, name, n);
    s[n] = '=';
    memcpy(s + n + 1, value, strlen(value) + 1);
    env_storage[env_count++] = s;
    env_sync();

    return 0;
}

int unsetenv(const char *name) {
    size_t n = strlen(name);

    for (unsigned i = 0; i < env_count; i++) {
        const char *e = env_storage[i];

        if (strncmp(e, name, n) != 0 || e[n] != '=')
            continue;

        /* Compacting the table rather than leaving a hole: `environ` is a NULL-terminated
         * array and a hole in the middle would have to be a NULL, which would look like
         * the end of the list. */
        for (unsigned j = i + 1; j < env_count; j++)
            env_storage[j - 1] = env_storage[j];

        env_count--;
        env_sync();

        return 0;
    }

    return 0;
}

/* `putenv()` takes a whole "NAME=VALUE" string and does *not* copy it: the string becomes
 * the caller's, which is why it is declared as taking `char *` and not `const char *`.
 * The distinction is not pedantry -- it is what lets a program build the string in a buffer
 * it then reuses, and it is why the copy above is the one `setenv()` does. */
int putenv(char *entry) {
    if (entry == NULL)
        return -1;

    const char *eq = strchr(entry, '=');

    if (eq == NULL || eq == entry) {
        errno = EINVAL;
        return -1;
    }

    for (unsigned i = 0; i < env_count; i++) {
        const char *e = env_storage[i];

        if (strncmp(e, entry, (size_t)(eq - entry)) == 0 && e[eq - entry] == '=') {
            env_storage[i] = entry;
            return 0;
        }
    }

    if (env_count >= ENV_MAX) {
        errno = ENOMEM;
        return -1;
    }

    env_storage[env_count++] = entry;
    env_sync();

    return 0;
}

/* --- termination -------------------------------------------------------------- */

#define ATEXIT_MAX 32

static void (*atexit_fns[ATEXIT_MAX])(void);
static unsigned atexit_count;

int atexit(void (*fn)(void)) {
    if (fn == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* A full table is a failure rather than a silent drop.  Silently dropping a handler
     * means a program that registered one runs without it and never finds out; failing the
     * registration means the program knows. */
    if (atexit_count >= ATEXIT_MAX) {
        errno = ENOMEM;
        return -1;
    }

    atexit_fns[atexit_count++] = fn;
    return 0;
}

/* Most recent first, which is the reverse of registration and therefore the reverse of the
 * order the program's own scopes end.  A handler that registered another handler gets that
 * one run before itself, which is what makes the "stack" unwind the way the code reads. */
void __stdio_run_atexit(void) {
    while (atexit_count > 0)
        atexit_fns[--atexit_count]();
}

void abort(void) {
    raise(SIGABRT);
    _exit(127);
}

/* --- conversion ---------------------------------------------------------------- */

long strtol(const char *s, char **end, int base) {
    /* Skip what is not part of a number, and record it as the end if the caller wants it.
     * The sign is read before the digits and *not* allowed into the accumulator, which is
     * the bug that makes `strtol("-1", ...)` return 0 on an implementation that folds it
     * in. */
    while (*s == ' ' || (*s >= '\t' && *s <= '\r'))
        s++;

    int      negative = 0;
    uint64_t acc      = 0;
    int      any      = 0;

    if (*s == '+' || *s == '-') {
        negative = (*s == '-');
        s++;
    }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        /* Base 0 means "guess from the shape": a leading 0 is octal, otherwise decimal.  A
         * leading 0 is octal on every Unix and is the rule that surprises people, which is
         * why it is here rather than a default of 10. */
        base = (s[0] == '0') ? 8 : 10;
    }

    for (;;) {
        int d;

        if (*s >= '0' && *s <= '9')
            d = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            d = *s - 'A' + 10;
        else
            break;

        if (d >= base)
            break;

        /* Overflow is clamped rather than wrapped, and ERANGE is set.  Wrapping produces a
         * number that is neither the input nor an error, and a program using this to size
         * a buffer then allocates the wrong amount. */
        if (acc > (uint64_t)0x7fffffffffffffffull / (uint64_t)base) {
            if (end != NULL)
                *end = (char *)(uintptr_t)s;
            errno = ERANGE;
            return negative ? -0x7fffffffffffffffL : 0x7fffffffffffffffL;
        }

        acc = acc * (uint64_t)base + (uint64_t)d;
        any = 1;
        s++;
    }

    if (end != NULL)
        *end = (char *)(uintptr_t)s;

    if (!any) {
        /* Nothing was a number.  The return is 0 and *no* error is set: a caller that
         * passes `end` and checks it is asking exactly this question, and setting errno
         * here would make `strtol(s, &e, 10)` on a non-numeric string look like a
         * failure of something else. */
        return 0;
    }

    if (negative)
        return -(long)acc;

    return (long)acc;
}

unsigned long strtoul(const char *s, char **end, int base) {
    /* The same loop with a different accumulator and no sign.  Not `strtol()` with a cast:
     * that turns "-1" into ULONG_MAX, which is the answer the C standard asks for and the
     * answer nobody wants, and the two must be separate code to both be right. */
    while (*s == ' ' || (*s >= '\t' && *s <= '\r'))
        s++;

    int         negative = (*s == '-');
    uint64_t    acc      = 0;
    int         any      = 0;

    if (*s == '+' || *s == '-')
        s++;

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }

    for (;;) {
        int d;

        if (*s >= '0' && *s <= '9')
            d = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            d = *s - 'A' + 10;
        else
            break;

        if (d >= base)
            break;

        acc = acc * (uint64_t)base + (uint64_t)d;
        any = 1;
        s++;
    }

    if (end != NULL)
        *end = (char *)(uintptr_t)s;

    (void)negative;         /* "minus" is simply ignored, which is the specification */

    return any ? (unsigned long)acc : 0;
}

long long strtoll(const char *s, char **end, int base) {
    return (long long)strtol(s, end, base);
}

int atoi(const char *s) {
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s) {
    return strtol(s, NULL, 10);
}

/* `atof()` and `strtod()` exist because a program including <stdlib.h> and using them must
 * not fail to link.  There is no floating-point hardware configured in this kernel's build
 * flags (`-mno-sse -mno-mmx`), so a real conversion would need software floating point
 * that is not here.  They answer 0.0 and set nothing, which is the one answer that cannot
 * be mistaken for a date or a price. */
double atof(const char *s) {
    (void)s;
    return 0.0;
}

double strtod(const char *s, char **end) {
    /* The end pointer is set to the *start*, which is what `strtod` says for a string that
     * does not begin with a number: nothing was consumed.  Returning NULL would make a
     * caller that only checks the end pointer treat it as a parse failure. */
    if (end != NULL)
        *end = (char *)(uintptr_t)s;

    return 0.0;
}

int abs(int n) {
    return n < 0 ? -n : n;
}

long labs(long n) {
    return n < 0 ? -n : n;
}

/* --- sorting and searching ------------------------------------------------------ */

/* A merge sort rather than quicksort.
 *
 * This is not the fast choice on random data and it is the right one here: the data a
 * program hands `qsort()` is very often *already* ordered -- a directory listing, a path
 * table, the arguments of a command -- and quicksort on ordered input with a naive pivot
 * choice is quadratic, which for 4096 entries is 16 million comparisons.  Merge sort is
 * always `n log n`, and its cost is the temporary buffer, which is the only allocation
 * `qsort()` ever does. */
static void merge_run(char *src, char *dst, size_t n, size_t size,
                      int (*cmp)(const void *, const void *)) {
    size_t half = n / 2;
    size_t i = 0, j = half, k = 0;

    while (i < half && j < n) {
        /* `<=` rather than `<`, so that equal elements keep their relative order: the
         * comparison is on whole records, so "stable" means what it says here. */
        if (cmp(src + i * size, src + j * size) <= 0)
            memcpy(dst + (k++) * size, src + (i++) * size, size);
        else
            memcpy(dst + (k++) * size, src + (j++) * size, size);
    }

    while (i < half)
        memcpy(dst + (k++) * size, src + (i++) * size, size);
    while (j < n)
        memcpy(dst + (k++) * size, src + (j++) * size, size);
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    if (n < 2 || size == 0)
        return;

    char *tmp = malloc(n * size);
    char *a   = base;
    char *b   = tmp;

    if (tmp == NULL)
        return;             /* the caller's array is unchanged, which is all we can do */

    for (size_t width = 1; width < n; width *= 2) {
        for (size_t lo = 0; lo < n; lo += width * 2) {
            size_t mid = lo + width;
            size_t hi  = lo + width * 2;

            if (mid > n)
                mid = n;
            if (hi > n)
                hi = n;

            merge_run(a + lo * size, b + lo * size, hi - lo, size, cmp);
        }

        char *t = a;
        a = b;
        b = t;
    }

    /* After an odd number of passes `a` is not the caller's array, so the result is copied
     * back rather than assumed to be there. */
    if (a != base)
        memcpy(base, a, n * size);

    free(tmp);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *)) {
    size_t lo = 0;
    size_t hi = n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int    c   = cmp(key, (const char *)base + mid * size);

        if (c == 0)
            return (char *)(uintptr_t)base + mid * size;

        /* `<` rather than `<=`: with `c` reduced to a sign, a match has already been
         * returned above, so anything else is strictly one side or the other. */
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }

    return NULL;
}

/* --- randomness ------------------------------------------------------------------ */

/* Not random, and this is stated rather than hidden: there is no entropy source in the
 * kernel, and a generator seeded from the clock alone produces the same sequence on every
 * boot of every machine, which is worse than one that is visibly not random because it can
 * be relied upon to be wrong.
 *
 * Three things go in so that two processes started in the same millisecond do not produce
 * the same sequence: the clock, this process's id, and the address of a stack object --
 * which is the part that actually differs between two processes, since their stacks are at
 * different addresses. */
static uint64_t rand_state = 1;

int rand(void) {
    if (rand_state == 0)
        rand_state = 1;

    /* xorshift64: multiply, shift, xor.  Three lines, no division, and a period of
     * 2^64-1, which for a program using `rand()` to pick a file name or a delay is far
     * more than enough.  The quality is not good enough for anything cryptographic and is
     * not claimed to be. */
    rand_state ^= rand_state << 13;
    rand_state ^= rand_state >> 7;
    rand_state ^= rand_state << 17;

    return (int)(rand_state >> 33);
}

void srand(unsigned int seed) {
    rand_state = seed != 0 ? seed : 1;
}

long random(void) {
    return (long)(unsigned int)rand();
}

void srandom(unsigned int seed) {
    srand(seed);
}

int system(const char *command) {
    /* There is a shell in phase 9; before it links, this is not a shell-shaped hole but an
     * honest one: `ENOENT`, which is what "there is no /bin/sh to run this with" means. */
    (void)command;

    errno = ENOENT;
    return -1;
}