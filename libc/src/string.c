/* minx - the string functions.
 *
 * Written one at a time rather than generated, for the same reason the kernel's are: the
 * interesting part of `strchr()` is not the loop, it is what happens when the character is
 * the NUL, and that is a thing worth being able to read.  A table of near-identical
 * bodies is where that gets lost.
 *
 * Two decisions that apply to all of them:
 *
 *   - The compiler is told `-ffreestanding`, so it does not turn a `memcpy()` in a loop
 *     into a call to `memcpy()` that calls itself.  Everything below is therefore a real
 *     definition and the optimisation is the caller's business, not this file's.
 *
 *   - Nothing here checks its arguments.  A NULL pointer is a bug in the caller and every
 *     other libc crashes on one too; what they differ on is crashing *silently*, and
 *     there is nothing a check here could say that would help.
 */

/* `size_t` is unsigned, so "i < n" with `i` an int would be a comparison between a signed
 * and an unsigned value, and `n` would be promoted to something enormous.  Every loop
 * below counts with a `size_t` for that reason. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char       *d = dst;
    const unsigned char *s = src;

    /* Word-at-a-time while both sides agree, which they do when they are the same
     * pointer -- `memmove(x, x, n)` has to copy correctly -- and do not once the ranges
     * overlap in either direction.  The overlap test is `d < s+n` as well as `s < d+n`,
     * because an overlap is symmetric: copying backwards when the destination is lower is
     * just the wrong way round.
     *
     * The alignment test is on both, deliberately.  An aligned load and a misaligned store
     * is exactly what x86 is fastest at, so requiring only the load to be aligned would
     * throw that away for no reason. */
    if (n >= 16 && (((uintptr_t)d | (uintptr_t)s) & 7) == 0 &&
        (d < s + n ? s < d + n : d < s + n)) {
        uint64_t       *dw = (uint64_t *)(void *)d;
        const uint64_t *sw = (const uint64_t *)(const void *)s;

        while (n >= 32) {
            dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2]; dw[3] = sw[3];
            dw += 4; sw += 4; n -= 32;
        }
        while (n >= 8) {
            *dw++ = *sw++;
            n -= 8;
        }

        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }

    while (n-- > 0)
        *d++ = *s++;

    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char       *d = dst;
    const unsigned char *s = src;

    /* Overlapping is not an error here, it is the whole point: the direction is chosen so
     * that no byte is written before it has been read.  Destination above the source
     * copies backwards; otherwise forwards.  Equal pointers need no work at all. */
    if (d == s || n == 0)
        return dst;

    if (d > s) {
        d += n;
        s += n;

        while (n-- > 0)
            *--d = *--s;

        return dst;
    }

    while (n-- > 0)
        *d++ = *s++;

    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    unsigned char  v = (unsigned char)c;

    /* The same shape as `memcpy()`: widen to the machine word once both the value and the
     * address allow it, because a byte-at-a-time `memset()` is the inner loop of half the
     * programs a libc is asked to run. */
    if (n >= 16 && ((uintptr_t)d & 7) == 0) {
        uint64_t pattern = (uint64_t)v * 0x0101010101010101ull;
        uint64_t *dw = (uint64_t *)(void *)d;

        while (n >= 32) {
            dw[0] = pattern; dw[1] = pattern; dw[2] = pattern; dw[3] = pattern;
            dw += 4; n -= 32;
        }
        while (n >= 8) {
            *dw++ = pattern;
            n -= 8;
        }

        d = (unsigned char *)dw;
    }

    while (n-- > 0)
        *d++ = v;

    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a;
    const unsigned char *q = b;

    /* Compared as unsigned bytes.  As signed ones, "a byte 0x80" sorts *before* "a space"
     * on a machine where `char` is signed, and every comparison of binary data -- and of
     * UTF-8 -- silently depends on which way the compiler went. */
    while (n-- > 0) {
        if (*p != *q)
            return (int)*p - (int)*q;
        p++;
        q++;
    }

    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    unsigned char        v = (unsigned char)c;

    while (n-- > 0) {
        if (*p == v)
            return (void *)(uintptr_t)p;
        p++;
    }

    return NULL;
}

