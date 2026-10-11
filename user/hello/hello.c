/* minx - the first user program.
 *
 * Ring 3, and the first thing in this kernel that has ever run there.  It does three
 * things that between them check the whole boundary:
 *
 *   1. Reads its own arguments off the stack the loader built.  A program that only
 *      writes to a descriptor runs happily on a stack that is merely wrong; one that
 *      reads argc and argv is how a broken loader is noticed.
 *   2. Writes to a descriptor, which is the round trip through `int $0x80` -- the trap
 *      into the kernel, the argument check, the console, and the result coming back.
 *   3. Writes to a *bad* address as well, and checks that the kernel refused it rather
 *      than faulting or, worse, reading it.  A boundary that has only been tested with
 *      good input has not been tested.
 */
/* The ABI is shared by every user program, so it lives one directory up: a second program
 * should not have to copy a syscall wrapper to be able to make one. */
#include "../common/minx_user.h"

static void put(const char *s) {
    minx_write(1, s, minx_strlen(s));
}

static void put_num(unsigned long v) {
    char buf[24];
    int i = 23;

    buf[i--] = 0;
    if (v == 0)
        buf[i--] = '0';

    while (v != 0) {
        buf[i--] = (char)('0' + (v % 10));
        v /= 10;
    }
    /* The digits are buf[i+1 .. 22], which is `22 - i` of them -- not `23 - i`.
     *
     * The off-by-one sends the terminator as well, and it is invisible here: a NUL
     * printed to a terminal moves nothing, so the numbers all read correctly while the
     * program is quietly writing bytes nobody asked for.  It was not invisible to
     * `make test`, whose tally grep reads the serial log and treats a file with NULs in
     * it as binary and declines to report what it found -- so four correct-looking
     * numbers failed the whole suite. */
    minx_write(1, &buf[i + 1], (unsigned long)(22 - i));
}

/* The byte the copy-on-write checks below argue over.
 *
 * `volatile` because the whole point is that the child's write must not be visible here,
 * and the compiler is entitled to cache a non-volatile static in a register across a call
 * it believes cannot have changed it.  Making it volatile means the read after `wait` is a
 * real load of the real byte -- so a kernel that shared the page instead of copying it
 * would be caught rather than hidden by the optimiser.
 *
 * Non-zero initialised, so a page that was never written still differs from a page of
 * zeroes, and a byte that was never copied reads as something impossible. */
static volatile unsigned char cow_byte = 0xa5;

/* The byte a pipe test uses, at every position.  A function of the position rather than a
 * constant, so a pipe that returned the same byte twice -- a ring that wrapped into itself,
 * a length that came back wrong -- fails the checksum instead of passing a comparison
 * against a repeated string. */
static unsigned char pat(unsigned long i) {
    return (unsigned char)(i * 7u + 3u);
}

/* What `pat` sums to over `want` bytes.  Both sides of the pipe compute this from the same
 * definition rather than being handed the other's answer, so a kernel that moved every
 * byte correctly but reordered them is caught. */
static unsigned long pat_sum(unsigned long want) {
    unsigned long s = 0;

    for (unsigned long i = 0; i < want; i++)
        s += pat(i);

    return s;
}

/* --- pipes ---------------------------------------------------------------- */

/* One process, one pipe: what it stores and what it says when there is nothing more.
 *
 * Returned rather than acted on, so a failure here names which check failed instead of
 * printing a line the caller has to interpret. */
static int pipe_oneshot(void) {
    int fds[2];
    char buf[32];

    if (minx_pipe(fds) != 0) {
        put("pipe: FAILED, the kernel refused to make one\n");
        return 0;
    }

    /* The two ends are new numbers.  They could be 0 and 1 if the kernel handed back
     * descriptors that were already open, which is how a program ends up writing its pipe
     * to the console. */
    if (fds[0] < 3 || fds[1] < 3 || fds[0] == fds[1]) {
        put("pipe: FAILED, the ends are not two new descriptors\n");
        return 0;
    }

    long w = minx_write(fds[1], "hello", 5);
    if (w != 5) {
        put("pipe: FAILED, the write did not take five bytes\n");
        return 0;
    }

    long r = minx_read(fds[0], buf, 32);
    if (r != 5) {
        put("pipe: FAILED, the read did not take five bytes\n");
        return 0;
    }

    for (int i = 0; i < 5; i++) {
        if (buf[i] != "hello"[i]) {
            put("pipe: FAILED, what came back is not what went in\n");
            return 0;
        }
    }

    /* Each end only does its own job.  A read end that accepted a write would either drop
     * the data or block for ever, and a write end that accepted a read would hang; the
     * kernel has to say no instead. */
    if (minx_write(fds[0], "x", 1) != -EBADF) {
        put("pipe: FAILED, the read end accepted a write\n");
        return 0;
    }
    if (minx_read(fds[1], buf, 1) != -EBADF) {
        put("pipe: FAILED, the write end accepted a read\n");
        return 0;
    }

    /* Nothing buffered and the write end gone: end of file, as zero bytes.  This is what
     * makes a program reading a pipe from a process that finished terminate rather than
     * wait for ever on a condition nobody can satisfy. */
    if (minx_close(fds[1]) != 0) {
        put("pipe: FAILED, closing the write end was refused\n");
        return 0;
    }
    if (minx_read(fds[0], buf, 32) != 0) {
        put("pipe: FAILED, a closed write end did not report end of file\n");
        return 0;
    }

    /* The console is not the program's to close, and a number nobody opened is not a
     * descriptor. */
    if (minx_close(1) != -EBADF) {
        put("pipe: FAILED, closing the console was allowed\n");
        return 0;
    }
    if (minx_close(fds[0] + 40) != -EBADF) {
        put("pipe: FAILED, closing a descriptor nobody opened was allowed\n");
        return 0;
    }

    /* Both ends gone, so the pipe is finished with -- and the next `pipe` gets these same
     * numbers back, which is itself worth having checked by accident. */
    if (minx_close(fds[0]) != 0) {
        put("pipe: FAILED, closing the last end was refused\n");
        return 0;
    }

    return 1;
}

