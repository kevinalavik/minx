/* minx - signals.  See signal.h for the model and what it deliberately does not do. */
#include "signal.h"

#include "console.h"
#include "kprintf.h"
#include "kmalloc.h"
#include "string.h"
#include "syscall.h"
#include "task.h"
#include "vmm.h"

/* --- names ---------------------------------------------------------------- */

static const char *const signal_names[MINX_NSIG + 1] = {
    [SIGHUP] = "SIGHUP",   [SIGINT] = "SIGINT",   [SIGQUIT] = "SIGQUIT",
    [SIGILL] = "SIGILL",   [SIGTRAP] = "SIGTRAP", [SIGABRT] = "SIGABRT",
    [SIGBUS] = "SIGBUS",   [SIGFPE] = "SIGFPE",   [SIGKILL] = "SIGKILL",
    [SIGUSR1] = "SIGUSR1", [SIGSEGV] = "SIGSEGV", [SIGUSR2] = "SIGUSR2",
    [SIGPIPE] = "SIGPIPE", [SIGALRM] = "SIGALRM", [SIGTERM] = "SIGTERM",
    [SIGCHLD] = "SIGCHLD", [SIGCONT] = "SIGCONT", [SIGSTOP] = "SIGSTOP",
    [SIGTSTP] = "SIGTSTP", [SIGTTIN] = "SIGTTIN", [SIGTTOU] = "SIGTTOU",
    [SIGURG] = "SIGURG",   [SIGXCPU] = "SIGXCPU", [SIGXFSZ] = "SIGXFSZ",
    [SIGSYS] = "SIGSYS",
};

const char *signal_name(int signum) {
    if (signum <= 0 || signum > MINX_NSIG)
        return "?";
    return signal_names[signum] != NULL ? signal_names[signum] : "SIG?";
}

int signal_default_action(int signum) {
    switch (signum) {
    case SIGCHLD:
    case SIGCONT:
    case SIGURG:
        return SIGNAL_DEFAULT_IGNORE;

    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return SIGNAL_DEFAULT_STOP;

    default:
        /* Everything else terminates, which is what a signal with no handler installed
         * is *for*: the process is told and it goes away.  A signal whose default were
         * "ignore" would be invisible by default, and the program would carry on with a
         * condition nothing had told it about. */
        return SIGNAL_DEFAULT_TERMINATE;
    }
}

/* --- the return trampoline ------------------------------------------------ */

/* The page holds three instructions at offset 0, and nothing else matters:

 *     b8 <SYS_SIGRETURN>   mov eax, SYS_SIGRETURN
 *     cd 80                int $0x80
 *     f4                   hlt

 * Written by the kernel because the address is the kernel's choice: a handler's return
 * address is written into the task's stack by the delivery path, and whatever sits there
 * has to be code.  Pointing it at a kernel address is not an option -- the `ret` executes
 * it in ring 3, and a kernel address is not mapped there at all, which is a #PF on the
 * instruction *after* the handler, with the handler's return value already gone. */
static uint64_t trampoline_phys;

int signal_map_trampoline(page_table_t *tables) {
    if (trampoline_phys == 0 || tables == NULL)
        return -1;

    /* Read-only and executable, and the same physical page in every address space: there is
     * exactly one copy of these three instructions in the machine, and a process cannot
     * change them because it cannot write them. */
    return vmm_map_into(tables, SIGNAL_TRAMPOLINE_ADDR, trampoline_phys, page_size,
                        false);
}

static int trampoline_build(void) {
    uint64_t phys = vmm_user_page_alloc();

    if (phys == 0)
        return -1;

    uint8_t *code = (uint8_t *)phys_to_virt(phys);

    memset(code, 0xcc, page_size);          /* int3 everywhere else */
    size_t i = 0;
    code[i++] = 0xb8;                        /* mov eax, imm32 */
    uint32_t nr = (uint32_t)SYS_SIGRETURN;
    code[i++] = (uint8_t)(nr & 0xff);
    code[i++] = (uint8_t)((nr >> 8) & 0xff);
    code[i++] = (uint8_t)((nr >> 16) & 0xff);
    code[i++] = (uint8_t)((nr >> 24) & 0xff);
    code[i++] = 0xcd;                        /* int $0x80 */
    code[i++] = 0x80;

    trampoline_phys = phys;
    return 0;
}

