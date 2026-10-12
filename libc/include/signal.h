/* minx - <signal.h>
 *
 * Signals, and the one model this kernel has: **one handler at a time per signal number**.
 *
 * `SIG_DFL` and `SIG_IGN` are 0 and 1, which is not arbitrary -- they are the two values
 * that cannot be a userspace address, so a kernel that sees one in the slot knows what it
 * means without a separate flag and without having to trust a pointer it has never
 * dereferenced.  Every other value is an address in the process's own space.
 *
 * All signals are blocked for the duration of a handler.  There is no signal mask within a
 * handler, so a handler that is interrupted by the signal it handles is a handler that
 * needs to decide for itself whether it can be re-entered -- which is a real thing to have
 * to think about, and is recorded in docs/DECISIONS.md as the cost of the simpler model.
 */
#ifndef MINX_SIGNAL_H
#define MINX_SIGNAL_H

#include <sys/sysdep.h>

/* The numbers.  The same as every other Unix's where there is one, which is what makes a
 * program's `SIGTERM` mean what it says. */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGWINCH  28

#define NSIG      32

/* The two dispositions that are not addresses.  See the header comment for why they are
 * these two numbers. */
#define SIG_ERR   ((void (*)(int))-1)
#define SIG_DFL   ((void (*)(int))0)
#define SIG_IGN   ((void (*)(int))1)

typedef void (*sighandler_t)(int);

/* A set of signals, one bit each.  `unsigned long` rather than an array of bytes because
 * `sigemptyset()` and `sigaddset()` then need no allocation and cannot fail, and because
 * the kernel copies the whole thing in one `copy_from_user` -- a fixed-size word is a
 * copy with a size the kernel already knows. */
typedef unsigned long sigset_t;

/* What a program has to keep across a signal handler for `siglongjmp()` to be able to
 * return from one: the handlers, the blocked set, and the return address.  A `sigjmp_buf`
 * is larger than a `jmp_buf` because of that, and a program that uses the smaller one to
 * leave a handler does so by leaving the *stack* back to where it was, not the handler. */
typedef unsigned long sigjmp_buf[10];

struct sigaction {
    sighandler_t sa_handler;
    unsigned long sa_flags;
    sigset_t      sa_mask;       /* the set of signals to block while the handler runs */
};

#define SA_NOCLDSTOP 0x0001
#define SA_RESTART   0x0002
#define SA_RESETHAND 0x0004

/* --- the calls --------------------------------------------------------------- */

sighandler_t signal(int signum, sighandler_t handler);
int sigaction(int signum, const struct sigaction *act, struct sigaction *old);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int sigpending(sigset_t *out);
/* How `sigprocmask()` changes the blocked set: add to it, remove from it, or replace it
 * outright.  `SIG_SETMASK` with a NULL set *asks* for the current mask, which is a question
 * with an answer and no side effect -- and that is what `sigsetjmp()` uses it for. */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

int sigemptyset(sigset_t *out);
int sigfillset(sigset_t *out);
int sigaddset(sigset_t *out, int signum);
int sigdelset(sigset_t *out, int signum);
int sigismember(const sigset_t *set, int signum);

int kill(int pid, int signum);
int raise(int signum);

/* The names, for `perror()` and for a shell's message when a child dies.  These are the
 * strings without the "SIG" prefix, because that is what every Unix's are. */
const char *strsignal(int signum);

/* The three defaults that are worth naming, because "what does this kernel do when nothing
 * handles it" is a question with three different answers. */
#define SIG_DFL_IGNORE   0
#define SIG_DFL_TERM     1
#define SIG_DFL_CORE     2

/* What a program asks about a signal's default action. */
int sig_default_action(int signum);

#endif /* MINX_SIGNAL_H */
