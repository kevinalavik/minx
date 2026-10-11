/* minx - signals.
 *
 * A signal is the one way a process is told about something it did not ask about: the end
 * of a terminal line, a child's exit, a write to a pipe nobody reads.  Everything else in
 * this kernel is a call the program made; a signal is the kernel changing the program's
 * mind, and it is the only thing that makes `^C` mean what it means.
 *
 * The model here is deliberately the small one, and the simplifications are named rather
 * than hidden:
 *
 *  - **One handler at a time, no queue of handlers.**  SIG_IGN and SIG_DFL are addresses
 *    (`0` and `1`), not flags in a table, because a program that installs a handler for a
 *    signal and takes it down again should not have to call anything to put it back.
 *  - **A handler runs with every signal blocked.**  Nesting is not attempted: a second
 *    signal that arrives while a handler is running stays pending and is delivered on the
 *    way out.  That is enough for `^C` in a shell and for SIGCHLD, which is all this is
 *    used for, and a nested delivery would need a second saved context.
 *  - **The interrupted context is kept in the task, not on the user stack.**  The signal
 *    frame the handler sees is three quadwords -- the return address, the signal number,
 *    and the saved context pointer -- and `sigreturn` puts everything back into the live
 *    interrupt frame the syscall arrived on.  There is no `siginfo_t` and no `ucontext_t`
 *    because nothing here has a caller that needs one; `docs/SYSCALLS.md` says exactly what
 *    the layout is, and the libc matches it word for word.
 *
 * Delivery happens in exactly one place -- the end of the interrupt dispatcher, which is
 * the last instruction before every return to ring 3 -- because delivering a signal from
 * anywhere else means "somewhere else" has to remember to do it.
 */
#ifndef MINX_SIGNAL_H
#define MINX_SIGNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"
#include "pagetables.h"

/* `task_t` is not included: task.h includes *this* file, for the slot table it embeds.
 * A forward declaration is enough for the two calls that take a task. */
struct task;

/* Signal numbers.  These are the usual Unix numbers, and deliberately so: a program's
 * source says SIGINT and means ^C, and a kernel that invented its own numbering would
 * make every one of them a different number from the one the source says. */
enum {
    SIGHUP  = 1,
    SIGINT  = 2,
    SIGQUIT = 3,
    SIGILL  = 4,
    SIGTRAP = 5,
    SIGABRT = 6,
    SIGBUS  = 7,
    SIGFPE  = 8,
    SIGKILL = 9,
    SIGUSR1 = 10,
    SIGSEGV = 11,
    SIGUSR2 = 12,
    SIGPIPE = 13,
    SIGALRM = 14,
    SIGTERM = 15,
    SIGCHLD = 17,
    SIGCONT = 18,
    SIGSTOP = 19,
    SIGTSTP = 20,
    SIGTTIN = 21,
    SIGTTOU = 22,
    SIGURG  = 23,
    SIGXCPU = 24,
    SIGXFSZ = 25,
    SIGSYS  = 31,
};

/* The highest number this kernel will accept, and the size of every per-task signal
 * field.  64 is enough for every name above and leaves the top for the ones a later
 * phase adds. */
#define MINX_NSIG 64

/* Dispositions, as user addresses, so that "installed", "ignored" and "default" are
 * three distinguishable values a program can hand over without a flag beside them. */
#define SIG_DFL ((uint64_t)0)
#define SIG_IGN ((uint64_t)1)
#define SIG_ERR ((uint64_t)0xffffffffffffffffull)

/* Flags `sigaction` accepts.  Neither is honoured yet; both are accepted so that a libc
 * which sets them by default does not have to be told twice. */
#define SA_NOCLDSTOP 0x00000001ull
#define SA_ONSTACK    0x08000000ull

/* What a program hands over, and what it gets back.  Three quadwords, and the order is
 * the order in `docs/SYSCALLS.md`. */
typedef struct {
    uint64_t handler;
    uint64_t flags;
    uint64_t mask;       /* extra signals blocked while the handler runs */
} sigaction_t;

/* How many handlers one process can have installed at once.
 *
 * A fixed table rather than a per-signal array, because the array is 65 words in every
 * task_t -- 33 KiB of a kernel's worth of processes doing nothing -- while eight slots is
 * 256 bytes and covers every program this kernel runs.  A ninth install fails with
 * EINVAL rather than growing anything on a path that can be called from a signal
 * handler. */
#define SIG_HANDLER_SLOTS 8

typedef struct {
    uint32_t signum;
    uint64_t handler;
    uint64_t flags;
    uint64_t mask;
    bool     used;
} sig_slot_t;

/* Every task carries these.  In task.h rather than here, because task_t embeds them. */

/* Build the signal-return trampoline's page and map it into `tables`.
 *
 * The trampoline is three instructions the kernel writes itself -- `mov eax, SYS_SIGRETURN`
 * and `int $0x80` -- because a signal handler returns to an address the kernel chose, and
 * the only code that can live at an address the kernel chooses is code the kernel wrote.
 * Every user address space gets the same physical page, read-only and executable, so
 * there is exactly one copy of it in the machine. */
int  signal_map_trampoline(page_table_t *tables);

/* Where the trampoline lives in user space.  A fixed low address, above the null guard
 * and far below USER_IMAGE_BASE, so it can never collide with a program's own segments. */
#define SIGNAL_TRAMPOLINE_ADDR 0x0000000000006000ull

/* The calls themselves.  Defined in signal.c and installed by syscall_init(); declared here
 * so the one table that maps numbers to handlers can see them. */
long sys_kill(regs_t *regs);
long sys_sigaction(regs_t *regs);
long sys_sigreturn(regs_t *regs);
long sys_sigprocmask(regs_t *regs);
long sys_sigpending(regs_t *regs);
long sys_pause(regs_t *regs);

/* Build the trampoline page.  Called once from kmain(), after the direct map is live. */
int signal_init(void);

/* Called from the one place every return to ring 3 goes through.  Takes the live frame,
 * delivers a signal if one is pending and unblocked, and patches the frame so the tail's
 * `iretq` enters the handler instead. */
void signal_deliver_on_return_to_user(regs_t *regs);

/* Note a signal as pending for `t`.  Returns 0, or -1 with the reason: a signal that
 * cannot be delivered (SIGKILL's cousin, a dead task, an out-of-range number) has to be
 * reported rather than silently dropped. */
long signal_send(struct task *t, int signum);

/* The default action for a signal nobody has installed a handler for.  Returns what the
 * process should do: terminate, stop, or continue.  The shell asks this so it can print
 * "Terminated" for a child that died of one. */
enum {
    SIGNAL_DEFAULT_IGNORE = 0,
    SIGNAL_DEFAULT_TERMINATE,
    SIGNAL_DEFAULT_STOP,
};
int signal_default_action(int signum);
const char *signal_name(int signum);

#endif /* MINX_SIGNAL_H */