/* --- the syscall door ----------------------------------------------------- */

static bool signum_ok(int signum) {
    return signum > 0 && signum <= MINX_NSIG;
}

static sig_slot_t *slot_find(task_t *t, int signum, bool create) {
    sig_slot_t *free_slot = NULL;

    for (unsigned i = 0; i < SIG_HANDLER_SLOTS; i++) {
        if (t->sig_slots[i].used && t->sig_slots[i].signum == (uint32_t)signum)
            return &t->sig_slots[i];
        if (!t->sig_slots[i].used && free_slot == NULL)
            free_slot = &t->sig_slots[i];
    }

    return create ? free_slot : NULL;
}

long sys_sigaction(regs_t *regs) {
    int signum = (int)(int32_t)syscall_arg(regs, 0);
    uint64_t act_ptr = syscall_arg(regs, 1);
    uint64_t old_ptr = syscall_arg(regs, 2);
    task_t *t = sched_current();

    if (!signum_ok(signum))
        errno_return(EINVAL);

    if (t == NULL)
        errno_return(ESRCH);

    /* The old disposition comes back first, and out of the *task*, so it is right even if
     * the program then fails to install the new one. */
    if (old_ptr != 0) {
        sig_slot_t *slot = slot_find(t, signum, false);
        sigaction_t old;

        old.handler = slot != NULL ? slot->handler : SIG_DFL;
        old.flags  = slot != NULL ? slot->flags : 0;
        old.mask   = slot != NULL ? slot->mask : 0;

        if (!syscall_copy_to_user(old_ptr, &old, sizeof(old)))
            errno_return(EFAULT);
    }

    if (act_ptr == 0)
        return 0;                      /* the query half of the call, which succeeded */

    sigaction_t act;

    if (!syscall_copy_from_user(&act, act_ptr, sizeof(act)))
        errno_return(EFAULT);

    /* SIG_DFL and SIG_IGN are dispositions, not addresses to call.  Everything else has to
     * be an address ring 3 can execute, or a program that installs a bad handler would
     * fault on the *return* into it with nothing in the report pointing here. */
    if (act.handler != SIG_DFL && act.handler != SIG_IGN &&
        !syscall_user_range_ok(act.handler, 1, false))
        errno_return(EFAULT);

    sig_slot_t *slot = slot_find(t, signum, true);

    if (slot == NULL)
        errno_return(EAGAIN);

    slot->used = true;
    slot->signum = (uint32_t)signum;
    slot->handler = act.handler;
    slot->flags = act.flags;
    slot->mask = act.mask;
    return 0;
}

long sys_sigprocmask(regs_t *regs) {
    int how = (int)(int32_t)syscall_arg(regs, 0);
    uint64_t set_ptr = syscall_arg(regs, 1);
    uint64_t old_ptr = syscall_arg(regs, 2);
    task_t *t = sched_current();

    if (t == NULL)
        errno_return(ESRCH);

    if (old_ptr != 0) {
        uint64_t old = t->sig_blocked;

        if (!syscall_copy_to_user(old_ptr, &old, sizeof(old)))
            errno_return(EFAULT);
    }

    if (set_ptr == 0)
        return 0;

    uint64_t set;

    if (!syscall_copy_from_user(&set, set_ptr, sizeof(set)))
        errno_return(EFAULT);

    /* SIGKILL and SIGSTOP cannot be blocked, whatever a program says.  That is not a
     * policy this kernel invented: a process that can block SIGKILL is one that can never
     * be killed, which is the whole reason the two are uncatchable. */
    set &= ~((1ull << (SIGKILL - 1)) | (1ull << (SIGSTOP - 1)));

    switch (how) {
    case 0: t->sig_blocked |= set;  break;      /* SIG_BLOCK   */
    case 1: t->sig_blocked &= ~set; break;      /* SIG_UNBLOCK */
    case 2: t->sig_blocked = set;   break;      /* SIG_SETMASK */
    default: errno_return(EINVAL);
    }

    return 0;
}

