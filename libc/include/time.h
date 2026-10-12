/* minx - <time.h>
 *
 * ISO C's time interface, and POSIX's two structs.  The clock is the kernel's monotonic
 * one, so `time()` here does not mean wall-clock seconds since the epoch: it means
 * milliseconds since boot.  A wall clock needs a source of real time that this kernel does
 * not yet have (see docs/STATUS.md, phase 10), and pretending otherwise with a number that
 * is not a date is worse than saying what the number is.
 *
 * `clock()` and `times()` are likewise CPU time, which this kernel can answer because it
 * counts ticks.
 */
#ifndef MINX_TIME_H
#define MINX_TIME_H

#include <sys/sysdep.h>

/* The two structs every Unix has, with the same fields in the same order.  `tv_nsec` is
 * long rather than int so that a program which writes `tv.tv_nsec = 1000000000L` -- which
 * every program that has ever been written by somebody in a hurry does -- compiles. */
struct timespec {
    time_t tv_sec;
    long    tv_nsec;
};

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

struct tm {
    int tm_sec;        /* 0-60, 60 for a leap second */
    int tm_min;        /* 0-59 */
    int tm_hour;       /* 0-23 */
    int tm_mday;       /* 1-31 */
    int tm_mon;        /* 0-11, 0 is January */
    int tm_year;       /* years since 1900 */
    int tm_wday;       /* 0-6, 0 is Sunday */
    int tm_yday;       /* 0-365 */
    int tm_isdst;      /* -1 unknown, 0 no, 1 yes */
};

/* The CLOCK_* selectors.  Only CLOCK_REALTIME and CLOCK_MONOTONIC are answered, and both
 * answer the same thing here: the kernel has one clock. */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#define CLOCKS_PER_SEC 1000000L

/* --- the calls -------------------------------------------------------------- */

/* Milliseconds since boot.  Returned in a `time_t`, which is the only place this value
 * goes: a program that wants a wall clock has to get it from somewhere else and there is
 * nowhere else yet. */
time_t time(time_t *out);

/* CPU time consumed, in `CLOCKS_PER_SEC` units.  This is a *process* figure, from the
 * scheduler's accounting, and unlike `times()` it excludes the children. */
clock_t clock(void);

/* The kernel's monotonic counter, as a `struct timespec`.  `CLOCK_REALTIME` is accepted
 * and answers the same: one clock, one answer, rather than two names for it. */
int clock_gettime(int clk, struct timespec *out);
int nanosleep(const struct timespec *req, struct timespec *rem);

/* Both conversions are the identity.
 *
 * A real `gmtime()` needs a calendar and `localtime()` needs a calendar and a timezone
 * database, and inventing either produces a date that is confidently wrong -- a `date`
 * command that prints the 12th of May 1970 is not a program with a bug, it is a program
 * lying.  So the struct comes back with the fields it can honestly fill in and the ones
 * it cannot left at zero, and the fact is recorded here and in docs/STATUS.md rather than
 * hidden behind a plausible number. */
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);

char *asctime(const struct tm *tm);
char *ctime(const time_t *t);
size_t strftime(char *buf, size_t max, const char *fmt, const struct tm *tm);

#endif /* MINX_TIME_H */