/* More than the pipe can hold, across a fork, which is the only way to see that a full
 * pipe makes the writer wait and an empty one makes the reader wait.
 *
 * Neither half can be checked from a single process: the writer would block with nothing
 * able to drain it and the reader would block with nothing able to fill it.  So the parent
 * writes and the child reads, and each of them has to see all of it. */
static int pipe_across_fork(void) {
    /* Three times the capacity, plus a bit that is not a multiple of anything, so the
     * ring's wrap is crossed at least once and the last write is short. */
    const unsigned long want = 3ul * 4096ul + 100ul;
    const unsigned long expect = pat_sum(want);
    int fds[2];

    if (minx_pipe(fds) != 0) {
        put("pipe: FAILED, the kernel refused to make one\n");
        return 0;
    }
    long child = minx_fork();

    if (child < 0) {
        put("pipe: FAILED, the kernel refused to fork\n");
        return 0;
    }

    if (child == 0) {
        /* The reader.  It gives up its inherited write end, or the end of file would
         * never arrive: the pipe counts ends, not processes, and the parent's copy is one
         * more. */
        unsigned long total = 0;
        unsigned long sum = 0;
        char buf[256];

        minx_close(fds[1]);

        for (;;) {
            long r = minx_read(fds[0], buf, 256);

            if (r < 0) {
                put("pipe: FAILED, the child's read on fd ");
                put_num((unsigned long)fds[0]);
                put(" was refused with -");
                put_num((unsigned long)(-r));
                put("\n");
                minx_exit(3);
            }
            if (r == 0)
                break;              /* end of file: every byte arrived */              /* end of file: every byte arrived */

            /* Cast to unsigned: `char` is signed here, so a byte above 127 would
             * sign-extend into a huge number and the checksum would not be comparing the
             * bytes that were sent.  The pattern deliberately produces those bytes --
             * half of them, in fact -- so this is not a corner case that only shows up
             * with unlucky data. */
            for (long i = 0; i < r; i++)
                sum += (unsigned char)buf[i];

            total += (unsigned long)r;

            /* A dot every page of progress, from user space.  Two processes passing
             * 12388 bytes through one 4096-byte pipe have to alternate, so nothing in the
             * kernel prints -- all this shows is whether the two halves are actually
             * getting to each other, which is the whole question a hang here would raise. */
        }

        minx_close(fds[0]);

        if (total != want) {
            put("pipe: FAILED, the child got ");
            put_num(total);
            put(" of ");
            put_num(want);
            put("\n");
            minx_exit(4);
        }
        if (sum != expect) {
            put("pipe: FAILED, the bytes that arrived were not the ones sent\n");
            minx_exit(5);
        }

        minx_exit(0);
    }

    /* The writer.  Its inherited read end goes too, so the child is the only reader and
     * the pipe has exactly two ends for the two of them. */
    minx_close(fds[0]);

    {
        unsigned long total = 0;
        char buf[256];

        while (total < want) {
            /* Never more than is left, so the last write is short rather than rounding
             * `want` up to a whole number of chunks.  A pipe hands back everything it was
             * asked for when it has room, so a writer that rounds up is asking for more
             * than it means to send -- and the reader, which counts what arrives, is the
             * one that catches it. */
            size_t_ ask = 256;

            if (want - total < (unsigned long)ask)
                ask = (size_t_)(want - total);

            for (size_t_ i = 0; i < ask; i++)
                buf[i] = (char)pat(total + i);

            long w = minx_write(fds[1], buf, ask);

            if (w < 0) {
                put("pipe: FAILED, the parent's write was refused\n");
                return 0;
            }
            if (w == 0) {
                /* A zero-byte write to a pipe with room is a pipe that lost its buffer. */
                put("pipe: FAILED, a write with room in the pipe moved nothing\n");
                return 0;
            }

            total += (unsigned long)w;

        }
    }

    minx_close(fds[1]);

    int status = -1;

    if (minx_wait(&status) != child) {
        put("pipe: FAILED, wait returned the wrong child\n");
        return 0;
    }
    if (status != 0) {
        put("pipe: FAILED, the child reported ");
        put_num((unsigned long)status);
        put("\n");
        return 0;
    }

    return 1;
}