size_t strlen(const char *s) {
    const char *p = s;

    /* Get to an eight-byte boundary one byte at a time, and a string that *ends* before
     * the boundary is found here rather than in the word loop below.
     *
     * That case is the one that matters: `strlen("octal")` at an address that puts the
     * NUL before the next eight-byte boundary has to answer 5, and returning 0 there made
     * `printf("%s", ...)` print nothing for every string shorter than the alignment gap.
     * libctest printed "libctest: ok -- " with the explanation missing, for exactly those
     * strings whose length happened to end early, and passed for the rest -- which is the
     * worst shape a string bug can have: right most of the time, on data you did not
     * choose. */
    while (((uintptr_t)p & 7) != 0) {
        if (*p == 0)
            return (size_t)(p - s);
        p++;
    }

    for (;;) {
        uint64_t w = *(const uint64_t *)(const void *)p;

        /* The classic: a zero byte within a little-endian word has one of its bytes clear.
         * `haszero` puts that as a bit in the position of the byte, so one subtraction
         * per word answers "is there a NUL in here" for eight bytes at a time. */
        if ((w - 0x0101010101010101ull) & ~w & 0x8080808080808080ull) {
            while (*p != 0)
                p++;
            return (size_t)(p - s);
        }

        p += 8;
    }
}

size_t strnlen(const char *s, size_t max) {
    size_t n = 0;

    while (n < max && s[n] != 0)
        n++;

    return n;
}

char *strcpy(char *dst, const char *src) {
    char       *d = dst;
    const char *s = src;

    /* Word-at-a-time, and the subtlety is the overlap: `strcpy` is allowed to copy
     * forwards over its own source when the ranges touch, which is what the first word
     * store may already have done.  A version that reads all of a word before writing any
     * of it -- as `memcpy()` above does -- is the one that is safe, and this one is too
     * because each word is loaded, then stored, and the next load is from *after* it. */
    while (((uintptr_t)d & 7) != 0 || ((uintptr_t)s & 7) != 0) {
        if ((*d = *s) == 0)
            return dst;
        d++;
        s++;
    }

    for (;;) {
        uint64_t w;

        memcpy(&w, s, sizeof(w));

        /* Copy the word, then find out where the NUL in it was, so that the terminator
         * and everything after it lands too.  `w & 0x80...` marks candidate bytes; the
         * zero-byte test narrows it to the real one. */
        memcpy(d, &w, sizeof(w));
        d += 8;
        s += 8;

        if ((w - 0x0101010101010101ull) & ~w & 0x8080808080808080ull)
            return dst;
    }
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;

    /* Not `strlcpy`: this is the function whose whole point is that a short source is
     * *padded* with NULs rather than terminated early, so `dst[n]` is always writable
     * memory for the caller and the result is always NUL-terminated when n > 0. */
    for (; i < n && src[i] != 0; i++)
        dst[i] = src[i];

    for (; i < n; i++)
        dst[i] = 0;

    return dst;
}

char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i = 0;

    for (; i < n && src[i] != 0; i++)
        d[i] = src[i];

    d[i] = 0;
    return dst;
}

int strcmp(const char *a, const char *b) {
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;

    /* Unsigned, again: `strcmp` is specified as comparing `unsigned char`, and a
     * comparison that went the other way makes `strcmp` disagree with `memcmp` on the same
     * bytes. */
    while (*p != 0 && *p == *q) {
        p++;
        q++;
    }

    return (int)*p - (int)*q;
}

int strncmp(const char *a, const char *b, size_t n) {
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;

    for (; n > 0; n--) {
        if (*p != *q)
            return (int)*p - (int)*q;
        if (*p == 0)
            return 0;
        p++;
        q++;
    }

    return 0;
}

char *strchr(const char *s, int c) {
    char v = (char)c;

    /* `strchr(s, '\0')` returns a pointer to the terminator, not NULL.  That is the rule
     * every implementation gets wrong at least once, and a program that walks to the end
     * of a string with it depends on the answer being non-NULL. */
    for (;; s++) {
        if (*s == v)
            return (char *)(uintptr_t)s;
        if (*s == 0)
            return NULL;
    }
}

char *strrchr(const char *s, int c) {
    char v = (char)c;
    const char *last = NULL;

    for (;; s++) {
        if (*s == v)
            last = s;
        if (*s == 0)
            break;
    }

    return (char *)(uintptr_t)last;
}

