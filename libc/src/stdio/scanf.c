/* minx - the `scanf` family.
 *
 * Reading is the other direction from `printf()` and has the same shape: one engine, four
 * entry points that differ only in where the characters come from.  A `scanf` with its own
 * copy of the format loop is a `scanf` whose copies disagree about `%l` on a different day.
 *
 * What is implemented is the part a program actually uses to parse things it did not write:
 * whitespace handling, literal characters, `%s %c %d %i %u %x %o %n %%`, a width, `*` to
 * suppress an assignment, and the `hh`/`h`/`l`/`ll`/`z` length modifiers.
 *
 * What is *not* implemented is `%f` and its relatives, and for the same reason `printf`'s
 * are not: there is no floating-point hardware in this build and no software floating point
 * here, so a conversion that claimed to read a double would store a number that is wrong
 * without looking wrong.  `%f` consumes its characters and assigns nothing.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- the input side ------------------------------------------------------------ */

/* A source of characters, so the format loop does not care whether the text came from a
 * stream or from a string.  `pushback` is one character, which is all any of these needs:
 * `scanf` looks at most one character past what it has consumed and has to give it back. */
typedef struct {
    FILE            *file;      /* set for the stream sources */
    const char      *str;       /* set for the string source  */
    size_t           len;
    size_t           at;
    int              pushback;  /* -1 when there is nothing pushed back */
} src_t;

static void src_init_file(src_t *s, FILE *f) {
    s->file = f; s->str = NULL; s->len = 0; s->at = 0; s->pushback = -1;
}

static void src_init_str(src_t *s, const char *str) {
    s->file = NULL; s->str = str; s->len = 0; s->at = 0; s->pushback = -1;

    /* The length is measured once, up front.  A string source has to know where it ends
     * before it reads, which a NUL scan does in one pass -- and doing it here means the
     * loop below has no way to read past the end even if the format asks it to. */
    if (str != NULL)
        while (str[s->len] != 0)
            s->len++;
}

static int src_getc(src_t *s) {
    if (s->pushback >= 0) {
        int c = s->pushback;

        s->pushback = -1;
        return c;
    }

    if (s->file != NULL)
        return fgetc(s->file);

    if (s->at >= s->len)
        return EOF;

    return (unsigned char)s->str[s->at++];
}

static void src_ungetc(src_t *s, int c) {
    if (c != EOF)
        s->pushback = c;
}

/* The predicate `scanf` uses for "whitespace".  Locale-independent on purpose, and the same
 * six characters every other libc uses without a locale.  A locale-sensitive `isspace()`
 * would make a program parse a file whose bytes happen to fall in some other locale's
 * whitespace differently on two machines. */
static int is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }

static int digit_value(int c) {
    if (c >= '0' && c <= '9')      return c - '0';
    if (c >= 'a' && c <= 'f')      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')      return c - 'A' + 10;
    return -1;
}

/* --- the engine ----------------------------------------------------------------- */

/* Write one integer into whatever `scanf` was pointed at, at whatever width the length
 * modifier asked for.
 *
 * One place, because getting the width wrong for one modifier and not the others is a
 * truncation into a 4-byte target from a 8-byte conversion -- which corrupts the next thing
 * on the stack and is reported as a crash somewhere else. */
static void store_int(void *target, int lng, long long v) {
    switch (lng) {
    case 2:  *(long long *)target = v; break;
    case 1:  *(long *)target = (long)v; break;
    case -1: *(short *)target = (short)v; break;
    default: *(int *)target = (int)v; break;
    }
}

/* The length modifier as a single number:
 *
 *   2  long long / size_t / intmax_t -- 64 bits
 *   1  long                          -- 64 bits here
 *   -1 short, and `hh` narrowed twice to a signed char
 *   0  int
 *
 * `hh` is `-1` and `h` is `1` because the *target* type is all that matters at the point of
 * the store, and `char *` is `short *` and `int *` both being one pointer wide means a
 * narrower store of the same value produces the same bytes. */
typedef struct {
    int  assigned;
    int  matched;
} scan_result_t;

/* Read one conversion.  Returns 0 always; the result is in `r`.  Stopping is the caller's
 * decision because the caller's loop is over the format, not over conversions. */