long sys_sigpending(regs_t *regs) {
    uint64_t out = syscall_arg(regs, 0);
    task_t *t = sched_current();

    if (t == NULL)
        errno_return(ESRCH);

    /* The whole pending set, *including* the blocked ones.
     *
     * That is what `sigpending` means, and it is the opposite of what delivery looks at:
     * a signal that is blocked is still pending -- it has been sent and has not been
     * delivered -- and masking it here would make "is it coming?" answer "no", which is
     * exactly the question a program asks before it unblocks.  Reporting only the
     * deliverable ones makes a blocked signal invisible, and a program that blocks
     * something, sends it and checks is told it was lost. */
    uint64_t pending = __atomic_load_n(&t->sig_pending, __ATOMIC_ACQUIRE);

    if (out != 0 && !syscall_copy_to_user(out, &pending, sizeof(pending)))
        errno_return(EFAULT);

    return 0;
}

long sys_kill(regs_t *regs) {
    int pid = (int)(int64_t)syscall_arg(regs, 0);
    int signum = (int)(int32_t)syscall_arg(regs, 1);

    if (!signum_ok(signum))
        errno_return(EINVAL);

    if (pid <= 0) {
        /* Only "this process" is answered here.  A broadcast or a process group needs the
         * group and the table to walk, which arrive with job control; saying so beats
         * pretending a signal went somewhere. */
        if (pid == 0 || pid == -1) {
            task_t *self = sched_current();

            if (self == NULL)
                errno_return(ESRCH);

            long r = signal_send(self, signum);

            if (r < 0)
                errno_return(EINVAL);
            return 0;
        }
        errno_return(ESRCH);
    }

    task_t *target = NULL;
    unsigned n = task_count();

    for (unsigned i = 0; i < n; i++) {
        task_t *c = task_by_index(i);

        if (c != NULL && c->id == (uint64_t)pid) {
            target = c;
            break;
        }
    }

    if (target == NULL)
        errno_return(ESRCH);

    long r = signal_send(target, signum);

    if (r < 0)
        errno_return(EINVAL);
    return 0;
}

long sys_pause(regs_t *regs) {
    task_t *t = sched_current();

    if (t == NULL)
        errno_return(ESRCH);

    /* Block until something is delivered.  Anything else -- a wake for another reason,
     * a signal that is blocked -- comes back round the loop, which is what `pause` means:
     * it may return for a reason other than a signal, and the caller re-checks. */
    while (t->sig_pending == 0)
        task_wait();

    return 0;
}

/* --- sigreturn ------------------------------------------------------------ */

/* The five words `iretq` is about to pop, in the order it pops them: RIP, CS, RFLAGS, RSP,
 * SS.  They are the last five quadwords of `regs`, which is exactly what `isr_common()`'s
 * tail reads. */
static uint64_t *iret_words(regs_t *regs) {
    return (uint64_t *)regs + (sizeof(regs_t) / sizeof(uint64_t)) - 5;
}

