/* minx - sigtest: what a signal actually does.
 *
 * A signal is the only thing in this kernel that changes a program's mind, and there are
 * five separate things it has to get right.  Each one below is checked separately, because
 * "signals do not work" is not a diagnosis:
 *
 *   1. A handler runs, with the right number as its argument, and the program continues
 *      afterwards with its own state intact.
 *   2. It runs on a stack, at a stack pointer that was the process's before, and returns
 *      to exactly where it was interrupted -- including its registers, which is checked by
 *      computing a value across the handler and reading it afterwards.
 *   3. A signal to another process reaches that process.
 *   4. A blocked signal is *pending*, not lost, and arrives the moment it is unblocked.
 *   5. A signal nobody handles does its default thing: SIGCHLD is ignored, and SIGUSR2
 *      terminates the process it was sent to.  The second of those is checked by forking a
 *      child, because it kills the process that sent it otherwise.
 *
 * The return is zero only if all five passed.
 */
#include "../common/minx_user.h"
#include "../common/print.h"

/* Set by the handler, read by everything afterwards.  `volatile` because a handler runs
 * between two instructions of this program and the compiler has to be told it cannot assume
 * the value is the one it last wrote. */
static volatile int got_usr1;
static volatile int got_usr2;
static volatile long got_arg;
static volatile long kept_across;

/* The handler.
 *
 * It does not call sigreturn: a handler's `return` goes to a three-instruction trampoline
 * the kernel put on the stack, and that calls it.  This is the whole of the ABI, and it is
 * worth being explicit about because it is the part a program cannot see.
 *
 * It also deliberately does nothing else.  A handler that did work -- a write, an
 * allocation, another call -- would be testing the kernel with itself rather than checking
 * that the mechanism delivered anything at all. */
static void on_usr1(long signum) {
    got_usr1++;
    got_arg = signum;
    kept_across = 0x5eed;
}

static void on_usr2(long signum) {
    got_usr2++;
    (void)signum;
}

static int fail(const char *what) {
    minx_report("sigtest", 0, what);
    return 1;
}

