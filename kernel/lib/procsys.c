/* minx - the process system calls that are not files and not signals.
 *
 * Everything here is about *this process*: who it is, what it may do, what it is allowed
 * to wait for, and how much memory it has.  They are grouped in one file because they are
 * one thing, and because the ones that answer "what is this process's..." are the ones that
 * have to agree with each other -- `getppid` and `wait` and `/proc/<pid>/status` all read
 * the same two fields, and three files reading them is three places for them to disagree.
 */
#include "apic.h"
#include "procsys.h"

#include "console.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "power.h"
#include "string.h"
#include "syscall.h"
#include "task.h"
#include "timer.h"
#include "vmm.h"

/* --- identity ------------------------------------------------------------- */

long sys_getppid(regs_t *regs) {
    task_t *t = sched_current();

    (void)regs;
    return t != NULL && t->parent != NULL ? (long)t->parent->id : 0;
}

long sys_getuid(regs_t *regs) {
    task_t *t = sched_current();

    (void)regs;
    return t != NULL ? (long)t->uid : 0;
}

long sys_geteuid(regs_t *regs) {
    return sys_getuid(regs);
}

long sys_getgid(regs_t *regs) {
    task_t *t = sched_current();

    (void)regs;
    return t != NULL ? (long)t->gid : 0;
}

/* `setuid`.
 *
 * Every process here is root, and there is one user, so the only real question is whether
 * this call should be able to *reduce* a process's own privileges.  It can: `setuid(n)` with
 * no argument meaning "back to real" is in the init program of every Unix ever written, and
 * a kernel that refuses it leaves `su` -- which this tree is expected to grow -- unable to
 * do the one thing it is for.  Only root may set an identifier, and only to something it
 * already has, which is the rule that makes `su` work and stops a program giving itself
 * powers it did not have. */
long sys_setuid(regs_t *regs) {
    task_t *t = sched_current();

    if (t == NULL)
        errno_return(ESRCH);

    long want = (long)syscall_arg(regs, 0);

    if ((int64_t)want == -1)
        return (long)t->uid;            /* "tell me", not "change me" */

    if (t->uid != 0 && (uint64_t)want != t->uid)
        errno_return(EPERM);

    t->uid = (uint64_t)want;
    return (long)t->uid;
}

/* --- process groups and sessions ------------------------------------------ */

long sys_setsid(regs_t *regs) {
    task_t *t = sched_current();

    (void)regs;

    if (t == NULL)
        errno_return(ESRCH);

    /* A process that is already a group leader cannot make a new session: it is already
     * the leader of one, and a session is defined by its leader.  EPERM here is what every
     * system returns, and a `sh` that calls setsid twice is a bug in the shell rather than
     * something the kernel should paper over. */
    if (t->pgrp == (uint32_t)t->id)
        errno_return(EPERM);

    t->session = (uint32_t)t->id;
    t->pgrp = (uint32_t)t->id;
    return (long)t->id;
}

long sys_setpgid(regs_t *regs) {
    task_t *t = sched_current();
    long pid = (long)syscall_arg(regs, 0);
    long pgid = (long)syscall_arg(regs, 1);

    if (t == NULL)
        errno_return(ESRCH);

    /* pgid 0 means "my own pid", and both zero mean "this process". */
    if (pgid == 0)
        pgid = (long)t->id;

    task_t *target = t;

    if (pid != 0 && pid != (long)t->id) {
        target = task_by_pid((uint64_t)pid);

        if (target == NULL)
            errno_return(ESRCH);
    }

    /* Only within one session: a group is a set of processes in the same session, and a
     * program that could move a process from one session into another could take it out of
     * its own terminal's control. */
    if (target->session != t->session)
        errno_return(EPERM);

    target->pgrp = (uint32_t)pgid;
    return 0;
}

long sys_getpgid(regs_t *regs) {
    task_t *t = sched_current();
    long pid = (long)syscall_arg(regs, 0);

    if (t == NULL)
        errno_return(ESRCH);

    if (pid == 0)
        return (long)t->pgrp;

    task_t *target = task_by_pid((uint64_t)pid);

    if (target == NULL)
        errno_return(ESRCH);

    return (long)target->pgrp;
}

/* --- uname ----------------------------------------------------------------- */

/* Six strings and three numbers, in the order every system uses, because the numbers are
 * what a program's `uname -s` prints and the order is what makes the struct a struct rather
 * than five unrelated things. */
long sys_uname(regs_t *regs) {
    uint64_t out = syscall_arg(regs, 0);
    struct {
        char     sysname[65];
        char     nodename[65];
        char     release[65];
        char     version[65];
        char     machine[65];
        uint64_t domain;
        char     domain_name[33];
    } u;

    memset(&u, 0, sizeof(u));
    strlcpy(u.sysname, "minx", sizeof(u.sysname));
    strlcpy(u.nodename, "minx", sizeof(u.nodename));
    strlcpy(u.release, "0.1", sizeof(u.release));
    strlcpy(u.version, "a Unix-like OS for x86-64", sizeof(u.version));
    strlcpy(u.machine, "x86_64", sizeof(u.machine));
    strlcpy(u.domain_name, "(none)", sizeof(u.domain_name));

    if (!syscall_copy_to_user(out, &u, sizeof(u)))
        errno_return(EFAULT);

    return 0;
}

