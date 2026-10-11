/* minx user programs - the kernel ABI, and nothing else.
 *
 * This is the whole of the user-visible interface: four call numbers, one structure,
 * and the two instructions that reach the kernel.  There is no libc here and no
 * intention of one yet -- phase 9 brings mlibc, which will have its own idea of all of
 * this and will need to agree with it.
 */
#include <stdint.h>

#ifndef MINX_USER_H
#define MINX_USER_H

/* Call numbers.  These must match kernel/include/syscall.h. */
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_EXIT    4
#define SYS_BRK     5
#define SYS_WRITEV  6
#define SYS_GETPID  7
#define SYS_FORK    8
#define SYS_WAIT    9
#define SYS_PIPE    10
#define SYS_EXEC    11
#define SYS_KILL         12
#define SYS_SIGACTION    13
#define SYS_SIGRETURN    14
#define SYS_SIGPROCMASK  15
#define SYS_SIGPENDING   16
#define SYS_PAUSE        17

/* Signals.  The same numbers as kernel/include/signal.h, which is the list the names mean
 * everywhere: a program that says SIGINT means ^C. */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20

#define SIG_DFL ((unsigned long)0)
#define SIG_IGN ((unsigned long)1)

/* sigaction's argument.  Three quadwords, in this order; docs/SYSCALLS.md has the same
 * table and the kernel copies it in and out whole. */
struct sigaction {
    unsigned long handler;
    unsigned long flags;
    unsigned long mask;
};

/* Errors, as the negative return values the kernel uses. */
#define EBADF   9
#define EFAULT  14
#define ENOSYS  38
#define ENOMEM  12
#define EPIPE   32

typedef unsigned long size_t_;
typedef long ssize_t_;

struct iovec {
    unsigned long base;
    unsigned long len;
};

/* The door.  `int $0x80` rather than `syscall`, for the reason recorded in
 * docs/DECISIONS.md: it reuses the interrupt path the kernel already has, including the
 * TSS stack switch, so a syscall costs nothing in new machinery and cannot get the
 * privilege transition subtly wrong. */
#define MINX_SYSCALL "int $0x80"

