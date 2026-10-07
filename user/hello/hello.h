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

/* Errors, as the negative return values the kernel uses. */
#define EBADF   9
#define EFAULT  14
#define ENOSYS  38
#define ENOMEM  12

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
