/* minx - smptest: do CPU-bound processes really run at the same time?
 *
 * "SMP works" is easy to claim and easy to get wrong, because a scheduler that round-robins
 * one task to completion before starting the next behaves identically from the outside --
 * every program runs, every program finishes, and nothing ever notices that four processors
 * spent the run taking turns.
 *
 * So this does not check that the children *finish*. It checks two things a serial scheduler
 * cannot fake:
 *
 *   1. **They were on more than one processor.**  Each child asks the kernel which processor
 *      it is running on, and the kernel answers from the APIC's own identification register
 *      rather than from the scheduler's bookkeeping -- so "the scheduler spread them out"
 *      and "the hardware ran them at once" are checked against different sources, and a
 *      scheduler that believed it had migrated a task would be caught.
 *
 *   2. **Their lifetimes overlapped.**  Every child records the millisecond clock on entry
 *      and on exit, and the check is that the *latest* start is before the *earliest* end.
 *      Run in sequence, the last child starts after the first has finished and this fails,
 *      however many processors there are.
 *
 * The work is a sieve rather than a loop of additions on purpose: it has an unpredictable
 * memory access pattern and enough of one to keep a processor busy, so a child is not
 * finished before the scheduler has put it anywhere. And the sieve's answer is checked from
 * every child, so a child that returned the right number without computing it -- which is
 * what a broken "run things in parallel" story would produce -- cannot pass.
 *
 * Both checks are conditional on there being more than one processor. A one-processor machine
 * cannot fail either, and a test that reports a failure for a correct single-processor run is
 * a test about the machine rather than about the kernel.
 */
#include "../common/minx_user.h"
#include "../common/print.h"

#define CHILDREN   8
#define SIEVE_MAX  300000
/* How many times each child sieves.
 *
 * Because the only clock available is in milliseconds, a child has to run for
 * longer than one of them or "these children overlapped" and "these children
 * ran in sequence" produce the same numbers.  One sieve of 300000 is under a
 * millisecond, and the first run of this test reported that its children had not
 * overlapped -- on a machine where they had, all inside the same millisecond.
 *
 * Repeated rather than made larger because a bigger sieve is a bigger static array,
 * and a test that wants a hundred times the work should not want a hundred times
 * the memory.  The answer has to be the same every round, which is checked. */
#define ROUNDS      24
#define PRIMES     25997      /* primes below SIEVE_MAX: a fact about arithmetic, not about
                              * this kernel, and checked so that a machine not running this
                              * program is caught before it makes the other numbers mean
                              * nothing */

/* What one child reports back to its parent.
 *
 * Written as one `write()` of a whole record on purpose: a pipe written a field at a time is
 * four writes, and four writes is four places for two children's reports to interleave. */
typedef struct {
    long cpu;        /* which processor, from the APIC             */
    long start_ms;   /* the clock on entry                          */
    long end_ms;     /* the clock on exit                           */
    long primes;     /* the answer, so the work cannot be skipped   */
    long pid;
} result_t;

/* Primes below `n`, by the sieve of Eratosthenes.
 *
 * Static rather than on the stack: 300001 bytes is more than a test program's stack should
 * be asked for, and a stack overflow here would look like a kernel fault. */
static long sieve(long n) {
    static unsigned char mark[SIEVE_MAX + 1];
    long found = 0;

    for (long i = 2; i <= n; i++)
        mark[i] = 0;

    for (long p = 2; p * p <= n; p++) {
        if (mark[p])
            continue;

        for (long k = p * p; k <= n; k += p)
            mark[k] = 1;
    }

    for (long i = 2; i <= n; i++) {
        if (!mark[i])
            found++;
    }

    return found;
}

/* Primes below `n`, repeated `ROUNDS` times.
 *
 * Every round must give the same answer; the value returned is the last one, and the parent
 * checks it against a number that is a fact about arithmetic.  So a child cannot get the
 * right answer by doing the work once and returning early from the rest. */
static long busy(long n) {
    long found = 0;

    for (long r = 0; r < ROUNDS; r++)
        found = sieve(n);

    return found;
}

/* How many processors are online, from `/proc/cpuinfo`'s own `online : N of M known` line.
 *
 * Read rather than counted, and the reason is worth recording: the first version counted
 * lines beginning with "processor" and reported **five on a four-processor machine**.  There
 * is one such line per processor at the end of the file, and the count was only wrong because
 * `model name : minx virtual processor` contains the word too -- a count of something that
 * looks like the answer is not the answer.
 *
 * The kernel already publishes the number, in a field whose format it controls, so parsing
 * that is both shorter and correct.  `known` is not what is wanted: a processor the firmware
 * described and the kernel did not bring online is not one that can run anything. */
static long cpu_count(void) {
    long fd = minx_open("/proc/cpuinfo", O_RDONLY, 0);

    if (fd < 0)
        return 1;

    static char buf[8192];
    long       total = 0;

    while (total < (long)sizeof(buf) - 1) {
        long got = minx_read((int)fd, buf + total,
                             (unsigned long)sizeof(buf) - 1 - (unsigned long)total);

        if (got <= 0)
            break;

        total += got;
    }

    buf[total] = 0;
    minx_close((int)fd);

    static const char key[] = "online\t\t: ";
    const char        *at   = buf;

    while ((at = minx_strstr(at, key)) != NULL) {
        long n = 0;

        for (const char *p = at + sizeof(key) - 1; *p >= '0' && *p <= '9'; p++)
            n = n * 10 + (*p - '0');

        if (n > 0)
            return n;

        at += sizeof(key) - 1;
    }

    return 1;
}

