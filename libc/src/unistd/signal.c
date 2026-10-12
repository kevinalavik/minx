/* minx - signals.
 *
 * The model is in <signal.h>: one handler per signal number, `SIG_DFL` is 0 and `SIG_IGN`
 * is 1, and all signals are blocked for the duration of a handler.
 *
 * The kernel keeps the handler in the *task*, not here, and that is the right place for it:
 * a process that forks must not inherit its parent's handlers by accident, and a process
 * that execs must start with none of them.  Both are the same statement -- the disposition
 * is the process's, and `fork()` copies the address space but the kernel decides what a
 * child inherits -- and it is why every function in this file is a pass-through.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

sighandler_t signal(int signum, sighandler_t handler) {
    struct sigaction act;
    struct sigaction old;

    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    act.sa_flags   = SA_RESETHAND;

    if (sigaction(signum, &act, &old) != 0)
        return SIG_ERR;

    /* `signal()` is specified to return the *previous* disposition, so a program can put
     * it back.  `SIG_ERR` is -1, which is not a userspace address, which is the same
     * reason `SIG_DFL` is 0 and `SIG_IGN` is 1. */
    return old.sa_handler;
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *old) {
    return (int)__syscall3(SYS_SIGACTION, signum, (long)(uintptr_t)act,
                           (long)(uintptr_t)old);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    return (int)__syscall3(SYS_SIGPROCMASK, how, (long)(uintptr_t)set,
                           (long)(uintptr_t)old);
}

int sigpending(sigset_t *out) {
    return (int)__syscall1(SYS_SIGPENDING, (long)(uintptr_t)out);
}

int kill(int pid, int signum) {
    return (int)__syscall2(SYS_KILL, pid, signum);
}

/* `raise()` is `kill(getpid(), sig)`, and it is spelled out rather than defined that way so
 * that the obvious question has an obvious answer: a signal raised this way goes through
 * the same path as one sent from another process, including the "blocked signals are
 * pending, not lost" rule.  A `raise()` that bypassed the kernel's queue would be a second
 * delivery mechanism with its own rules, and a handler that behaved differently depending
 * on which one was used would be a bug nobody could reproduce. */
int raise(int signum) {
    return kill(getpid(), signum);
}

/* --- the sets --------------------------------------------------------------------- */

/* One bit per signal, in an `unsigned long`, which is 64 signals on this target and which
 * is enough for every signal number this kernel has.  A set is a plain value rather than
 * an object, so `sigemptyset()` cannot fail and never allocates. */
int sigemptyset(sigset_t *out) {
    if (out == NULL) {
        errno = EFAULT;
        return -1;
    }

    *out = 0;
    return 0;
}

int sigfillset(sigset_t *out) {
    if (out == NULL) {
        errno = EFAULT;
        return -1;
    }

    /* Every bit, not just the ones that are signals.  A set with spare bits set is not a
     * problem -- `sigprocmask()` masks against the signals it knows -- and a `sigfillset`
     * that had to know which numbers are real would be a function whose answer changes with
     * the kernel. */
    *out = ~(sigset_t)0;
    return 0;
}

/* The bounds check is what makes these two total functions.  `sigaddset()` with a signal
 * number of 64 would otherwise shift by 64 and produce 0 -- silently, on a 64-bit target,
 * where the shift is defined -- and the caller would believe a signal was blocked that is
 * not. */
int sigaddset(sigset_t *out, int signum) {
    if (out == NULL) {
        errno = EFAULT;
        return -1;
    }

    if (signum <= 0 || signum >= (int)(sizeof(sigset_t) * 8)) {
        errno = EINVAL;
        return -1;
    }

    *out |= (sigset_t)1 << signum;
    return 0;
}

int sigdelset(sigset_t *out, int signum) {
    if (out == NULL) {
        errno = EFAULT;
        return -1;
    }

    if (signum <= 0 || signum >= (int)(sizeof(sigset_t) * 8)) {
        errno = EINVAL;
        return -1;
    }

    *out &= ~((sigset_t)1 << signum);
    return 0;
}

int sigismember(const sigset_t *set, int signum) {
    if (set == NULL || signum < 0 || signum >= (int)(sizeof(sigset_t) * 8))
        return -1;

    return (*set & ((sigset_t)1 << signum)) != 0;
}

/* --- the names --------------------------------------------------------------------- */

/* One table for the signals this kernel has, and a lookup that returns a *stable* string
 * for one it does not.
 *
 * Stable is the important word: the buffer is reused, so `fprintf(stderr, "%s %s", a,
 * strsignal(sig), b)` with the same signal twice prints one name twice rather than the
 * same buffer twice.  A caller that needs the string to survive another call copies it. */
const char *strsignal(int signum) {
    static char unknown[16];
    static const char *names[] = {
        [0]  = "signal 0",
        [SIGHUP]  = "hangup",
        [SIGINT]  = "interrupt",
        [SIGQUIT] = "quit",
        [SIGILL]  = "illegal instruction",
        [SIGTRAP] = "trace/breakpoint trap",
        [SIGABRT] = "aborted",
        [SIGBUS]  = "bus error",
        [SIGFPE]  = "floating point exception",
        [SIGKILL] = "killed",
        [SIGUSR1] = "user defined signal 1",
        [SIGSEGV] = "segmentation fault",
        [SIGUSR2] = "user defined signal 2",
        [SIGPIPE] = "broken pipe",
        [SIGALRM] = "alarm clock",
        [SIGTERM] = "terminated",
        [SIGCHLD] = "child exited",
        [SIGCONT] = "continued",
        [SIGSTOP] = "stopped (signal)",
        [SIGTSTP] = "stopped",
        [SIGWINCH] = "window size changed",
    };

    if (signum < 0 || signum >= (int)(sizeof(names) / sizeof(names[0])))
        return NULL;

    if (names[signum] != NULL)
        return names[signum];

    /* A gap in the table is a signal this kernel knows the number of but has not named.
     * Saying so is better than returning NULL, which a caller printing it with `%s` would
     * turn into "(null)" or a crash. */
    snprintf(unknown, sizeof(unknown), "signal %d", signum);

    return unknown;
}

/* --- what a signal does when nothing handles it ------------------------------------------ */

/* Three defaults, and a signal is in exactly one of them.  A program asking this is asking
 * "what will happen if I do nothing", and the answer is the only thing that matters about a
 * signal a program has not handled. */
int sig_default_action(int signum) {
    switch (signum) {
    /* Ignored: the signal is delivered and does nothing.  SIGCHLD is here because a
     * program's children exiting is not an error, and every Unix has agreed on that since
     * long before any of them had a `waitpid()` that could block. */
    case SIGCHLD:
        return SIG_DFL_IGNORE;

    /* Fatal, with a core.  A fault is in this group because the default *is* to die and a
     * debugger wants the core; there is no debugger here, so the difference is only what
     * `sigaction()` reports. */
    case SIGILL:
    case SIGABRT:
    case SIGBUS:
    case SIGFPE:
    case SIGSEGV:
        return SIG_DFL_CORE;

    /* Fatal, quietly. */
    case SIGHUP:
    case SIGINT:
    case SIGQUIT:
    case SIGKILL:
    case SIGUSR1:
    case SIGUSR2:
    case SIGPIPE:
    case SIGALRM:
    case SIGTERM:
    case SIGCONT:
    case SIGSTOP:
    case SIGTSTP:
    case SIGWINCH:
    default:
        return SIG_DFL_TERM;
    }
}