long sys_sigreturn(regs_t *regs) {
    task_t *t = sched_current();

    if (t == NULL || t->sig_active == 0) {
        /* A `sigreturn` with no handler running is a program's mistake, and returning an
         * error from it is the only safe answer: without a saved context there is nowhere
         * to return to, so anything else would send the machine somewhere arbitrary. */
        errno_return(EINVAL);
    }

    regs_t saved = t->sig_saved;

    /* Everything the handler is allowed to have changed, and nothing else.  RAX is
     * restored rather than left as the handler's return value: on this ABI the handler
     * returns nothing, and letting a handler decide what the interrupted instruction
     * computes with is a way to turn a stray `mov` into an arbitrary kernel decision. */
    regs->r15 = saved.r15;
    regs->r14 = saved.r14;
    regs->r13 = saved.r13;
    regs->r12 = saved.r12;
    regs->r11 = saved.r11;
    regs->r10 = saved.r10;
    regs->r9  = saved.r9;
    regs->r8  = saved.r8;
    regs->rbp = saved.rbp;
    regs->rdi = saved.rdi;
    regs->rsi = saved.rsi;
    regs->rdx = saved.rdx;
    regs->rcx = saved.rcx;
    regs->rbx = saved.rbx;
    regs->rax = saved.rax;

    /* And the interrupted context, into the live frame the `iretq` is going to consume.
     *
     * This is the part that is worth being careful about.  The syscall arrived on a
     * *kernel* stack: the CPU switched to TSS.RSP0 before the door ran, so the frame being
     * restored is not the one the signal interrupted.  Copying the five words across is
     * what makes the `iretq` land the process back at the instruction it was on, with the
     * user stack pointer it had, and in ring 3. */
    uint64_t *out = iret_words(regs);

    out[0] = saved.rip;
    out[1] = saved.cs;
    out[2] = saved.rflags;
    out[3] = saved.rsp;
    out[4] = saved.ss;

    /* Put the blocked set back exactly as it was, and take the signal out of "being
     * handled" before anything can try to deliver another one.  `sig_blocked_saved` rather
     * than a subtraction: delivery blocks *everything*, so nothing of what is blocked now
     * was blocked before, and the only correct restoration is the old value itself. */
    t->sig_active = 0;
    t->sig_blocked = t->sig_blocked_saved;

    return 0;
}

/* --- sending -------------------------------------------------------------- */

long signal_send(struct task *st, int signum) {
    if (st == NULL || !signum_ok(signum))
        return -1;

    /* SIGKILL and SIGSTOP cannot be caught, blocked or ignored, so they are not merely
     * pending: they are acted on here, at once. */
    if (signum == SIGKILL) {
        if (st->id == 1) {
            /* PID 1 is init, and a kernel whose init can be killed by any process is a
             * kernel with nothing left. */
            kprintf("signal: refusing SIGKILL for pid 1\n");
            return -1;
        }
        st->exited = 1;
        st->exit_code = -SIGKILL;
        if (st->parent != NULL)
            task_unblock(st->parent);
        task_exit_with(0);
        return 0;
    }

    __atomic_or_fetch(&st->sig_pending, 1ull << (signum - 1), __ATOMIC_RELEASE);

    if (signum == SIGSTOP || signum == SIGTSTP)
        st->sig_stopped = 1;

    /* A signal to a task that is not running needs a processor to deliver it on, and the
     * task may well be asleep: a SIGCHLD to a parent blocked in `wait` is the case that
     * matters, and the delivery only happens when it is next switched into. */
    if (st->state != TASK_RUNNING && st->state != TASK_RUNNABLE)
        task_unblock(st);

    return 0;
}

/* --- delivery ------------------------------------------------------------- */

static int lowest_pending(uint64_t pending) {
    if (pending == 0)
        return 0;
    return __builtin_ctzll(pending) + 1;
}