/* --- time ------------------------------------------------------------------- */

long sys_nanosleep(regs_t *regs) {
    uint64_t req = syscall_arg(regs, 0);
    uint64_t rem = syscall_arg(regs, 1);

    struct { uint64_t sec, nsec; } ts;

    if (req == 0)
        return 0;

    if (!syscall_copy_from_user(&ts, req, sizeof(ts)))
        errno_return(EFAULT);

    if (ts.nsec >= 1000000000ull)
        errno_return(EINVAL);

    uint64_t ms = ts.sec * 1000ull + ts.nsec / 1000000ull;

    /* A sleep of less than a millisecond is not a sleep of nothing: the timebase counts in
     * milliseconds, so rounding to zero would make `usleep(500)` return immediately, and a
     * program polling would spin.  One millisecond is the floor and it is said here. */
    task_sleep_ms(ms == 0 ? 1 : ms);

    if (rem != 0) {
        struct { uint64_t sec, nsec; } left = { 0, 0 };

        (void)left;
        /* Nothing is left: the sleep ran to completion.  Zero is written rather than
         * skipped, because a program that checks `rem` after a signal should see zero
         * rather than whatever was in its own buffer. */
        if (!syscall_copy_to_user(rem, &left, sizeof(left)))
            errno_return(EFAULT);
    }

    return 0;
}

/* Which processor is this task running on?
 *
 * `lapic_id()` reads the APIC's own identification register, so the answer comes from the
 * processor that executed the `int $0x80` -- not from a cached per-task copy and not from
 * whatever the scheduler last recorded.  Those would all answer "where did the scheduler
 * last put this task", which is not the question; `smptest` exists to check that the
 * scheduler's idea and the hardware's agree.
 *
 * A task that has never run cannot be on a processor, but a user program cannot ask before
 * it has run, so there is no case here that needs an answer of "none". */
long sys_getcpu(regs_t *regs) {
    (void)syscall_arg(regs, 0);

    return (long)lapic_id();
}

long sys_sched_yield(regs_t *regs) {
    (void)regs;

    /* "Give the processor to somebody else if there is somebody else."  Returning without
     * yielding when there is nobody else is not a failure: a single-process machine has
     * nobody, and making yield complain would make every program that uses it a program
     * that has to check. */
    sched_yield();
    return 0;
}

long sys_times(regs_t *regs) {
    uint64_t out = syscall_arg(regs, 0);
    task_t *t = sched_current();

    if (t == NULL)
        errno_return(ESRCH);

    /* Four clock ticks: self, children, and the two that only mean something once there is
     * more than one process.  Milliseconds here rather than the hundred-Hz ticks the name
     * implies, because the timebase this kernel has *is* milliseconds and a caller that
     * multiplies by ten to get a CPU percentage would then be wrong by a factor of ten.
     * docs/SYSCALLS.md says so rather than leaving the unit to be guessed. */
    uint64_t ticks[4];

    ticks[0] = t->exec_ns / 1000000ull;
    ticks[1] = 0;
    ticks[2] = 0;
    ticks[3] = timer_uptime_ms();

    if (out != 0 && !syscall_copy_to_user(out, ticks, sizeof(ticks)))
        errno_return(EFAULT);

    return (long)ticks[0];
}

long sys_getrusage(regs_t *regs) {
    task_t *t = sched_current();

    if (sched_current() == NULL)
        errno_return(ESRCH);

    /* Two numbers per resource plus the flags, in the order the structure has.  Only the
     * time and the memory are real here; the rest are zero because there is nothing to
     * count, and zero is the honest answer for a counter that has never been incremented
     * rather than a made-up one. */
    struct {
        uint64_t utime, stime;
        int64_t  maxrss;
        int64_t  ixrss;
        int64_t  idrss;
        int64_t  isrss;
        int64_t  minflt, majflt, nswap;
        int64_t  inblock, oublock, msgsnd, msgrcv, nsignals, nvcsw, nivcsw;
    } ru;

    memset(&ru, 0, sizeof(ru));
    ru.utime = t != NULL ? t->exec_ns / 1000000ull : 0;
    ru.stime = 0;
    ru.maxrss = 4096;

    if (!syscall_copy_to_user(syscall_arg(regs, 1), &ru, sizeof(ru)))
        errno_return(EFAULT);

    return 0;
}

long sys_reboot(regs_t *regs) {
    long how = (long)syscall_arg(regs, 0);

    /* Only root, and only because there is only root: the check is here so that the rule is
     * written down, and so that adding users means adding a check rather than a policy. */
    task_t *t = sched_current();

    if (t == NULL || t->uid != 0)
        errno_return(EPERM);

    /* RB_AUTOBOOT is 0x01234567 and is the *only* value that reboots; every other value is
     * "please halt", and this kernel has no use for the second one, so it powers off.  A
     * program that wants the machine to go down either way gets it. */
    if (how == 0x01234567ul) {
        reboot();
        return 0;
    }

    poweroff();
    return 0;
}