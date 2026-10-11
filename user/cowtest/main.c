/* minx - cowtest: what copy-on-write promises.
 *
 * The promise is narrow and worth stating: a write by one process to a page it shares with
 * its twin is visible to that process alone.  Everything else about copy-on-write is an
 * implementation detail, and a test that checked one of those would be testing the
 * implementation rather than the promise.
 *
 * So this program checks the promise four ways, each of which a shared page would fail and
 * a *silently dropped* write would also fail -- which is the point.  "The child's write was
 * visible here" and "the child's write happened at all" are different bugs, and the checks
 * below tell them apart:
 *
 *   1. The child writes a byte; the parent's copy is unchanged.  A shared page fails here.
 *   2. The parent's copy *does* change when the parent writes it.  A page shared
 *      read-only, or a write that was dropped, fails here.
 *   3. The child writes, then the parent writes, then the child checks *its own* value is
 *      still its own.  Copying one way but not the other fails here.
 *   4. The child writes enough to cover a whole page and to spill into the next, because
 *      the fault that fixes a page up is a page fault: a kernel that copied one page and
 *      never the next would pass the first three checks and fail this one.
 *
 * Everything the checks look at is `volatile`, which is not decoration: the compiler is
 * entitled to keep a non-volatile static in a register across a function call, and a test
 * whose reads were hoisted would pass against a kernel that shares pages.
 */
#include "../common/minx_user.h"
#include "../common/print.h"

/* A page is 4096 bytes; the page below is mapped by the loader as part of the program's
 * data, so writing across this boundary is what makes the *next* page's leaf get copied
 * too. */
static volatile unsigned char page[8192] __attribute__((aligned(4096)));

#define CHILD_BYTE 0x5a
#define PARENT_BYTE 0xa5

static int fail(const char *what) {
    minx_report("cowtest", 0, what);
    return 1;
}

static void fill(unsigned char value) {
    for (unsigned long i = 0; i < sizeof(page); i++)
        page[i] = value;
}

int minx_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    minx_put("cowtest: pid ");
    minx_put_num(minx_getpid());
    minx_put(", page at ");
    minx_put_num((unsigned long)(uintptr_t)page);
    minx_put("\n");

    /* Something distinctive in every byte, so "unchanged" is not the same as "zero". */
    fill(PARENT_BYTE);

    long pid = minx_fork();

    if (pid < 0)
        return fail("fork() refused");

    if (pid == 0) {
        /* The child.
         *
         * Everything below the fork is the parent's memory as it was; everything above is
         * the child's.  The first write is the one that *causes* the copy, so it is the one
         * that must work before any check can mean anything. */
        for (unsigned long i = 0; i < sizeof(page); i++)
            page[i] = CHILD_BYTE;

        /* Written, and visible to itself. */
        if (page[0] != CHILD_BYTE || page[4095] != CHILD_BYTE ||
            page[4096] != CHILD_BYTE || page[sizeof(page) - 1] != CHILD_BYTE)
            minx_exit(41);

        /* Two distinct values, so "the write happened" and "the write landed in this
         * process's page" are separate claims. */
        page[0] = 0x11;
        page[sizeof(page) - 1] = 0x22;
        minx_exit(page[0] == 0x11 && page[sizeof(page) - 1] == 0x22 ? 0 : 42);
    }

    /* The parent waits for the child to be *finished* before looking, because looking
     * earlier would race: the parent might read before the child wrote, and a shared page
     * would then look correct for exactly as long as the race was narrow.  Waiting first
     * turns "the write was visible here" into a statement about what the child did, not
     * about when the parent happened to look. */
    int status = -1;
    long reaped = minx_wait(&status);

    if (reaped != pid)
        return fail("wait() did not return the child that was forked");

    if (status != 0) {
        minx_put("cowtest: the child exited ");
        minx_put_signed(status);
        minx_put("\n");
        return fail("the child's own writes were not visible to the child");
    }

    /* 1. The child's writes are not here. */
    if (page[0] != PARENT_BYTE) {
        minx_put("cowtest: page[0] is ");
        minx_put_num(page[0]);
        minx_put(", expected ");
        minx_put_num(PARENT_BYTE);
        minx_put("\n");
        return fail("the child's write was visible to the parent: the page is shared");
    }

    if (page[sizeof(page) - 1] != PARENT_BYTE)
        return fail("the child's write at the far end was visible to the parent");

    if (page[4096] != PARENT_BYTE)
        return fail("the child's write to the *next* page was visible to the parent");

    minx_put("cowtest: the parent's 8192 bytes survived the child's writes\n");

    /* 2. The parent's own writes still work -- that this page is not now permanently
     *    read-only, which is the failure mode of a copy-on-write that forgets to put the
     *    write bit back. */
    page[0] = 0x33;
    page[sizeof(page) - 1] = 0x44;

    if (page[0] != 0x33 || page[sizeof(page) - 1] != 0x44)
        return fail("the parent's own write to a shared page did not take effect");

    /* And the second write, to the same page the child wrote to, must not have disturbed
     * the child's view -- which the parent cannot see directly, so the child is asked. */
    pid = minx_fork();

    if (pid < 0)
        return fail("the second fork() refused");

    if (pid == 0) {
        /* Inherits the parent's page, which the parent has since written to. */
        if (page[0] != 0x33 || page[sizeof(page) - 1] != 0x44)
            minx_exit(51);

        /* Writes again, so this fork's own write-on-write has to be handled separately
         * from the first child's. */
        page[0] = 0x55;
        minx_exit(page[0] == 0x55 ? 0 : 52);
    }

    reaped = minx_wait(&status);

    if (reaped != pid || status != 0)
        return fail("a page copied twice was not private to both processes");

    if (page[0] != 0x33 || page[sizeof(page) - 1] != 0x44)
        return fail("the second child's writes were visible to the parent");

    minx_report("cowtest", 1, NULL);
    return 0;
}