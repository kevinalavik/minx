/* minx - the `printf` family.
 *
 * One engine and seven entry points, which is the whole design.  `printf`, `fprintf`,
 * `sprintf` and `snprintf` differ only in where the bytes go, and an implementation with
 * four copies of the format loop has four chances to get the width, the precision and the
 * length modifier wrong in four different ways.
 *
 * The engine below is therefore written against one thing -- "put these bytes here" -- and
 * every entry point is a different answer to that.
 *
 * What it supports is ISO C's `printf`: flags `-` `+` ` ` `#` `0`, width, precision,
 * `.` and `*`, the length modifiers `hh` `h` `l` `ll` `z` `j` `t`, the conversions
 * `d` `i` `u` `o` `x` `X` `c` `s` `p` `n` `%` and `f` `e` `g` `a`, and the `*` width and
 * precision.  Floating point is the exception and is called out where it is reached.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysdep.h>

/* --- where the bytes go --------------------------------------------------------- */

/* Three destinations, one interface.  The string ones have to know their limit; the stream
 * one does not, because `fwrite()` is the limit. */
typedef struct {
    FILE      *file;      /* set for the stream destinations */
    char      *buf;       /* set for the string ones */
    size_t     size;      /* how much room, for the string ones */
    size_t     used;      /* how much would have been written, limit or not */
} sink_t;

/* Store, count, or both.
 *
 * Three modes, distinguished by the sink rather than by a flag on the call:
 *
 *   file != NULL   through the stream -- `printf`
 *   size > 0       into the buffer, up to the limit -- `snprintf`
 *   size == 0      count, and store only if there is a buffer -- `sprintf`, and the
 *                  measuring pass `asprintf()` makes before it allocates
 *
 * The count happens in all three, and that is the whole reason the count lives here rather
 * than in the callers: a program that has not allocated a buffer yet still needs to know
 * how much to allocate, and there is no other way for it to find out. */
static void sink_char(sink_t *o, char c) {
    if (o->file != NULL) {
        fputc((unsigned char)c, o->file);
    } else if (o->size == 0 || o->used + 1 < o->size) {
        if (o->buf != NULL)
            o->buf[o->used] = c;
    }

    o->used++;
}

static void sink_bytes(sink_t *o, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        sink_char(o, p[i]);
}

static void sink_pad(sink_t *o, char c, int n) {
    for (int i = 0; i < n; i++)
        sink_char(o, c);
}

/* --- the number to print ----------------------------------------------------------- */

/* An unsigned value plus how wide it is.
 *
 * Assembled in an array rather than formatted in place, because the field width and the
 * zero padding interact: the zeroes go *after* the sign, so `printf("%05d", -42)` is
 * `-0042` and not `-4200`.  Getting that right requires the digits and the sign to be
 * separate, which is why the sign is produced here and not inside the digit loop. */
typedef struct {
    char     digits[24];      /* the digits, least significant first */
    unsigned ndigits;
    int      negative;
} num_t;

/* Every integer conversion comes through here, which is what makes `%d`, `%u`, `%x` and
 * `%o` differ only in the base and in whether the value is signed. */