int minx_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    minx_put("sigtest: pid ");
    minx_put_num(minx_getpid());
    minx_put("\n");

    struct sigaction act;
    struct sigaction old;

    /* --- 1. a handler runs ------------------------------------------------ */

    got_usr1 = 0;
    got_arg = -1;

    act.handler = (unsigned long)on_usr1;
    act.flags = 0;
    act.mask = 0;

    long r = minx_sigaction(SIGUSR1, &act, &old);

    if (r != 0)
        return fail("sigaction() refused to install a handler");

    if (old.handler != SIG_DFL)
        return fail("a fresh process did not report SIG_DFL as its first disposition");

    /* Installing one must be able to be undone, or a program that installs SIGINT to clean
     * up and then restores the default has no way to say so. */
    struct sigaction dfl;

    dfl.handler = SIG_DFL;
    dfl.flags = 0;
    dfl.mask = 0;

    if (minx_sigaction(SIGUSR1, &dfl, &old) != 0)
        return fail("sigaction() refused to restore the default");

    if (old.handler != (unsigned long)on_usr1)
        return fail("restoring the default did not report the handler it replaced");

    if (minx_sigaction(SIGUSR1, &act, NULL) != 0)
        return fail("sigaction() refused to reinstall the handler");

    /* --- 2. it runs, with the right argument, and returns ----------------- */

    kept_across = 0x1234;

    r = minx_kill(minx_getpid(), SIGUSR1);

    if (r != 0)
        return fail("kill() refused a signal to this process");

    if (got_usr1 != 1) {
        minx_put("sigtest: the handler ran ");
        minx_put_num((unsigned long)got_usr1);
        minx_put(" times\n");
        return fail("a signal to this process was not delivered");
    }

    if (got_arg != SIGUSR1)
        return fail("the handler's argument was not the signal number");

    if (kept_across != 0x5eed)
        return fail("the process's own state did not survive the handler");

    minx_put("sigtest: SIGUSR1 delivered, handler ran with ");
    minx_put_signed(got_arg);
    minx_put("\n");

    /* Registers across the handler, which is what makes it a *return* rather than a fresh
     * entry: the value is computed before, the handler does arithmetic of its own, and the
     * value is read again after.  Anything that restored the frame wrongly shows up here
     * and nowhere else. */
    {
        volatile long a = 11, b = 17;
        volatile long product = a * b;

        minx_kill(minx_getpid(), SIGUSR1);

        if (product != 187)
            return fail("a computation across a handler came back wrong");
    }

    /* --- 4. a blocked signal is pending, not lost ------------------------- */

    unsigned long set = 1ul << (SIGUSR1 - 1);
    unsigned long before = 0;
    unsigned long pending = 0;

    if (minx_sigprocmask(0 /* SIG_BLOCK */, &set, &before) != 0)
        return fail("sigprocmask() refused to block");

    /* `before` is the mask as it was *before* the call, so it is the wrong thing to look at
     * for what the call did.  Asking for the current mask with a NULL set is the question
     * that has the right answer, and checking the wrong one is how a working sigprocmask
     * gets reported as broken. */
    {
        unsigned long now = 0;

        if (minx_sigprocmask(1, NULL, &now) != 0)
            return fail("sigprocmask() refused to report the current mask");

        if (!(now & (1ul << (SIGUSR1 - 1))))
            return fail("SIGUSR1 was blocked and the mask does not say so");

        if (before & (1ul << (SIGUSR1 - 1)))
            return fail("the mask was reported as already containing a signal that "
                        "was not blocked");

        before = now;
    }

    got_usr1 = 0;

    if (minx_kill(minx_getpid(), SIGUSR1) != 0)
        return fail("kill() refused while SIGUSR1 was blocked");

    /* Three statements, and the handler must not have run for any of them.  The signal is
     * *queued*; a kernel that dropped it would look exactly like one that had not yet
     * delivered it, until the unblocking below is where the two differ. */
    if (got_usr1 != 0)
        return fail("a blocked signal ran its handler anyway");

    if (minx_sigpending(&pending) != 0)
        return fail("sigpending() failed");

    if (!(pending & (1ul << (SIGUSR1 - 1))))
        return fail("a blocked signal was not reported as pending");

    if (minx_kill(minx_getpid(), SIGUSR1) != 0)
        return fail("kill() refused a second blocked signal");

    if (minx_sigpending(&pending) != 0)
        return fail("sigpending() failed the second time");

    /* Two of the same signal are one pending bit, which is POSIX's rule and is what stops a
     * program that ignores ^C in a loop from building an unbounded queue. */
    if ((pending & (1ul << (SIGUSR1 - 1))) == 0)
        return fail("the second blocked signal was lost");

    minx_put("sigtest: SIGUSR1 blocked and pending\n");

    if (minx_sigprocmask(1 /* SIG_UNBLOCK */, &set, NULL) != 0)
        return fail("sigprocmask() refused to unblock");

    /* The signal was already queued, so it is delivered at the very next return to ring 3 --
     * which is the `sigprocmask` call itself.  Nothing here has to wait for it. */
    if (got_usr1 != 1)
        return fail("unblocking did not deliver the signal that was pending");

    if (minx_sigpending(&pending) != 0 || (pending & (1ul << (SIGUSR1 - 1))))
        return fail("a delivered signal is still reported as pending");

    minx_put("sigtest: unblocking delivered it, and it is no longer pending\n");

    /* --- 5. the default for an unhandled signal --------------------------- */

    /* SIGCHLD's default is to be ignored, which is the one default that is *not* death, and
     * a kernel that got it wrong would kill the process that receives one. */
    if (minx_sigaction(SIGCHLD, &dfl, NULL) != 0)
        return fail("sigaction() refused SIGCHLD");

    if (minx_kill(minx_getpid(), SIGCHLD) != 0)
        return fail("kill() refused SIGCHLD");

    minx_put("sigtest: an unhandled SIGCHLD did not kill this process\n");

    /* --- 3. and 5, together: a signal to another process, whose default kills it --- */

    long pid = minx_fork();

    if (pid < 0)
        return fail("fork() refused");

    if (pid == 0) {
        /* The child.  It installs nothing, so SIGUSR2's default applies, and the kernel
         * has to kill it where it stands. */
        got_usr2 = 0;
        minx_put("sigtest: child ");
        minx_put_num(minx_getpid());
        minx_put(" waiting for SIGUSR2\n");

        for (volatile unsigned long spin = 0; spin < 200000000ul; spin++)
            ;

        /* Reaching this line at all is the failure: SIGUSR2 should have ended the child. */
        minx_put("sigtest: the child survived a SIGUSR2 that should have killed it\n");
        minx_exit(70);
    }

    /* Give the child time to start, so the signal is not sent to a task that has not been
     * created yet.  There is no `waitpid` with a timeout in this ABI, and a sleep syscall
     * arrives with the scheduler; a bounded spin in the parent is the honest stand-in, and
     * the child's own output line is the evidence that it started. */
    for (volatile unsigned long spin = 0; spin < 20000000ul; spin++)
        ;

    if (minx_kill(pid, SIGUSR2) != 0)
        return fail("kill() refused a signal to another process");

    int status = -1;
    long reaped = minx_wait(&status);

    if (reaped != pid)
        return fail("wait() did not return the signalled child");

    if (status == 70) {
        minx_put("sigtest: the child exited 70: it was never killed\n");
        return fail("a signal with no handler did not do its default thing");
    }

    minx_put("sigtest: the child died of SIGUSR2, status ");
    minx_put_signed(status);
    minx_put("\n");

    /* A handler installed in the parent must survive all of the above: every one of those
     * signals was delivered to this process or refused, and a handler that quietly stopped
     * being installed would show up as the count not moving. */
    got_usr1 = 0;

    if (minx_kill(minx_getpid(), SIGUSR1) != 0 || got_usr1 != 1)
        return fail("the handler installed at the start was not still installed at the end");

    (void)on_usr2;
    minx_report("sigtest", 1, NULL);
    return 0;
}