static inline long minx_syscall1(long nr, long a0) {
    long ret;

    __asm__ volatile(MINX_SYSCALL
                     : "=a"(ret)
                     : "a"(nr), "D"(a0)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline long minx_syscall2(long nr, long a0, long a1) {
    long ret;

    __asm__ volatile(MINX_SYSCALL
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline long minx_syscall3(long nr, long a0, long a1, long a2) {
    long ret;

    __asm__ volatile(MINX_SYSCALL
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

/* The sixth argument lives in r10 and the seventh in r8, and the compiler cannot know
 * that without being told, so they are bound by name and initialised.  `initialise` is
 * only here to satisfy the "uninitialised" analysis; the register constraint means the
 * value the compiler puts there is the fifth argument's, not zero. */
static inline long minx_syscall6(long nr, long a0, long a1, long a2,
                                 long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = 0;

    __asm__ volatile(MINX_SYSCALL
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2),
                       "r"(r10), "r"(a3), "r"(a4), "r"(a5)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline ssize_t_ minx_write(int fd, const void *buf, size_t_ n) {
    return minx_syscall3(SYS_WRITE, fd, (long)buf, (long)n);
}

static inline ssize_t_ minx_read(int fd, void *buf, size_t_ n) {
    return minx_syscall3(SYS_READ, fd, (long)buf, (long)n);
}

/* Replace this process with the program called `path`.
 *
 * `argv` is an array of pointers into *this* process's memory, which the kernel reads before
 * unmapping it.  It may be NULL.
 *
 * On success this does not return: the old program's text and stack are gone, so there is no
 * code left here to come back to.  On failure it returns -errno and the process carries on,
 * so a program can report what went wrong. */
static inline long minx_exec(const char *path, char *const *argv, char *const *envp) {
    return minx_syscall3(SYS_EXEC, (long)(uintptr_t)path, (long)(uintptr_t)argv,
                         (long)(uintptr_t)envp);
}

/* Close a descriptor.  The kernel returns EBADF for a number that is not open, and for
 * 0, 1 and 2 as well: the console belongs to the process table, not to the program, so a
 * program that closes its own stdout loses its output rather than taking the console away
 * from every other process on the machine. */
static inline long minx_close(int fd) {
    return minx_syscall1(SYS_CLOSE, fd);
}

/* Create a pipe and write its two ends into `fds`, which must be a pointer to two ints.
 * Returns 0, or -errno. */
static inline long minx_pipe(int *fds) {
    return minx_syscall1(SYS_PIPE, (long)(uintptr_t)fds);
}

static inline long minx_exit(int code) {
    return minx_syscall1(SYS_EXIT, code);
}

static inline long minx_getpid(void) {
    return minx_syscall1(SYS_GETPID, 0);
}

/* Returns the child's identifier in the parent and zero in the child, which is the whole
 * convention: the child continues at the instruction after this one with every register
 * the same except the return value, so there is nothing to set up on either side. */
static inline long minx_fork(void) {
    return minx_syscall1(SYS_FORK, 0);
}

/* Blocks until one of this process's children has exited.  Returns its identifier, or
 * -1 with nothing left to wait for.  `status` may be NULL; it is a pointer like any other,
 * so the kernel checks it before writing to it. */
static inline long minx_wait(int *status) {
    return minx_syscall2(SYS_WAIT, (long)(uintptr_t)status, 0);
}

/* Send `signum` to a process.  `pid` is a process identifier; 0 means this process and
 * nothing else.  Returns 0, or -errno. */
static inline long minx_kill(long pid, int signum) {
    return minx_syscall2(SYS_KILL, pid, signum);
}

/* Install `act` for `signum`, and put whatever was there into `old` when `old` is not
 * NULL.  Returns 0, or -errno. */
static inline long minx_sigaction(int signum, const struct sigaction *act,
                                 struct sigaction *old) {
    return minx_syscall3(SYS_SIGACTION, signum, (long)(uintptr_t)act,
                         (long)(uintptr_t)old);
}

/* Change what is blocked: `how` is 0 to add, 1 to remove, 2 to replace.  `old_set` may be
 * NULL.  Returns 0, or -errno. */
static inline long minx_sigprocmask(int how, const unsigned long *set,
                                    unsigned long *old_set) {
    return minx_syscall3(SYS_SIGPROCMASK, how, (long)(uintptr_t)set,
                         (long)(uintptr_t)old_set);
}

/* The signals that are pending and not blocked, written into `set`.  Returns 0, or
 * -errno. */
static inline long minx_sigpending(unsigned long *set) {
    return minx_syscall1(SYS_SIGPENDING, (long)(uintptr_t)set);
}

/* Block until something is delivered.  Returns 0.  There is no error return and there is
 * no way to tell which signal woke it, which is exactly why `pause` exists: the caller
 * looks at its own state afterwards. */
static inline long minx_pause(void) {
    return minx_syscall1(SYS_PAUSE, 0);
}

/* Return from a signal handler.
 *
 * A handler's `return` goes to a trampoline the kernel put on its stack, which calls this.
 * Nothing in a program ever calls it directly, and the kernel refuses it when no handler is
 * running -- there would be nothing to return to. */
static inline long minx_sigreturn(void) {
    return minx_syscall1(SYS_SIGRETURN, 0);
}

/* strlen, because a freestanding program has no libc and the loader's job is not to
 * provide one.  Written out rather than declared so that the program's own code has no
 * unresolved symbols at all -- a loader that cannot resolve anything is a loader that
 * cannot go wrong resolving the wrong thing. */
static inline size_t_ minx_strlen(const char *s) {
    size_t_ n = 0;

    while (s[n] != 0)
        n++;
    return n;
}

#endif /* MINX_USER_H */