static void scan_conv(src_t *s, const char **fmt, va_list ap, scan_result_t *r) {
    const char *p = *fmt;
    int c;

    /* Whitespace in the format matches any run of whitespace in the input, and also
     * matches none.  This is the rule that surprises everybody once. */
    while (is_space((unsigned char)*p))
        p++;

    if (*p == 0) {
        *fmt = p;
        return;
    }

    /* An ordinary character matches exactly itself. */
    if (*p != '%') {
        c = src_getc(s);

        if (c != (unsigned char)*p) {
            src_ungetc(s, c);
            *fmt = p;
            return;
        }

        *fmt = p + 1;
        return;
    }

    p++;

    if (*p == '%') {
        c = src_getc(s);

        if (c != '%') {
            src_ungetc(s, c);
            *fmt = p;
            return;
        }

        *fmt = p + 1;
        return;
    }

    int suppress = 0;

    if (*p == '*') {
        suppress = 1;
        p++;
    }

    int width = 0;

    while (is_digit((unsigned char)*p))
        width = width * 10 + (*p++ - '0');

    int lng = 0;

    if (*p == 'h') {
        p++;
        if (*p == 'h') {
            p++;
            lng = -1;
        } else {
            lng = 1;
        }
    } else if (*p == 'l') {
        p++;
        if (*p == 'l') {
            p++;
            lng = 2;
        } else {
            lng = 1;
        }
    } else if (*p == 'z' || *p == 'j' || *p == 't') {
        p++;
        lng = 2;
    } else if (*p == 'L') {
        p++;
    }

    char conv = *p;

    /* Input whitespace is skipped before a conversion -- *not* `%c` and not `%[`.
     *
     * This is the part that is most often got wrong in both directions: `%d` on "  42"
     * gives 42 because of the skip here, and `%c` on " 42" gives the space because there
     * is none.  A `scanf` that skipped whitespace for `%c` would make reading the spaces in
     * a line impossible. */
    if (conv != 'c' && conv != '[') {
        for (;;) {
            c = src_getc(s);

            if (!is_space(c)) {
                src_ungetc(s, c);
                break;
            }
        }
    }

    switch (conv) {
    case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
        unsigned base = (conv == 'x' || conv == 'X') ? 16 : (conv == 'o') ? 8 : 10;
        int      negative = 0;
        long long v = 0;
        int       got = 0;
        int       budget = width > 0 ? width : 1 << 30;

        c = src_getc(s);

        if (c == '+' || c == '-') {
            negative = (c == '-');
            budget--;
            c = src_getc(s);
        }

        /* `0x` is accepted for `%i` and for `%x`, and is *not* required.  It has to be
         * undone when the `x` turns out not to be there, because `%x` of "0" is zero and of
         * "0x" is nothing at all. */
        if ((conv == 'i' || conv == 'x' || conv == 'X') && c == '0' && budget > 1) {
            int c2 = src_getc(s);

            if (c2 == 'x' || c2 == 'X') {
                int d = src_getc(s);

                if (digit_value(d) >= 0 && digit_value(d) < 16) {
                    v = digit_value(d);
                    got = 1;
                    budget -= 2;
                    c = src_getc(s);
                    goto digits;
                }

                src_ungetc(s, d);
            }

            src_ungetc(s, c2);
        }

        /* `0` as the first digit of an octal conversion, and of a decimal one it is just
         * zero -- which is why `%o` of "0" is 0 and not "no conversion matched". */
        if (got == 0 && base == 8 && c == '0') {
            v = 0;
            got = 1;
            budget--;
            c = src_getc(s);
        }

    digits:
        while (got < budget) {
            int d = digit_value(c);

            if (d < 0 || (unsigned)d >= base)
                break;

            v = v * (long long)base + d;
            got++;
            c = src_getc(s);
        }

        src_ungetc(s, c);

        if (got == 0) {
            *fmt = p;
            return;
        }

        if (!suppress) {
            store_int(va_arg(ap, void *), lng, negative ? -v : v);
            r->assigned++;
        }

        r->matched++;
        break;
    }

    case 'c': {
        int   n = width > 0 ? width : 1;
        char *out = suppress ? NULL : va_arg(ap, char *);

        for (int i = 0; i < n; i++) {
            int ch = src_getc(s);

            if (ch == EOF) {
                if (i == 0) {
                    *fmt = p;
                    return;
                }
                break;
            }

            if (out != NULL)
                out[i] = (char)ch;
        }

        if (out != NULL)
            out[n] = 0;

        if (!suppress)
            r->assigned++;

        r->matched++;
        break;
    }

    case 's': {
        char *out = suppress ? NULL : va_arg(ap, char *);
        int   n   = 0;
        int   budget = width > 0 ? width : 1 << 30;

        while (n < budget) {
            int ch = src_getc(s);

            if (ch == EOF || is_space(ch)) {
                src_ungetc(s, ch);
                break;
            }

            if (out != NULL)
                out[n] = (char)ch;

            n++;
        }

        if (n == 0) {
            *fmt = p;
            return;
        }

        /* The terminator goes one past the last character written, and a program that gave
         * a width of `n` must have a buffer of `n + 1`.  That is the documented contract;
         * `%255s` into a `char[255]` is an overflow on every other system too. */
        if (out != NULL)
            out[n] = 0;

        if (!suppress)
            r->assigned++;

        r->matched++;
        break;
    }

    case 'n': {
        /* The number of characters read so far.  It never fails, because a program that has
         * already done the reading and wants the count is not asking whether anything
         * matched. */
        int *ip = va_arg(ap, int *);

        if (ip != NULL)
            *ip = (int)(s->at + (s->pushback >= 0 ? 1 : 0));

        if (!suppress)
            r->assigned++;

        *fmt = p + 1;
        return;
    }

    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        /* Floating point: the characters are consumed and nothing is assigned.  There is
         * no representation to assign -- this build has no floating-point hardware and no
         * software floating point -- and storing a plausible wrong number into a program's
         * `double` would be worse than storing nothing, because nothing is detectable. */
        int n = 0;

        for (;;) {
            int ch = src_getc(s);

            if (ch == EOF)
                break;

            if (!(is_digit(ch) || ch == '.' || ch == '-' || ch == '+' ||
                  ch == 'e' || ch == 'E' || ch == 'x' || ch == 'X' ||
                  (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
                src_ungetc(s, ch);
                break;
            }

            n++;
        }

        if (n == 0) {
            *fmt = p;
            return;
        }

        r->matched++;
        break;
    }

    default:
        /* An unknown conversion is the caller's error, and it is reported as "nothing
         * matched" rather than as a count that could be mistaken for a real one. */
        *fmt = p;
        return;
    }

    *fmt = p + 1;
}

