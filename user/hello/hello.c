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
    minx_write(1, &buf[i + 1], (unsigned long)(23 - i));
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

    put("hello: ok\n");
    return 0;
}
