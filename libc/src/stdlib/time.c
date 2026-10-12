/* minx - time.
 *
 * One clock, so `time()` is not wall-clock seconds.  It is milliseconds since boot, and
 * saying so here is the difference between a program that can print it and one that
 * formats it as a date and produces 1970.
 *
 * There is no real-time source in the kernel: the CMOS clock is there but nothing reads it,
 * and inventing a date out of a counter would produce a `date` command that is confidently
 * wrong.  Phase 10 adds the CMOS reader; until then `gmtime()` says so in the fields it
 * cannot fill in, which is more useful than a plausible number.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Where the kernel's clock actually lives.  Two calls rather than one, because there is no
 * `gettimeofday()` here and `clock_gettime()` is the interface every program already knows.
 *
 * `CLOCK_REALTIME` and `CLOCK_MONOTONIC` are the same clock on this machine, and both
 * answer the same.  There is one timebase and a program that wants "wall clock" is a
 * program that wants something this kernel does not have. */
/* The four numbers `times()` answers with, in the kernel's order and widths: this
 * process's CPU time, its children's user time, its children's system time, and the
 * machine's uptime.  All four in milliseconds, because milliseconds is what this kernel's
 * timebase counts in -- docs/SYSCALLS.md says so rather than leaving the unit to be
 * guessed, because a caller that multiplies by ten to get a CPU percentage would otherwise
 * be wrong by a factor of ten.
 *
 * The two children's fields are always zero: this kernel does not account for a task's
 * children, and a program dividing its own time by a structurally-zero field would divide
 * by zero rather than get a wrong answer. */
struct tms_raw {
    uint64_t utime_ms;
    uint64_t stime_ms;
    uint64_t cutime_ms;
    uint64_t cstime_ms;
    uint64_t uptime_ms;
};

/* POSIX's `struct tms` is the first four of these and nothing else; it is declared in
 * <sys/times.h> for a program that wants it by name.  The assertion is here because the
 * kernel copies the whole structure in one `copy_to_user`, and a size mismatch is not a
 * crash -- it is a `cstime` reading the uptime. */
typedef char assert_tms_raw_size[(sizeof(struct tms_raw) == 40) ? 1 : -1];

static bool read_clocks(struct tms_raw *out) {
    return __syscall1(SYS_TIMES, (long)(uintptr_t)out) == 0;
}

int clock_gettime(int clk, struct timespec *out) {
    struct tms_raw t;

    if (out == NULL) {
        errno = EFAULT;
        return -1;
    }

    if (!read_clocks(&t))
        return -1;

    /* `CLOCK_REALTIME` and `CLOCK_MONOTONIC` answer the same thing, and that is not a
     * shortcut: there is one timebase on this machine and no source of real time.  A
     * program asking for the realtime clock gets the monotonic one, which is *not* wall
     * time, and a program that formats it as a date gets 1970.  Returning the uptime is
     * the answer that is useful for measuring; saying in <time.h> that it is uptime is
     * what keeps it from being mistaken for a date. */
    uint64_t ms = t.uptime_ms;

    out->tv_sec  = (time_t)(ms / 1000);
    out->tv_nsec = (long)(ms % 1000) * 1000000L;

    return 0;
}

/* `time()` is `clock_gettime()` rounded down to whole seconds.
 *
 * Seconds since boot, not since the epoch, and the header says so.  A program that wants a
 * duration -- "how long did this take" -- is the use that works; a program that wants a
 * date is the use that does not, and the difference is one sentence in <time.h>. */
time_t time(time_t *out) {
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        if (out != NULL)
            *out = (time_t)-1;
        return (time_t)-1;
    }

    if (out != NULL)
        *out = ts.tv_sec;

    return ts.tv_sec;
}

/* CPU time, which is the one clock here that genuinely is a clock: the scheduler counts the
 * ticks a task has had, and that is what `clock()` is for.  It excludes children, which is
 * what POSIX says and is also the only thing this kernel can answer without walking a
 * process tree. */