/* The one loop.  Runs until the format ends or a conversion fails to match, and returns the
 * number of assignments -- which is what ISO C says `scanf` returns, and which is *not* the
 * number of conversions: a `%*d` matches and assigns nothing, so it counts as a match and
 * not as an assignment, and a program that checks the return against a count of `%`
 * conversions is off by one for every `*`. */
static int scan_run(src_t *s, const char *fmt, va_list ap) {
    scan_result_t r = { 0, 0 };
    const char   *p = fmt;

    if (fmt == NULL)
        return EOF;

    while (*p != 0) {
        const char *before = p;

        scan_conv(s, &p, ap, &r);

        /* No progress and no match: the format asked for something the input does not have,
         * and that is where `scanf` stops. */
        if (p == before)
            break;
    }

    /* EOF before anything was assigned is `EOF`, and EOF after some assignments is the
     * count.  A program that checks `== EOF` therefore only sees it when *nothing* matched,
     * which is the distinction that makes "end of input" different from "a field was
     * malformed". */
    if (r.assigned == 0)
        return (s->at >= s->len && s->file == NULL && s->pushback < 0) ? EOF : 0;

    return r.assigned;
}

/* --- the entry points ------------------------------------------------------------ */

int vfscanf(FILE *f, const char *fmt, va_list ap) {
    src_t s;

    src_init_file(&s, f);

    return scan_run(&s, fmt, ap);
}

int vscanf(const char *fmt, va_list ap) {
    src_t s;

    /* The string sources are told their length up front; this one is told nothing, because
     * there is no string.  `scan_run` therefore reads EOF from the stream and that is the
     * end of input. */
    src_init_file(&s, stdin);

    return scan_run(&s, fmt, ap);
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
    src_t s;

    src_init_str(&s, str);

    return scan_run(&s, fmt, ap);
}

int scanf(const char *fmt, ...) {
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vscanf(fmt, ap);
    va_end(ap);

    return n;
}

int fscanf(FILE *f, const char *fmt, ...) {
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vfscanf(f, fmt, ap);
    va_end(ap);

    return n;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsscanf(str, fmt, ap);
    va_end(ap);

    return n;
}

/* --- the small odds and ends ------------------------------------------------------- */

/* `perror()`: the name, a colon, the error, a newline, on stderr.
 *
 * Three writes rather than one `fprintf`, which looks worse and is better -- `fprintf` would
 * work, but `perror()` is what a program calls when something has already gone wrong, and
 * the fewer things between the failure and the message the better.  A NULL prefix prints
 * nothing before the colon, which is what every other libc does and what a program that has
 * nothing to add wants. */
void perror(const char *prefix) {
    int saved = errno;

    if (prefix != NULL && prefix[0] != 0) {
        fputs(prefix, stderr);
        fputs(": ", stderr);
    }

    fputs(strerror(saved), stderr);
    fputs("\n", stderr);
}

/* `setmode()` is an MSVCRT name for what POSIX calls nothing, and on this machine there is
 * nothing for it to do: text and binary are the same thing, because there is one kind of
 * file and the terminal translates nothing.  It returns 0, which is `O_TEXT`, because that
 * is what it is being asked about and a program that checks it deserves an answer. */
int setmode(int fd, int mode) {
    (void)fd;
    (void)mode;

    return 0;
}