void signal_deliver_on_return_to_user(regs_t *regs) {
    task_t *t = sched_current();

    if (t == NULL || !t->is_user || t->sig_active != 0)
        return;

    uint64_t ready = __atomic_load_n(&t->sig_pending, __ATOMIC_ACQUIRE) & ~t->sig_blocked;
    int signum = lowest_pending(ready);

    if (signum == 0)
        return;

    sig_slot_t *slot = slot_find(t, signum, false);
    uint64_t handler = slot != NULL ? slot->handler : SIG_DFL;

    /* Default and ignore are resolved *here*, not by the handler, so that a process with
     * no handler for SIGINT still stops and a process that ignores SIGPIPE still ignores
     * it -- in the one place that runs for both. */
    if (handler == SIG_IGN ||
        (handler == SIG_DFL && signal_default_action(signum) == SIGNAL_DEFAULT_IGNORE)) {
        __atomic_and_fetch(&t->sig_pending, ~(1ull << (signum - 1)), __ATOMIC_RELEASE);
        return;
    }

    if (handler == SIG_DFL) {
        int action = signal_default_action(signum);

        if (action == SIGNAL_DEFAULT_STOP) {
            __atomic_and_fetch(&t->sig_pending, ~(1ull << (signum - 1)),
                               __ATOMIC_RELEASE);
            t->sig_stopped = 1;
            return;
        }

        /* Terminate.  The default action is death, and a process that dies here dies
         * inside the interrupt that told it, which is what `kill -9` is. */
        kprintf("signal: pid %lu killed by %s\n", (unsigned long)t->id,
                signal_name(signum));
        __atomic_and_fetch(&t->sig_pending, ~(1ull << (signum - 1)), __ATOMIC_RELEASE);
        t->exit_code = -signum;
        task_exit_with(0);
        return;
    }

    /* Deliver.
     *
     * The whole interrupted frame is kept in the task before anything is changed, because
     * after the patch below the live frame no longer describes it -- and `sigreturn` puts
     * it back word for word.  `sig_saved` is a copy, not an alias: an alias would point at
     * this frame on *this* processor's kernel stack, which is gone by the time the
     * handler returns and which the next interrupt from ring 3 would overwrite anyway. */
    t->sig_saved = *regs;
    t->sig_active = (uint32_t)signum;
    __atomic_and_fetch(&t->sig_pending, ~(1ull << (signum - 1)), __ATOMIC_RELEASE);

    /* Block everything for the duration.  A nested delivery would need a second saved
     * context and a second frame, and nothing this kernel runs needs one; what it does
     * need is that a ^C inside a ^C handler does not destroy the outer context. */
    t->sig_blocked_saved = t->sig_blocked;
    t->sig_blocked |= ~(1ull << (SIGKILL - 1)) & ~(1ull << (SIGSTOP - 1));
    if (slot->mask != 0)
        t->sig_blocked |= slot->mask;

    /* Where the handler runs.
     *
     * Below the current stack pointer, in the task's own user stack, which is where the
     * program has room and which is guaranteed writable -- it is a page the task mapped.
     * The three quadwords are the whole of the signal frame this kernel uses:
     *
     *     sp + 0   the trampoline      <- what the handler's `ret` returns to
     *     sp + 8   the signal number   <- what the handler reads as its first argument
     *     sp + 16  &sig_saved          <- where the interrupted registers are
     *
     * A full `siginfo_t`/`ucontext_t` pair is what Linux puts here, and a program that
     * wants one can be given one; what is here is what a handler written against this
     * kernel's libc is compiled to expect, and the layout is in docs/SYSCALLS.md. */
    uint64_t sp = regs->rsp - 32;
    sp &= ~15ull;                      /* the ABI wants a 16-byte aligned stack on entry */

    if (!syscall_user_range_ok(sp, 32, true)) {
        /* No room on the stack.  Falling back to the default action is the honest answer:
         * the alternative is an `iretq` into a stack the process does not have. */
        t->sig_active = 0;
        kprintf("signal: no stack to deliver %s on for pid %lu; killing it\n",
                signal_name(signum), (unsigned long)t->id);
        t->exit_code = -signum;
        task_exit_with(0);
        return;
    }

    uint64_t frame[4];

    frame[0] = SIGNAL_TRAMPOLINE_ADDR;
    frame[1] = (uint64_t)signum;
    frame[2] = (uint64_t)(uintptr_t)&t->sig_saved;
    frame[3] = 0;
    syscall_copy_to_user(sp, frame, sizeof(frame));

    regs->rip = handler;
    regs->rsp = sp;
    regs->rdi = (uint64_t)signum;     /* the ABI's first argument register */
}

int signal_init(void) {
    return trampoline_build();
}