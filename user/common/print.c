/* minx - printing for user programs.  See print.h. */
#include "print.h"

#include "minx_user.h"

static unsigned long written;

long minx_put_fd(int fd, const char *s) {
    long n = minx_write(fd, s, minx_strlen(s));

    if (n > 0 && fd == 1)
        written += (unsigned long)n;
    return n;
}

long minx_put(const char *s) {
    return minx_put_fd(1, s);
}

unsigned long minx_put_count(void) {
    return written;
}

long minx_put_num(unsigned long v) {
    char buf[24];
    int i = 23;

    buf[i--] = 0;
    if (v == 0)
        buf[i--] = '0';

    while (v != 0) {
        buf[i--] = (char)('0' + (v % 10));
        v /= 10;
    }
    /* The digits are buf[i+1 .. 22], which is `22 - i` of them and not `23 - i`: the
     * extra byte is the terminator, and sending it writes a NUL nobody asked for.  It is
     * invisible on a terminal and it is what once made the whole test suite's tally grep
     * treat the serial log as binary. */
    return minx_write(1, &buf[i + 1], (unsigned long)(22 - i));
}

long minx_put_signed(long v) {
    if (v < 0) {
        minx_put("-");
        /* Negated as unsigned, so that the most negative long does not stay negative after
         * the sign is taken off it -- and then divided as unsigned, because the same
         * division is done on the magnitude below. */
        return minx_put_num((unsigned long)(-(v + 1)) + 1ul);
    }
    return minx_put_num((unsigned long)v);
}

long minx_report(const char *name, int ok, const char *detail) {
    minx_put(name);
    minx_put(ok ? ": ok" : ": FAILED");
    if (!ok && detail != NULL) {
        minx_put(" -- ");
        minx_put(detail);
    }
    return minx_put("\n");
}