static int same_cpu(result_t *a, result_t *b) {
    return a->cpu == b->cpu;
}

int minx_main(unsigned long argc, char **argv) {
    (void)argc;
    (void)argv;

    long cpus     = cpu_count();
    long kids     = cpus < CHILDREN ? cpus : CHILDREN;
    int  failures = 0;
    int  fds[2];
    long pids[CHILDREN];

    if (kids < 1)
        kids = 1;

    minx_put("smptest: ");
    minx_put_signed(cpus);
    minx_put(" processor(s), forking ");
    minx_put_signed(kids);
    minx_put(" CPU-bound children\n");

    if (minx_pipe(fds) != 0) {
        minx_put("smptest: pipe() failed\n");
        return minx_report("smptest", 0, "pipe");
    }

    for (long i = 0; i < kids; i++) {
        long pid = minx_fork();

        if (pid < 0) {
            minx_put("smptest: fork() failed\n");
            minx_close(fds[0]);
            minx_close(fds[1]);
            return minx_report("smptest", 0, "fork");
        }

        if (pid == 0) {
            result_t r;

            /* The read end closed first, so the record's start is when this child begins
             * work rather than when it begins preparing to. */
            minx_close(fds[0]);

            r.cpu      = minx_getcpu();
            r.start_ms = minx_uptime_ms();
            r.primes   = busy(SIEVE_MAX);
            r.end_ms   = minx_uptime_ms();
            r.pid      = minx_getpid();

            (void)minx_write(fds[1], &r, (unsigned long)sizeof(r));
            minx_close(fds[1]);
            minx_exit(0);
        }

        pids[i] = pid;
    }

    minx_close(fds[1]);

    result_t got[CHILDREN];
    long     nread = 0;

    while (nread < (long)sizeof(got)) {
        long chunk = minx_read(fds[0], (unsigned char *)got + nread,
                               (unsigned long)sizeof(got) - (unsigned long)nread);

        if (chunk <= 0)
            break;

        nread += chunk;
    }

    minx_close(fds[0]);

    long reported = nread / (long)sizeof(result_t);

    if (reported != kids) {
        minx_put("smptest: only ");
        minx_put_signed(reported);
        minx_put(" of ");
        minx_put_signed(kids);
        minx_put(" children reported\n");
        failures++;
    }

    /* Every child is reaped, whether or not it reported, and the status is checked.
     *
     * `wait()` is asked `kids` times and the pids it returns are checked as a **set**, not
     * against `pids[i]` in order.  `wait()` has no notion of the order children were forked
     * in -- it returns whichever finished first -- so comparing the first wait against the
     * first fork reports a failure for every child after the first on a machine where the
     * children finish in a different order than they started, which is most machines.
     *
     * That is not a theoretical concern: the first run of this test reported "a child exited
     * badly, status 0" seven times, with a status of zero, because the pids were compared in
     * the wrong order. */
    for (long i = 0; i < kids; i++) {
        int  status = 0;
        long pid    = minx_wait(&status);
        int  known  = 0;

        for (long j = 0; j < kids; j++) {
            if (pids[j] == pid)
                known = 1;
        }

        if (!known || status != 0) {
            minx_put("smptest: a child exited badly, status ");
            minx_put_signed(status);
            minx_put("\n");
            failures++;
        }
    }

    long distinct      = 0;
    long latest_start  = 0;
    long earliest_end  = 0;
    long wrong_answer  = 0;

    for (long i = 0; i < reported; i++) {
        result_t *r = &got[i];

        if (r->primes != PRIMES)
            wrong_answer++;

        int seen = 0;

        for (long j = 0; j < i; j++) {
            if (same_cpu(&got[j], r))
                seen = 1;
        }

        if (!seen)
            distinct++;

        if (r->start_ms > latest_start)
            latest_start = r->start_ms;

        if (earliest_end == 0 || r->end_ms < earliest_end)
            earliest_end = r->end_ms;
    }

    minx_put("smptest: ");
    minx_put_signed(reported);
    minx_put(" children, on ");
    minx_put_signed(distinct);
    minx_put(" distinct processor(s)\n");

    if (wrong_answer != 0) {
        minx_put("smptest: a child's sieve gave the wrong answer, so the work did not happen\n");
        failures++;
    }

    if (cpus > 1) {
        if (distinct < 2) {
            minx_put("smptest: every child ran on one processor: this is not parallel\n");
            failures++;
        }

        if (latest_start >= earliest_end) {
            minx_put("smptest: the children did not overlap: they ran one after another\n");
            failures++;
        }
    } else {
        minx_put("smptest: one processor, so overlap is neither required nor checked\n");
    }

    /* Two mistakes in one line, both of which turn a clean run into a reported failure with
     * nothing printed to explain it.
     *
     * The second argument is "did it pass", not "how many failed": passing the count turns
     * zero into "FAILED".
     *
     * And the call is *not* returned. `minx_report()` returns how many characters it wrote --
     * one newline -- so returning it exits 1, which the self-test reads as a failed check.
     * Every other test program calls it and then `return 0`, and the reason is here now
     * rather than only there. */
    minx_report("smptest", failures == 0 ? 1 : 0, NULL);
    return 0;
}