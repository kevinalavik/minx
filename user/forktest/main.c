/* minx - forktest: what `fork`, `wait` and `exit` actually promise.
 *
 * `hello` forks once.  That checks that a child is made and that it can talk to its parent
 * over a pipe, and nothing else: one child cannot tell a fork that copies an address space
 * from one that shares one by accident, because with one child there is nothing for the
 * two of them to disagree about.
 *
 * So this program does the things that only show up with more than one child:
 *
 *   1. Four children, each writing a different number of bytes down its own pipe, all
 *      interleaved.  A shared buffer, a lost wake or a queue that hands two processors the
 *      same task shows up here as a byte count that does not add up.
 *   2. Every child's exit status comes back through `wait`, and each one is different, so
 *      "the status of whichever child finished last" cannot pass.
 *   3. `wait` with no children left reports that rather than blocking for ever, which is
 *      the difference between a reaped process tree and a hung shell.
 *
 * The exit status of this program is zero only if every check passed, so the kernel's
 * self-test reads one number and knows.
 */
#include "../common/minx_user.h"
#include "../common/print.h"

#define CHILDREN 4

/* Each child writes its own number of bytes, so a pipe that returned another child's data
 * -- a ring handed to two readers, a cursor normalised wrongly -- shows up as a checksum
 * mismatch rather than as four identical successes. */
#define BYTES_FOR(i) (64 + (i) * 37)

static unsigned char pat(unsigned long i) {
    return (unsigned char)(i * 31u + 7u);
}

static unsigned long pat_sum(unsigned long n) {
    unsigned long s = 0;

    for (unsigned long i = 0; i < n; i++)
        s += pat(i);
    return s;
}

/* What the parent expects from child `i`, computed from the same definition the child
 * writes from, rather than from the child's answer: a kernel that delivered the bytes
 * correctly but the *test* that believed whatever came back would pass either way. */
static unsigned long expect_sum(int i) {
    return pat_sum(BYTES_FOR(i));
}

static int fail(const char *what) {
    minx_report("forktest", 0, what);
    return 1;
}