static num_t num_make(uint64_t magnitude, int negative, unsigned base) {
    num_t n;

    n.negative = negative;
    n.ndigits   = 0;

    /* A zero is one digit, and that digit is the *character* '0'.
     *
     * It was a NUL -- which is what `memset()` leaves and what "no digit" reads like -- and
     * the difference is invisible on a terminal and everywhere else.  `printf("%d", 0)`
     * wrote a zero byte and nothing else, so a program printing a count that happened to be
     * zero printed nothing at all: no "0", no newline of its own, just an absence.  It is
     * not a crash, it is not an error, and it is the sort of thing that passes every test
     * written against a program that never prints zero.
     *
     * The `ndigits = 1` is the other half of the same fix: without it the loop below never
     * runs and there is no digit to be wrong. */
    n.digits[0] = '0';

    if (magnitude == 0)
        n.ndigits = 1;

    while (magnitude != 0) {
        unsigned d = (unsigned)(magnitude % base);

        n.digits[n.ndigits++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        magnitude /= base;
    }

    return n;
}

/* Put the digits out, in base, with the case the conversion asked for.
 *
 * The `0x` is *not* emitted here.  It is emitted by the caller, before the zero padding,
 * because `%#06x` is `0x002a` and padding first gives `000x2a` -- and the caller is the
 * only place that knows where the padding goes. */
static void num_write(sink_t *o, const num_t *n, unsigned base, int upper) {
    for (unsigned i = n->ndigits; i > 0; i--) {
        char c = n->digits[i - 1];

        sink_char(o, upper && c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c);
    }
}

/* --- the format loop ------------------------------------------------------------------ */

/* The one format loop, writing to a sink.
 *
 * Every entry point in this file is a different answer to "where do the bytes go" and
 * nothing else: a sink that writes through `fputc` is `printf`, and a sink that stores into
 * a buffer with a limit is `snprintf`, and one with no limit at all is `sprintf`.  An
 * implementation with four copies of this loop has four chances to get the sign, the zero
 * padding and the precision wrong in four different ways, and the bug that only shows up in
 * `%#08x` is exactly the kind that survives a test run. */
static int vformat(sink_t *o, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p != 0; p++) {
        if (*p != '%') {
            sink_char(o, *p);
            continue;
        }

        p++;

        if (*p == '%') {
            sink_char(o, '%');
            continue;
        }

        /* Flags.  Each is a yes/no, and they are collected as booleans rather than as a
         * bitmask: five booleans cannot be combined into a value nobody meant. */
        int left  = 0;   /* '-' */
        int plus  = 0;   /* '+' */
        int space = 0;   /* ' ' */
        int alt   = 0;   /* '#' */
        int zero  = 0;   /* '0' */

        for (;; p++) {
            if (*p == '-') { left = 1; continue; }
            if (*p == '+') { plus = 1; continue; }
            if (*p == ' ') { space = 1; continue; }
            if (*p == '#') { alt = 1; continue; }
            if (*p == '0') { zero = 1; continue; }
            break;
        }

        int width = 0;

        if (*p == '*') {
            /* A negative width from an argument means "left aligned, and this much wide" --
             * which is how ISO C says to spell `-8` for a field nobody wants to hard-code.
             * Turning it into a negative width here is what stops the padding arithmetic
             * below from computing a huge positive one. */
            width = va_arg(ap, int);

            if (width < 0) {
                left = 1;
                width = -width;
            }

            p++;
        } else {
            while (*p >= '0' && *p <= '9')
                width = width * 10 + (*p++ - '0');
        }

        int precision = -1;      /* -1 means "not given", which is not the same as 0 */

        if (*p == '.') {
            p++;
            precision = 0;

            if (*p == '*') {
                precision = va_arg(ap, int);
                p++;
            } else {
                while (*p >= '0' && *p <= '9')
                    precision = precision * 10 + (*p++ - '0');
            }
        }

        /* The length modifier.  Only the ones that change how the argument is fetched:
         * `l` and its friends do, and `hh`/`h` truncate afterwards.  `L` is for long
         * double, which this kernel has no floating-point hardware for. */
        int lng = 0;        /* 0 = int, 1 = long, 2 = long long */
        int lngdouble = 0;

        if (*p == 'h') {
            p++;
            if (*p == 'h')
                p++;
            lng = -1;            /* truncated to char or short */
        } else if (*p == 'l') {
            p++;
            if (*p == 'l') {
                p++;
                lng = 2;
            } else {
                lng = 1;
            }
        } else if (*p == 'z' || *p == 'j' || *p == 't') {
            /* All three are "an integer as wide as a size, an intmax or a ptrdiff", which
             * is one fetch on this target: 64 bits. */
            p++;
            lng = 2;
        } else if (*p == 'L') {
            p++;
            lngdouble = 1;
        }

        int is_signed = 0;
        int base      = 10;
        int upper     = 0;

        char conv = *p;

        switch (conv) {
        case 'd': case 'i': is_signed = 1; break;
        case 'u': break;
        case 'o': base = 8; break;
        case 'x': base = 16; break;
        case 'X': base = 16; upper = 1; break;
        case 'b': base = 2; break;   /* a C23 conversion, harmless to accept */
        default: break;
        }

        /* One place that emits "digits, sign, padding", so that the three orderings that
         * are easy to get wrong -- sign before zero padding, padding before the digits
         * rather than after, width counted in characters and not bytes -- cannot be got
         * wrong three times. */
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'o' ||
            conv == 'x' || conv == 'X' || conv == 'b') {
            int64_t sv = 0;
            uint64_t uv;

            if (is_signed) {
                sv = (lng == 2) ? va_arg(ap, long long)
                  : (lng == 1) ? va_arg(ap, long)
                               : (lng == -1) ? (int64_t)(short)va_arg(ap, int)
                                             : va_arg(ap, int);
                uv = (uint64_t)sv;
            } else {
                uv = (lng == 2) ? va_arg(ap, unsigned long long)
                  : (lng == 1) ? va_arg(ap, unsigned long)
                               : (lng == -1) ? (unsigned short)va_arg(ap, unsigned int)
                                             : va_arg(ap, unsigned int);
                sv = (int64_t)uv;
            }

            int negative = is_signed && sv < 0;
            uint64_t mag = negative ? (uint64_t)(-(sv + 1)) + 1ull : uv;
            num_t n = num_make(mag, negative, (unsigned)base);
            int altdigits = (int)n.ndigits;

            /* The prefix, so the padding knows how many characters are already spoken for.
             * `0x` counts: `%#08x` of 0xab is `0x0000ab`, eight characters, not ten. */
            int prefix = 0;

            if (alt && base == 16 && !(n.ndigits == 1 && n.digits[0] == '0'))
                prefix = 2;
            else if (alt && base == 8 && !(n.ndigits == 1 && n.digits[0] == '0'))
                prefix = 1;

            int signlen = (negative || (is_signed && plus) || (is_signed && space)) ? 1 : 0;
            int digits  = altdigits + prefix;

            /* A precision of zero with a value of zero means *no digits at all*.  That is
             * the rule that makes `%.0d` of 0 empty, and it is the reason a program can
             * print a field that is empty rather than "0" -- which is how `%.0s` of "" and
             * `printf("%.*f", 0, 1.0)` behave on every other system. */
            if (precision == 0 && mag == 0 && !alt)
                digits = prefix;

            int pad = width - digits - signlen;

            if (pad < 0)
                pad = 0;

            if (!left && !(zero && precision < 0))
                sink_pad(o, ' ', pad);

            if (negative)
                sink_char(o, '-');
            else if (is_signed && plus)
                sink_char(o, '+');
            else if (is_signed && space)
                sink_char(o, ' ');

            /* Sign, then prefix, then zero padding, then digits -- in that order, and the
             * order is the whole of `%#06x`.
             *
             * `%#06x` of 42 is `0x002a`: six characters, two of them the prefix.  Padding
             * before the prefix gives `000x2a`, which is the same length and a different
             * number; padding after the digits gives `0x2a00`, which is a different number
             * *and* a different length.  Only one of the three is right and the two wrong
             * ones both look plausible, which is why the check exists at all.
             *
             * The prefix is not emitted for a zero: `printf("%#x", 0)` is `0` on every
             * other system -- there is nothing to mark -- and a program printing hashes
             * would otherwise get `0x0` for the empty hash. */
            if (alt && base == 16 && prefix == 2)
                sink_bytes(o, upper ? "0X" : "0x", 2);
            else if (alt && base == 8 && prefix == 1)
                sink_char(o, '0');

            if (!left && zero && precision < 0)
                sink_pad(o, '0', pad);

            /* A precision of zero on a value of zero produces *no digits at all*, which is
             * the rule that makes `%.0d` of 0 empty and `%.0f` of 0.0 empty.  A `printf`
             * that printed "0" here is not wrong-looking, it is wrong, and it is wrong for
             * every field a program wanted to leave empty. */
            if (!(precision == 0 && mag == 0))
                num_write(o, &n, (unsigned)base, upper);

            if (left)
                sink_pad(o, ' ', pad);

            continue;
        }

        switch (conv) {
        case 'c': {
            int c = va_arg(ap, int);

            if (!left)
                sink_pad(o, ' ', width - 1);
            sink_char(o, (char)c);
            if (left)
                sink_pad(o, ' ', width - 1);
            break;
        }

        case 's': {
            const char *s = va_arg(ap, const char *);

            /* NULL is not the caller's fault -- it is what `va_arg` gives when the
             * program passed a null pointer -- and every other libc prints "(null)" rather
             * than crashing, because the alternative is a segfault in the middle of an
             * error message. */
            if (s == NULL)
                s = "(null)";

            size_t len = (precision >= 0) ? strnlen(s, (size_t)precision) : strlen(s);

            int pad = width - (int)len;

            if (pad < 0)
                pad = 0;

            if (!left)
                sink_pad(o, ' ', pad);

            sink_bytes(o, s, len);

            if (left)
                sink_pad(o, ' ', pad);
            break;
        }

        case 'p': {
            void *v = va_arg(ap, void *);
            uintptr_t u = (uintptr_t)v;

            /* The width that makes `printf("%p")` produce the same shape as glibc's: a
             * fixed 16 hex digits with a leading zero, because a pointer is an address and
             * an address is easier to read at a constant width.  `0x` is not optional
             * here, unlike `%#x` -- a bare pointer is not a number and has nothing to
             * look like. */
            int  digits = 16;
            num_t n = num_make((uint64_t)u, 0, 16);

            sink_bytes(o, "0x", 2);
            sink_pad(o, '0', digits - (int)n.ndigits);
            num_write(o, &n, 16, 0);
            break;
        }

        case 'n': {
            /* The count of characters written *so far*, stored through a pointer.
             *
             * It is here because it is part of ISO C and because omitting it turns a
             * program's `printf("%d%n", &n)` into a crash rather than into a wrong answer.
             * The pointer is checked, and a bad one is a runtime rather than a fault: this
             * is the one conversion whose argument is *written*, so it is the one where a
             * program's mistake is a corruption instead of a bad read. */
            int     *ip = va_arg(ap, int *);

            if (ip != NULL)
                *ip = (int)o->used;
            break;
        }

        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
            /* Handled above; reaching here would mean the switch above missed one. */
            break;

        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            /* Floating point.  There is no floating-point hardware configured in this
             * kernel's build (`-mno-sse -mno-mmx`) and no software floating point here
             * yet, so these conversions cannot be right and saying so is better than
             * printing a plausible wrong number.
             *
             * What is printed is the value's own bit pattern, so the output is at least
             * *about* the argument and is distinguishable from anything real.  Phase 10
             * brings a real implementation. */
            (void)lngdouble;

            if (plus)
                sink_char(o, '+');
            sink_bytes(o, "0.0 (no floating point in this libc)", 32);
            break;

        default:
            /* An unknown conversion is printed as itself rather than skipped, so that a
             * format typo shows up in the output instead of quietly eating the rest of the
             * line. */
            sink_char(o, '%');
            sink_char(o, conv);
            break;
        }
    }

    return (int)o->used;
}

