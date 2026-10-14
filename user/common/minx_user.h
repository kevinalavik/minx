/* minx - the user/kernel boundary for programs with no libc.
 *
 * Everything here is also in <sys/sysdep.h> and <unistd.h>, and this header exists for the
 * programs that link *neither*: `hello`, `forktest`, `sigtest`, `fstest` and the rest of
 * the ring-3 test programs, which are written against the raw door so that a libc bug
 * cannot be what a test is measuring.
 *
 * It is also the structures -- `minx_stat`, `minx_dirent` -- which the kernel fills in, and
 * which a program needs whether or not it has a libc.
 *
 * The call numbers are not here.  They come from <sys/sysdep.h>, which generates them from
 * the kernel's own enum; they used to be a second hand-written copy of that enum and the
 * copy is what went stale, when renumbering left this file and user/common/start.S
 * disagreeing about what `exit` is.
 */
#ifndef MINX_USER_H
#define MINX_USER_H

#include <sys/sysdep.h>

/* --- errors ---------------------------------------------------------------- */

/* Not here any more: <errno.h>, reached through <sys/sysdep.h>.  This list was a subset
 * of that one, written out again, and a subset is the worst kind of copy -- it looks
 * complete to every program that only uses the calls it covers, and silently disagrees
 * with the kernel for a program that uses one it does not.  `ENOTEMPTY` and `ELOOP` are
 * the two this tree actually needed and that a stock list would not have had, which is
 * exactly why the kernel's list is the one that has to be authoritative. */

/* --- open flags ------------------------------------------------------------- */

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x10000

/* --- whence ----------------------------------------------------------------- */

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* --- signals ---------------------------------------------------------------- */

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
#define SIGTTIN  21
#define SIGTTOU  22

#define SIG_DFL ((unsigned long)0)
#define SIG_IGN ((unsigned long)1)
#define SIG_ERR ((unsigned long)-1)

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* sigaction's argument.  Three quadwords, in this order; docs/SYSCALLS.md has the same
 * table and the kernel copies it in and out whole. */
struct sigaction {
    unsigned long handler;
    unsigned long flags;
    unsigned long mask;
};

/* --- memory ------------------------------------------------------------------ */

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* --- types -------------------------------------------------------------------- */

typedef unsigned long size_t_;
typedef long ssize_t_;
typedef long off_t_;

struct iovec {
    unsigned long base;
    unsigned long len;
};

/* `stat`, as the kernel fills it in.  The layout is the kernel's own and is the same in
 * every user program, which is what makes it possible for a program to hand the kernel a
 * buffer and have the fields land where the program's names are.  The kernel copies the
 * whole structure in one go rather than field by field, so the two definitions have to
 * agree exactly -- which they do because they are the same thirteen fields in the same
 * order with the same widths. */
struct minx_stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int  st_mode;
    unsigned int  st_nlink;
    unsigned int  st_uid;
    unsigned int  st_gid;
    unsigned long st_rdev;
    unsigned long st_size;
    unsigned long st_blksize;
    unsigned long st_blocks;
    unsigned long st_atime;
    unsigned long st_mtime;
    unsigned long st_ctime;
};

/* One entry from `getdents`.  The record length is in the structure, so a caller that does
 * not like the name length can skip to the next entry by adding it rather than having to
 * work out how long the name was. */
struct minx_dirent {
    unsigned long d_ino;
    unsigned long d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    unsigned char  d_pad[7];
    char           d_name[256];
};

/* What `d_type` holds: the `DT_*` values, the same ones every other Unix puts there, so a
 * program can write `d_type == DT_DIR` and mean it. */
#define MINX_DT_UNKNOWN 0
#define MINX_DT_FIFO    1
#define MINX_DT_CHR     2
#define MINX_DT_DIR     4
#define MINX_DT_BLK     6
#define MINX_DT_REG     8
#define MINX_DT_LNK     10
#define MINX_DT_SOCK    12