int minx_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    minx_put("forktest: starting, pid ");
    minx_put_num(minx_getpid());
    minx_put("\n");

    int fds[CHILDREN][2];
    long pids[CHILDREN];

    /* 1. One pipe per child, and every child writing to its own. */
    for (int i = 0; i < CHILDREN; i++) {
        long r = minx_pipe(fds[i]);

        if (r != 0)
            return fail("pipe() refused");

        pids[i] = minx_fork();

        if (pids[i] < 0)
            return fail("fork() refused");

        if (pids[i] == 0) {
            /* The child.  It owns the write end of pipe i and nothing else it cares about;
             * the read end has to be closed or the parent never sees end-of-file. */
            minx_close(fds[i][0]);

            unsigned char buf[BYTES_FOR(CHILDREN - 1)];

            for (unsigned long k = 0; k < (unsigned long)BYTES_FOR(i); k++)
                buf[k] = pat(k);

            unsigned long left = BYTES_FOR(i);

            while (left > 0) {
                long w = minx_write(fds[i][1], buf, left);

                if (w <= 0)
                    minx_exit(90 + i);
                left -= (unsigned long)w;
            }

            minx_close(fds[i][1]);
            /* The status the parent checks.  Different for every child, so a `wait` that
             * returned the wrong child's status cannot pass. */
            minx_exit(i + 1);
        }
    }

    /* 2. Drain every pipe, in the order they happen to finish rather than in child order,
     *    because that is what a shell does and what the scheduler actually does. */
    int got[CHILDREN];

    for (int i = 0; i < CHILDREN; i++) {
        got[i] = 0;
        minx_close(fds[i][1]);
    }

    int seen[CHILDREN];
    int open_pipes = CHILDREN;

    while (open_pipes > 0) {
        int progressed = 0;

        for (int i = 0; i < CHILDREN; i++) {
            if (seen[i] || got[i] >= (int)BYTES_FOR(i))
                continue;

            unsigned char buf[BYTES_FOR(CHILDREN - 1)];
            long r = minx_read(fds[i][0], buf, BYTES_FOR(i) - (unsigned long)got[i]);

            if (r < 0)
                return fail("read() failed on a pipe with a writer");

            if (r == 0) {
                /* End of file, which only happens once the child has closed its end. */
                if (got[i] != BYTES_FOR(i))
                    return fail("a pipe reached end of file short of its bytes");
                seen[i] = 1;
                open_pipes--;
                progressed = 1;
                continue;
            }

            unsigned long sum = 0;

            for (long k = 0; k < r; k++)
                sum += buf[k];

            if (expect_sum(i) - pat_sum((unsigned long)got[i]) != sum)
                return fail("a pipe delivered bytes that were not written to it");

            got[i] += (int)r;
            progressed = 1;

            if (got[i] == BYTES_FOR(i)) {
                minx_close(fds[i][0]);
                seen[i] = 1;
                open_pipes--;
            }
        }

        if (!progressed)
            return fail("every pipe was open and none of them made progress");
    }

    for (int i = 0; i < CHILDREN; i++) {
        if (got[i] != BYTES_FOR(i))
            return fail("a pipe delivered the wrong number of bytes");
    }

    minx_put("forktest: four pipes drained, ");
    minx_put_num((unsigned long)CHILDREN);
    minx_put(" children\n");

    /* 3. Reap them all, and check every status. */
    for (int n = 0; n < CHILDREN; n++) {
        /* One slot, not an indexed one.
         *
         * `wait` returns whichever child has exited, and that is not the child this loop
         * is on: four children finish in whatever order the scheduler picks, and `wait` is
         * specified to hand back the first it finds rather than the first that was made.
         * So the status has to be stored where the *returned pid* can be matched to it --
         * writing into status[n] and reading status[found] reads an entry nothing ever
         * wrote, which is whatever the stack held. */
        int st = -1;
        long pid = minx_wait(&st);

        if (pid < 0)
            return fail("wait() found no children left while some were uncollected");

        int found = -1;

        for (int i = 0; i < CHILDREN; i++) {
            if (pids[i] == pid && found < 0)
                found = i;
        }

        if (found < 0)
            return fail("wait() returned a pid that was never forked");

        /* The status word, not the exit code.
         *
         * POSIX puts a normal exit in the *high byte* so that the low seven bits can say
         * "killed by signal N" instead, and `WEXITSTATUS()` is how a program gets the code
         * back out.  Comparing the raw word against a bare exit number only ever worked
         * because the kernel reported the code itself -- which also meant a child that
         * exited 1 was indistinguishable from one killed by SIGHUP.  The kernel is right
         * and this comparison was the thing that was wrong. */
        int status = (st >> 8) & 0xff;

        if (status != found + 1) {
            minx_put("forktest: child index ");
            minx_put_num((unsigned long)found);
            minx_put(" should have exited ");
            minx_put_signed(found + 1);
            minx_put(" and exited ");
            minx_put_signed(status);
            minx_put("\n");
            return fail("a child's exit status came back as another child's");
        }

        minx_put("forktest: child ");
        minx_put_num(pid);
        minx_put(" exited ");
        minx_put_signed(status);
        minx_put("\n");
    }

    /* 4. With every child collected, `wait` must say so rather than block for ever.  This
     *    is what lets a shell's prompt come back. */
    int leftover = -1;
    long r = minx_wait(&leftover);

    if (r >= 0)
        return fail("wait() returned a child after all four had been collected");

    minx_put("forktest: no children left, wait() reported that\n");

    /* 5. The child of a fork is a different process, and must not be the same identifier
     *    the parent has. */
    for (int i = 0; i < CHILDREN; i++) {
        if (pids[i] == minx_getpid())
            return fail("fork() gave the child its parent's own pid");
    }

    minx_report("forktest", 1, NULL);
    return 0;
}