char *strstr(const char *hay, const char *needle) {
    /* The empty needle matches at the start, which is what makes
     * `strstr(s, "")` and `strncmp`-style prefix tests agree. */
    if (needle[0] == 0)
        return (char *)(uintptr_t)hay;

    for (; *hay != 0; hay++) {
        const char *h = hay;
        const char *n = needle;

        while (*n != 0 && *h == *n) {
            h++;
            n++;
        }

        if (*n == 0)
            return (char *)(uintptr_t)hay;
    }

    return NULL;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s != 0; s++)
        if (strchr(accept, *s) != NULL)
            return (char *)(uintptr_t)s;

    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;

    for (; s[n] != 0 && strchr(accept, s[n]) != NULL; n++)
        ;

    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;

    for (; s[n] != 0 && strchr(reject, s[n]) == NULL; n++)
        ;

    return n;
}

/* `strtok()` is the one function in <string.h> that has state, and the state is why
 * `strtok_r()` exists.  A program with two parsers in it -- a shell lexing a line while a
 * library parses a word out of it -- cannot use `strtok()` for both, and the bug is not a
 * crash but words quietly coming out in the wrong order.
 *
 * The reentrant form keeps the state in the caller, which is the only place two independent
 * parsers can both reach it. */
static char *strtok_save;

char *strtok_r(char *s, const char *delim, char **save) {
    char *start;

    if (s == NULL)
        s = strtok_save;

    if (s == NULL)
        return NULL;

    /* Leading delimiters are not a token.  A caller that has just split on spaces and
     * gets four spaces gets four empty tokens otherwise, and every loop over them has to
     * know about it. */
    s += strspn(s, delim);

    if (*s == 0) {
        strtok_save = NULL;
        return NULL;
    }

    start = s;
    s += strcspn(s, delim);

    if (*s != 0) {
        *s = 0;
        strtok_save = s + 1;
    } else {
        /* The last token ended at the terminator, so there is no state to keep: a second
         * call with NULL must return NULL, not the empty string that starts here. */
        strtok_save = NULL;
    }

    return start;
}

char *strtok(char *s, const char *delim) {
    return strtok_r(s, delim, &strtok_save);
}

/* --- the old BSD spellings --------------------------------------------------- */

void bzero(void *s, size_t n) {
    memset(s, 0, n);
}

int bcmp(const void *a, const void *b, size_t n) {
    return memcmp(a, b, n);
}

void bcopy(const void *src, void *dst, size_t n) {
    memmove(dst, src, n);
}

int ffs(int i) {
    int n = 1;

    if (i == 0)
        return 0;

    /* On negatives the search starts at the *top*: `ffs` is specified to find the lowest
     * set bit of the value's own width, and for a negative number that is the bit just
     * below the sign, not bit 0. */
    if (i < 0) {
        i = ~i;

        while ((i & 0x40000000) == 0) {
            i <<= 1;
            n++;
        }
        return n;
    }

    while ((i & 1) == 0) {
        i >>= 1;
        n++;
    }

    return n;
}

int strcasecmp(const char *a, const char *b) {
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;

    /* Folded with ASCII: letters to one case, everything else to itself.  Not `tolower()`,
     * which is locale-dependent and which a freestanding libc has no locale for. */
    for (;;) {
        unsigned char x = *p++;
        unsigned char y = *q++;

        if (x >= 'A' && x <= 'Z')
            x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = (unsigned char)(y - 'A' + 'a');

        if (x != y)
            return (int)x - (int)y;
        if (x == 0)
            return 0;
    }
}

int strncasecmp(const char *a, const char *b, size_t n) {
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;

    while (n-- > 0) {
        unsigned char x = *p++;
        unsigned char y = *q++;

        if (x >= 'A' && x <= 'Z')
            x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = (unsigned char)(y - 'A' + 'a');

        if (x != y)
            return (int)x - (int)y;
        if (x == 0)
            return 0;
    }

    return 0;
}

/* --- the allocating ones ------------------------------------------------------- */

/* `strdup()` and `strndup()` are here rather than in stdlib.c because they are string
 * functions that happen to allocate, and a program that includes <string.h> for them gets
 * them from where it expects, with `free()` from <stdlib.h>. */
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char  *p = malloc(n);

    if (p != NULL)
        memcpy(p, s, n);

    return p;
}

/* Bounded, and bounded in the way that matters: `strndup()` copies at most `n` bytes *and*
 * terminates, so the result is a string whatever `s` is.  An implementation that copies `n`
 * bytes without terminating leaves the result's last byte as whatever followed the source
 * in memory, which for a source at the end of a page is a fault. */
char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char  *p   = malloc(len + 1);

    if (p != NULL) {
        memcpy(p, s, len);
        p[len] = 0;
    }

    return p;
}