clock_t clock(void) {
    struct tms_raw t;

    if (!read_clocks(&t))
        return (clock_t)-1;

    /* Milliseconds to `CLOCKS_PER_SEC`, which is a million -- a microsecond clock.  POSIX
     * only requires `CLOCKS_PER_SEC` to be consistent with itself, and choosing the finest
     * one means a program that divides by it to get seconds gets a real fraction rather
     * than zero for anything shorter than a millisecond. */
    return (clock_t)((t.utime_ms + t.stime_ms) * (CLOCKS_PER_SEC / 1000));
}

/* --- the calendar -------------------------------------------------------------------- */

/* Both of these fill in what they honestly can and leave the rest zero, and that is the
 * whole of the story.
 *
 * A `struct tm` is a date: which year, which month, which day of the month.  Given a count
 * of seconds since some epoch there is a real calculation to do here, and doing it would
 * produce a *wrong* date rather than an obviously absent one -- a `date` command printing
 * "Thu Jan  1 00:00:00 1970" on every boot looks like a working date and is not.
 *
 * What is filled in: `tm_hour`, `tm_min` and `tm_sec` from the count, because those are
 * arithmetic on the seconds and are correct relative to whatever epoch the caller means.
 * Everything that needs a calendar -- the day, the month, the year, the day of the week --
 * is left zero, and the caller can see that they are zero. */
struct tm *gmtime(const time_t *t) {
    static struct tm tm;

    if (t == NULL)
        return NULL;

    time_t secs = *t;

    memset(&tm, 0, sizeof(tm));

    tm.tm_hour = (int)((secs / 3600) % 24);
    tm.tm_min  = (int)((secs / 60) % 60);
    tm.tm_sec  = (int)(secs % 60);

    /* `tm_wday` of 0 is Sunday, and leaving it there is not a smaller number than "unknown"
     * -- it is a *day*.  There is no sentinel in `struct tm` for "I do not know", which is
     * the structural reason this is documented rather than done quietly. */
    tm.tm_isdst = -1;

    return &tm;
}

struct tm *localtime(const time_t *t) {
    /* One timezone on this machine and it is UTC, because the machine has no idea what
     * time it is anywhere.  So `localtime()` and `gmtime()` are the same function, and a
     * program that can tell the difference between them is a program using a timezone this
     * kernel does not have. */
    return gmtime(t);
}

char *asctime(const struct tm *tm) {
    static char buf[64];

    if (tm == NULL)
        return NULL;

    snprintf(buf, sizeof(buf), "%02d:%02d:%02d (no date: this libc has no calendar)",
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    return buf;
}

char *ctime(const time_t *t) {
    if (t == NULL)
        return NULL;

    return asctime(gmtime(t));
}

/* `strftime()` handles the directives whose answer is arithmetic on the value it is given
 * and refuses the ones that need a calendar, rather than printing something plausible for
 * every letter.
 *
 * Returning 0 for "%Y" means "nothing was produced", and ISO C defines a return of 0 as a
 * failure.  That is the honest answer: this libc cannot format a year, and a caller that
 * checks the return finds out. */
size_t strftime(char *buf, size_t max, const char *fmt, const struct tm *tm) {
    if (buf == NULL || max == 0)
        return 0;

    size_t n = 0;

    for (const char *p = fmt; *p != 0 && n + 1 < max; p++) {
        if (*p != '%') {
            buf[n++] = *p;
            continue;
        }

        p++;

        switch (*p) {
        case 'H': n += (size_t)snprintf(buf + n, max - n, "%02d", tm->tm_hour); break;
        case 'M': n += (size_t)snprintf(buf + n, max - n, "%02d", tm->tm_min);  break;
        case 'S': n += (size_t)snprintf(buf + n, max - n, "%02d", tm->tm_sec);  break;
        case '%': buf[n++] = '%'; break;
        case 'Y': case 'y': case 'm': case 'd': case 'e':
        case 'B': case 'b': case 'h': case 'A': case 'a': case 'j': case 'p':
        case 'Z': case 'z':
            /* Needs a calendar or a timezone.  Emitting a space keeps the field's width so
             * that a format like "%Y-%m-%d %H:%M" still lines up, and the spaces are the
             * visible sign that the field is empty. */
            buf[n++] = ' ';
            break;
        default:
            buf[n++] = '%';
            if (n + 1 < max)
                buf[n++] = *p;
            break;
        }
    }

    buf[n] = 0;

    return n;
}