static int pipe_checks(void) {
    if (!pipe_oneshot())
        return 0;

    if (!pipe_across_fork())
        return 0;

    put("pipe: ok\n");
    return 1;
}

int minx_main(unsigned long argc, char **argv);

int minx_main(unsigned long argc, char **argv) {
    put("hello from ring 3\n");

    put("argc=");
    put_num(argc);
    put(" argv0=");

    if (argc > 0 && argv != 0 && argv[0] != 0)
        minx_write(1, argv[0], minx_strlen(argv[0]));
    else
        put("(none)");
    put("\n");

    /* A descriptor that exists: the console. */
    long wrote = minx_write(1, "write: ok\n", 10);
    if (wrote != 10) {
        put("write: FAILED\n");
        return 1;
    }

    /* A descriptor that does not: the kernel must say so rather than quietly succeed,
     * because a program that believes it wrote somewhere is worse off than one that
     * knows it did not. */
    long bad_fd = minx_write(99, "should not appear\n", 18);
    if (bad_fd >= 0) {
        put("open fd: FAILED, the kernel accepted a bad descriptor\n");
        return 1;
    }
    /* And it has to be *this* error.  "Some error" was all this asked for until the
     * kernel started answering EBADF with EFAULT: every negative return was being
     * compared unsigned against the top of user space, which a negative number passes,
     * so the boundary's own sanity check was rewriting one error into another and no
     * test here could see it. */
    if (bad_fd != -EBADF) {
        put("open fd: FAILED, expected EBADF\n");
        return 1;
    }
    put("bad fd: refused with -");
    put_num((unsigned long)(-bad_fd));
    put("\n");

    /* A pointer that is not mapped.  This is the check that matters most: the kernel
     * must not read through it, and must not take a fault to find that out. */
    long bad_ptr = minx_write(1, (const void *)0x10, 16);
    if (bad_ptr >= 0) {
        put("bad pointer: FAILED, the kernel read it\n");
        return 1;
    }
    put("bad pointer: refused with -");
    put_num((unsigned long)(-bad_ptr));
    put("\n");

    /* A pointer in the kernel's own half of the address space: still not readable from
     * ring 3, and a kernel that checked only "is it mapped" would say yes. */
    long kern_ptr = minx_write(1, (const void *)0xffffffff80000000ull, 16);
    if (kern_ptr >= 0) {
        put("kernel pointer: FAILED, the kernel read a kernel address\n");
        return 1;
    }
    put("kernel pointer: refused\n");

    /* An implementation that does not exist yet. */
    long nosys = minx_syscall1(999, 0);
    if (nosys != -38) {
        put("unknown call: FAILED\n");
        return 1;
    }
    put("unknown call: ENOSYS\n");

    long pid = minx_getpid();
    put("pid=");
    put_num((unsigned long)pid);
    put("\n");

    /* --- fork and copy-on-write ------------------------------------------
     *
     * The property under test is that two processes can hold the same page and each get a
     * private copy of it the first time one of them writes.  Sharing it outright is the
     * bug; copying everything at `fork` is merely expensive.  Only one of those two
     * mistakes is visible to a program, and it is the one that matters. */
    cow_byte = 'P';
    long child = minx_fork();

    if (child < 0) {
        put("fork: FAILED, the kernel refused\n");
        return 1;
    }

    if (child == 0) {
        /* The child: running this same code, one instruction after `fork`, with every
         * register but the return value the same.  It inherits the byte, writes its own
         * value -- a fault on a page it shares with its parent -- and exits with a status
         * the parent reads back.
         *
         * The stack writes around this call matter as much as `cow_byte`: the child's
         * frames are on the same user stack pages, so returning out of this frame is a
         * second copy-on-write fault, on a page with nothing to do with the argument. */
        if (cow_byte != 'P') {
            put("fork: FAILED, the child did not inherit the page\n");
            minx_exit(2);
        }
        cow_byte = 'C';
        minx_exit(7);
    }

    put("fork: child ");
    put_num((unsigned long)child);
    put("\n");

    if (child == minx_getpid()) {
        put("fork: FAILED, the child's identifier is the parent's\n");
        return 1;
    }

    /* The parent writes too, and may do so before or after the child's write.  Both fault
     * on the same page; whichever went second had to take its own copy. */
    cow_byte = 'Q';

    int status = 0;
    long reaped = minx_wait(&status);

    if (reaped != child) {
        put("fork: FAILED, wait returned the wrong child\n");
        return 1;
    }
    if (status != 7) {
        put("fork: FAILED, status ");
        put_num((unsigned long)status);
        put("\n");
        return 1;
    }

    /* The check the whole thing exists for.  If the page had been shared, the child's
     * write would be here. */
    if (cow_byte != 'Q') {
        put("cow: FAILED, the child's write reached the parent\n");
        return 1;
    }

    put("cow: ok\n");

    if (!pipe_checks())
        return 1;

    put("hello: ok\n");
    return 0;
}
