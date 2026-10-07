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
#include "hello.h"

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

    put("hello: ok\n");
    return 0;
}