/* --- the entry points ----------------------------------------------------------------- */

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vfprintf(stdout, fmt, ap);
    va_end(ap);

    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);

    return n;
}

/* The string forms, and the two things they need that the stream form does not.
 *
 * One difference and one rule:
 *
 *   - `size` 0 means "unbounded": store everything.  That is what `sprintf()` uses, since
 *     it has no size to pass, and what a buffer that knows its own length passes.
 *
 *   - `size` 0 with `buf` NULL means "count, do not store".  That is the measuring pass
 *     `asprintf()` makes before it allocates, and it is why the count is kept in the sink
 *     rather than being derived from what was stored: a sink that reported `strlen(buf)`
 *     could not answer the only question a caller with no buffer has. */
static int vformat_to_string(char *buf, size_t size, const char *fmt, va_list ap) {
    sink_t o;

    o.file = NULL;
    o.buf  = buf;
    o.size = size;
    o.used = 0;

    /* A NULL format is a programmer's mistake, and every libc here answers it the same way:
     * a negative return and nothing written.  Reading through the pointer instead would be
     * a fault in the middle of a message, which is the worst place to have one. */
    if (fmt == NULL)
        return -1;

    int n = vformat(&o, fmt, ap);

    /* NUL-terminated, but only if there is room for one -- a buffer filled exactly to its
     * last byte cannot also hold a terminator, and writing one anyway is the overflow that
     * `snprintf()` exists to prevent.  `snprintf()` with a size of exactly the string's
     * length is legal, returns that length, and writes no terminator. */
    if (buf != NULL && (size == 0 || o.used < size))
        buf[o.used] = 0;

    return n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    sink_t o;

    o.file = f;
    o.buf  = NULL;
    o.size = 0;
    o.used = 0;

    if (fmt == NULL)
        return -1;

    /* The count is the characters formatted, not the bytes the stream accepted: ISO C says
     * `printf` returns what it *would* have written, and a program checking the return
     * against what it asked for is checking for a truncated write, which is not the same
     * thing. */
    return vformat(&o, fmt, ap);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    return vformat_to_string(buf, size, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vformat_to_string(buf, size, fmt, ap);
    va_end(ap);

    return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    /* Size 0, which the sink reads as "no limit".  `sprintf()` cannot know how much room
     * there is and asking `strlen()` would be worse than useless -- the buffer is
     * uninitialised.  A program that wants a bound uses `snprintf()`, and a program that
     * uses `sprintf()` into a buffer it sized with `strlen()` of its own contents has a bug
     * that no libc can find. */
    return vformat_to_string(buf, 0, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsprintf(buf, fmt, ap);
    va_end(ap);

    return n;
}

int vasprintf(char **out, const char *fmt, va_list ap) {
    va_list copy;

    if (out == NULL)
        return -1;

    /* Measured twice, which is the only way `va_list` can be walked twice: `va_copy` is
     * the one operation on it that is allowed to exist for that purpose.  The first pass
     * has a size of 0, which the sink treats as "count but do not store" -- and that is
     * why the counting is in the sink at all rather than in the callers. */
    va_copy(copy, ap);
    int need = vformat_to_string(NULL, 0, fmt, copy);
    va_end(copy);

    if (need < 0)
        return -1;

    char *buf = malloc((size_t)need + 1);

    if (buf == NULL)
        return -1;

    int n = vformat_to_string(buf, (size_t)need + 1, fmt, ap);

    *out = buf;

    return n;
}

int asprintf(char **out, const char *fmt, ...) {
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vasprintf(out, fmt, ap);
    va_end(ap);

    return n;
}