/* Mode bits, the same as in kernel/include/types.h. */
#define S_IFMT   0170000u
#define S_IFDIR  0040000u
#define S_IFREG  0100000u
#define S_IFLNK  0120000u
#define S_IFCHR  0020000u
#define S_IFBLK  0060000u
#define S_IFIFO  0010000u
#define S_IFSOCK 0140000u

/* --- the calls ------------------------------------------------------------------ */

static inline ssize_t_ minx_write(int fd, const void *buf, size_t_ n) {
    return minx_syscall3(SYS_WRITE, fd, (long)buf, (long)n);
}

static inline ssize_t_ minx_read(int fd, void *buf, size_t_ n) {
    return minx_syscall3(SYS_READ, fd, (long)buf, (long)n);
}

static inline long minx_open(const char *path, int flags, int mode) {
    return minx_syscall3(SYS_OPEN, (long)(uintptr_t)path, flags, mode);
}

static inline long minx_close(int fd) {
    return minx_syscall1(SYS_CLOSE, fd);
}

static inline off_t_ minx_lseek(int fd, off_t_ off, int whence) {
    return minx_syscall3(SYS_LSEEK, fd, (long)off, whence);
}

static inline long minx_stat(const char *path, struct minx_stat *st) {
    return minx_syscall2(SYS_STAT, (long)(uintptr_t)path, (long)(uintptr_t)st);
}

static inline long minx_lstat(const char *path, struct minx_stat *st) {
    return minx_syscall2(SYS_LSTAT, (long)(uintptr_t)path, (long)(uintptr_t)st);
}

static inline long minx_fstat(int fd, struct minx_stat *st) {
    return minx_syscall2(SYS_FSTAT, fd, (long)(uintptr_t)st);
}

static inline long minx_getdents(int fd, void *buf, size_t_ n) {
    return minx_syscall3(SYS_GETDENTS, fd, (long)(uintptr_t)buf, (long)n);
}

static inline long minx_dup(int fd) { return minx_syscall1(SYS_DUP, fd); }

static inline long minx_dup2(int a, int b) { return minx_syscall2(SYS_DUP2, a, b); }

static inline long minx_mkdir(const char *path, int mode) {
    return minx_syscall2(SYS_MKDIR, (long)(uintptr_t)path, mode);
}

static inline long minx_rmdir(const char *path) {
    return minx_syscall1(SYS_RMDIR, (long)(uintptr_t)path);
}

static inline long minx_unlink(const char *path) {
    return minx_syscall1(SYS_UNLINK, (long)(uintptr_t)path);
}

static inline long minx_rename(const char *from, const char *to) {
    return minx_syscall2(SYS_RENAME, (long)(uintptr_t)from, (long)(uintptr_t)to);
}

static inline long minx_link(const char *from, const char *to) {
    return minx_syscall2(SYS_LINK, (long)(uintptr_t)from, (long)(uintptr_t)to);
}

static inline long minx_symlink(const char *target, const char *path) {
    return minx_syscall2(SYS_SYMLINK, (long)(uintptr_t)target, (long)(uintptr_t)path);
}

static inline long minx_readlink(const char *path, char *buf, size_t_ n) {
    return minx_syscall3(SYS_READLINK, (long)(uintptr_t)path, (long)(uintptr_t)buf,
                         (long)n);
}

static inline long minx_chmod(const char *path, int mode) {
    return minx_syscall2(SYS_CHMOD, (long)(uintptr_t)path, mode);
}

static inline long minx_chown(const char *path, int uid, int gid) {
    return minx_syscall3(SYS_CHOWN, (long)(uintptr_t)path, uid, gid);
}

static inline long minx_truncate(const char *path, off_t_ len) {
    return minx_syscall2(SYS_TRUNCATE, (long)(uintptr_t)path, (long)len);
}

static inline long minx_ftruncate(int fd, off_t_ len) {
    return minx_syscall2(SYS_FTRUNCATE, fd, (long)len);
}

static inline long minx_utimes(const char *path, const unsigned long *times) {
    return minx_syscall2(SYS_UTIMES, (long)(uintptr_t)path, (long)(uintptr_t)times);
}

