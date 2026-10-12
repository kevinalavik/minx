/* minx - the user/kernel boundary, and nothing else.
 *
 * This is the whole of the raw user-visible interface: the call numbers, the instructions
 * that reach the kernel, and the handful of inline wrappers over them.  It is here, in
 * libc, rather than in a program, because both sides need it: libc is built on top of it,
 * and a ring-3 test program with no libc still has to reach the kernel somehow.
 *
 * The call numbers are *generated* from kernel/include/syscall.h by
 * tools/gen-syscall-table.sh and included below.  They used to be a second hand-written
 * copy of that enum, and the copy is what broke: renumbering the enum updated one copy and
 * not the other, so every program's `exit` became `lseek` and every program that returned
 * from `main` spun in the entry stub for ever.
 *
 * Nothing above this header should name a call number.  `read` and `open` and `fork` are
 * in <unistd.h> and <fcntl.h>, they read well, and they are the only thing that should have
 * to change if the numbering does.
 *
 * The conventions are the SysV ones, because the compiler generates them:
 *
 *   number        rax
 *   1st argument  rdi
 *   2nd           rsi
 *   3rd           rdx
 *   4th           rcx
 *   5th           r8
 *   6th           r9
 *   return        rax, negative for -errno
 *
 * The sixth argument is passed in r10 rather than in the stack, which the compiler has to
 * be told about -- hence the named register in `minx_syscall6()`.
 */
#ifndef MINX_SYSDEP_H
#define MINX_SYSDEP_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* The numbers, from the kernel's enum.  See the header comment for why they are generated
 * rather than written here. */
#include "minx_syscall_table.h"

/* `int $0x80` rather than `syscall`, for the reason recorded in docs/DECISIONS.md: it
 * reuses the interrupt path the kernel already has, including the TSS stack switch, so a
 * syscall costs nothing in new machinery and cannot get the privilege transition subtly
 * wrong. */
#define MINX_SYSCALL "int $0x80"

/* --- sizes and types the door needs ---------------------------------------- */

/* The kernel's own types, named the way a C library names them, so that <unistd.h> and
 * friends can use `ssize_t` without every program defining it first.  They are the widths
 * the compiler uses on this target; nothing here is a reinterpretation.
 *
 * `size_t` comes from <stddef.h> and is deliberately not redefined -- two typedefs of the
 * same name are legal in C11 only when they name the same type, and relying on the
 * compiler's to agree with this file is a way of finding out at link time. */
typedef long          ssize_t;
typedef long          off_t;
typedef long          pid_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef unsigned int  mode_t;
typedef long          time_t;
typedef long          clock_t;
typedef long          suseconds_t;
typedef long          useconds_t;      /* what POSIX calls it in <unistd.h> */

/* The trailing-underscore spellings, which is what this tree's ring-3 test programs were
 * written against before there was a libc: `minx_write()` and friends return `ssize_t_`
 * and take `size_t_`.  Renaming them all is churn for no gain, and a name that exists so
 * that old code keeps compiling is not a name that has to be defended for ever. */
typedef ssize_t ssize_t_;
typedef size_t  size_t_;

/* `__ssize_t` and friends exist so that a header written for glibc keeps compiling: a
 * program that includes <sys/types.h> and says `__pid_t` means the same thing as the glibc
 * one.  Defining them here rather than in <sys/types.h> keeps every header that needs a
 * type from having to include <sys/types.h> first, which is a circular dependency every
 * libc hits and solves by the same trick. */
typedef long          __ssize_t;
typedef long          __off_t;
typedef long          __pid_t;
typedef unsigned long __size_t;
typedef unsigned int  __mode_t;
typedef unsigned int  __uid_t;
typedef unsigned int  __gid_t;
typedef long          __time_t;
typedef long          __clock_t;
typedef long          __syscall_slong_t;

/* --- the door ---------------------------------------------------------------- */

static inline long minx_syscall0(long nr) {
    long ret;

    __asm__ volatile(MINX_SYSCALL
                     : "=a"(ret)
                     : "a"(nr)
                     : "rcx", "r11", "memory");
    return ret;
}

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

static inline long minx_syscall4(long nr, long a0, long a1, long a2, long a3) {
    long ret;
    register long r10 __asm__("r10") = a3;

    __asm__ volatile(MINX_SYSCALL
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

/* The seventh and later arguments go on the stack, which the compiler has to be told
 * about; `r10` is bound by name so that the fourth argument's register is known. */
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

/* --- what the kernel does to errno ------------------------------------------ */

/* A call returns -errno rather than setting errno and returning -1, because that is what
 * this kernel's door does.  `__syscall_ret()` turns that into the C convention -- and does
 * it in exactly one place, which is what stops the two conventions drifting into each
 * other one wrapper at a time.
 *
 * The range check matters: a call that *succeeds* and returns -1 must not become an error,
 * and the kernel's errno values are all small positives, so anything at or below
 * -4095 is an error and anything above it is a result that happens to be negative.  That
 * is the same boundary Linux uses, and the reason is the same: a program is allowed to
 * return -4095 from a function whose error range is 1..4095. */
#define __MINX_ERRNO_MAX 4095

static inline long __syscall_ret(long r) {
    if (r < 0 && r > -__MINX_ERRNO_MAX) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

#define __syscall0(nr)              __syscall_ret(minx_syscall0(nr))
#define __syscall1(nr, a)           __syscall_ret(minx_syscall1(nr, a))
#define __syscall2(nr, a, b)        __syscall_ret(minx_syscall2(nr, a, b))
#define __syscall3(nr, a, b, c)     __syscall_ret(minx_syscall3(nr, a, b, c))
#define __syscall4(nr, a, b, c, d)  __syscall_ret(minx_syscall4(nr, a, b, c, d))
#define __syscall6(nr, a, b, c, d, e, f) \
    __syscall_ret(minx_syscall6(nr, a, b, c, d, e, f))

#endif /* MINX_SYSDEP_H */