static inline long minx_chdir(const char *path) {
    return minx_syscall1(SYS_CHDIR, (long)(uintptr_t)path);
}

static inline long minx_getcwd(char *buf, size_t_ n) {
    return minx_syscall2(SYS_GETCWD, (long)(uintptr_t)buf, (long)n);
}

static inline long minx_umask(int mask) { return minx_syscall1(SYS_UMASK, mask); }

static inline long minx_access(const char *path, int mode) {
    return minx_syscall2(SYS_ACCESS, (long)(uintptr_t)path, mode);
}

static inline long minx_mount(const char *src, const char *target, const char *type) {
    return minx_syscall3(SYS_MOUNT, (long)(uintptr_t)src, (long)(uintptr_t)target,
                         (long)(uintptr_t)type);
}

static inline long minx_umount(const char *target) {
    return minx_syscall1(SYS_UMOUNT, (long)(uintptr_t)target);
}

static inline long minx_fcntl(int fd, int cmd, long arg) {
    return minx_syscall3(SYS_FCNTL, fd, cmd, arg);
}

static inline long minx_ioctl(int fd, unsigned long request, unsigned long arg) {
    return minx_syscall3(SYS_IOCTL, fd, (long)request, (long)arg);
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

/* The console's descriptor is not the program's to close: it belongs to the process table,
 * so a program that closes its own stdout loses its output rather than taking the console
 * away from every other process on the machine. */
static inline long minx_exit(int code) {
    return minx_syscall1(SYS_EXIT, code);
}

static inline long minx_getpid(void) {
    return minx_syscall1(SYS_GETPID, 0);
}

static inline long minx_getppid(void) {
    return minx_syscall1(SYS_GETPPID, 0);
}

static inline long minx_getuid(void) {
    return minx_syscall1(SYS_GETUID, 0);
}

static inline long minx_getgid(void) {
    return minx_syscall1(SYS_GETGID, 0);
}

static inline long minx_getpgid(long pid) {
    return minx_syscall1(SYS_GETPGID, pid);
}

static inline long minx_setpgid(long pid, long pgid) {
    return minx_syscall2(SYS_SETPGID, pid, pgid);
}

static inline long minx_setsid(void) {
    return minx_syscall1(SYS_SETSID, 0);
}

static inline void *minx_brk(void *addr) {
    return (void *)(uintptr_t)minx_syscall1(SYS_BRK, (long)(uintptr_t)addr);
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
    /* Three arguments, all of them, because the kernel reads three.
     *
     * `sys_wait()` takes the status pointer, a pid selector and an options word, and a
     * caller that supplies only the first leaves the other two in whatever registers it
     * happened to have.  Those are *scratch* registers in the SysV ABI: nothing promises
     * them to be zero, so the options word arrived with a bit set and `WNOHANG` came back
     * on, and `wait()` returned 0 for a child that had already exited.  The rule this
     * follows is the rule every syscall ABI has -- pass what the call reads, or it reads
     * whatever was there -- and it is spelled out at the call because that is where it is
     * easy to get wrong when a call grows an argument. */
    return minx_syscall3(SYS_WAIT, (long)(uintptr_t)status, -1, 0);
}

/* Create a pipe and write its two ends into `fds`, which must be a pointer to two ints.
 * Returns 0, or -errno. */
static inline long minx_pipe(int *fds) {
    return minx_syscall1(SYS_PIPE, (long)(uintptr_t)fds);
}

/* Sleep for `sec.nsec`.  Returns 0, or -errno. */
static inline long minx_nanosleep(unsigned long sec, unsigned long nsec) {
    unsigned long ts[2];

    ts[0] = sec;
    ts[1] = nsec;
    return minx_syscall2(SYS_NANOSLEEP, (long)(uintptr_t)ts, 0);
}

/* Which processor this task is running on.
 *
 * Read from the APIC's own identification register by the kernel, so it is the processor
 * that executed the `int $0x80` and not the one the scheduler last recorded.  Those two are
 * not supposed to differ, and `smptest` is the check that they do not.
 *
 * Returns a processor id, not an index: on this machine they are the same number, and a
 * program that treated them as interchangeable would be the first thing to break if they
 * ever were not. */
static inline long minx_getcpu(void) {
    return minx_syscall1(SYS_GETCPU, 0);
}

/* Milliseconds since boot.
 *
 * `times()`' fourth clock, which is `timer_uptime_ms()`: monotonic, in milliseconds, and the
 * only clock on this system that does not go backwards.  `times()`' own first field is CPU
 * time, which is zero for a task that has not been scheduled, and a program measuring how
 * long its children took with that is measuring nothing. */
static inline long minx_uptime_ms(void) {
    unsigned long ticks[4] = { 0, 0, 0, 0 };

    if (minx_syscall1(SYS_TIMES, (long)(uintptr_t)ticks) < 0)
        return -1;

    return (long)ticks[3];
}

static inline long minx_sched_yield(void) {
    return minx_syscall1(SYS_SCHED_YIELD, 0);
}

/* Map `length` bytes of anonymous memory.  Returns the address, or a negative errno. */
static inline void *minx_mmap(unsigned long addr, unsigned long length, int prot,
                              int flags) {
    return (void *)(uintptr_t)minx_syscall6(SYS_MMAP, (long)addr, (long)length, prot,
                                            flags, -1, 0);
}

static inline long minx_munmap(void *addr, unsigned long length) {
    return minx_syscall2(SYS_MUNMAP, (long)(uintptr_t)addr, (long)length);
}

static inline long minx_mprotect(void *addr, unsigned long length, int prot) {
    return minx_syscall3(SYS_MPROTECT, (long)(uintptr_t)addr, (long)length, prot);
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

/* Change what is blocked: `how` is SIG_BLOCK, SIG_UNBLOCK or SIG_SETMASK.  `old_set` may be
 * NULL.  Returns 0, or -errno. */
static inline long minx_sigprocmask(int how, const unsigned long *set,
                                    unsigned long *old_set) {
    return minx_syscall3(SYS_SIGPROCMASK, how, (long)(uintptr_t)set,
                         (long)(uintptr_t)old_set);
}

/* The signals that are pending, written into `set`.  Returns 0, or -errno. */
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

/* The other two string routines a program needs before it has a libc, for the same reason
 * and with the same trade: written out, so that the program's own code still has no
 * unresolved symbols.  A test program that walks a directory listing compares names, and a
 * program that writes a buffer wants it zeroed rather than left to whatever the loader
 * left there -- both of which are one call here and a link error without it. */
static inline int minx_strcmp(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }

    /* The difference of the unsigned bytes, not of the chars: a program that gets a
     * negative answer for "Z" against "a" has been handed a sign that depends on
     * whether the compiler decided `char` is signed on this machine. */
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline int minx_strncmp(const char *a, const char *b, size_t_ n) {
    for (size_t_ i = 0; i < n; i++) {
        int d = (int)(unsigned char)a[i] - (int)(unsigned char)b[i];

        if (d != 0)
            return d;
        if (a[i] == 0)
            return 0;
    }
    return 0;
}

/* strstr, for the same reason: a program reading a file another program wrote cannot
 * assume the answer is at offset zero, and searching for a field is what keeps it working
 * when the format grows a field. */
static inline const char *minx_strstr(const char *hay, const char *needle) {
    if (*needle == 0)
        return hay;

    for (; *hay != 0; hay++) {
        const char *h = hay;
        const char *n = needle;

        while (*n != 0 && *h == *n) {
            h++;
            n++;
        }
        if (*n == 0)
            return hay;
    }
    return NULL;
}

static inline void *minx_memset(void *p, int c, size_t_ n) {
    unsigned char *b = p;

    for (size_t_ i = 0; i < n; i++)
        b[i] = (unsigned char)c;
    return p;
}

#define memset minx_memset

#endif /* MINX_USER_H */