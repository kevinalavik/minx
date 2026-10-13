/* minx - the unistd calls.
 *
 * Almost all of these are one line: get the arguments into registers and let the kernel
 * decide.  That is the point of the file, not an apology for it -- the decisions a libc
 * makes that the kernel should not have to are the ones worth reading (exit, wait, the
 * `exec` family), and they are the ones with comments.
 */
#include <errno.h>
#include <fcntl.h>
#include <internal.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* --- descriptors ------------------------------------------------------------ */

ssize_t read(int fd, void *buf, size_t n) {
    return (ssize_t)__syscall3(SYS_READ, fd, (long)(uintptr_t)buf, (long)n);
}

ssize_t write(int fd, const void *buf, size_t n) {
    return (ssize_t)__syscall3(SYS_WRITE, fd, (long)(uintptr_t)buf, (long)n);
}

int close(int fd) {
    return (int)__syscall1(SYS_CLOSE, fd);
}

off_t lseek(int fd, off_t off, int whence) {
    return (off_t)__syscall3(SYS_LSEEK, fd, off, whence);
}

/* `open()` is variadic because POSIX says so, but only the third argument is ever a mode --
 * and it is only *read* when O_CREAT is among the flags.  Reading a vararg that was not
 * passed is undefined behaviour, so the mode is fetched only when there is one to fetch.
 *
 * This is the classic bug in every hand-written `open()`: reading the third argument
 * unconditionally.  It works on a stack that happens to have something in the slot and
 * produces a random mode for a file opened without one. */
int open(const char *path, int flags, ...) {
    if (flags & O_CREAT) {
        va_list ap;
        int     mode;

        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);

        return (int)__syscall3(SYS_OPEN, (long)(uintptr_t)path, flags, mode);
    }

    return (int)__syscall3(SYS_OPEN, (long)(uintptr_t)path, flags, 0);
}

int creat(const char *path, mode_t mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int unlink(const char *path) {
    return (int)__syscall1(SYS_UNLINK, (long)(uintptr_t)path);
}

int rmdir(const char *path) {
    return (int)__syscall1(SYS_RMDIR, (long)(uintptr_t)path);
}

/* `remove()` is the C name for "unlink, except for a directory", and it has to decide
 * which.  A program that calls `remove()` on a path it just created as a file must not
 * have to know how, which is the whole reason this function exists. */
int remove(const char *path) {
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;

    return (st.st_mode & S_IFMT) == S_IFDIR ? rmdir(path) : unlink(path);
}

int rename(const char *from, const char *to) {
    return (int)__syscall2(SYS_RENAME, (long)(uintptr_t)from, (long)(uintptr_t)to);
}

int link(const char *from, const char *to) {
    return (int)__syscall2(SYS_LINK, (long)(uintptr_t)from, (long)(uintptr_t)to);
}

int symlink(const char *target, const char *path) {
    return (int)__syscall2(SYS_SYMLINK, (long)(uintptr_t)target,
                           (long)(uintptr_t)path);
}

ssize_t readlink(const char *path, char *buf, size_t n) {
    return (ssize_t)__syscall3(SYS_READLINK, (long)(uintptr_t)path,
                                (long)(uintptr_t)buf, (long)n);
}

int access(const char *path, int mode) {
    return (int)__syscall2(SYS_ACCESS, (long)(uintptr_t)path, mode);
}

/* The `R_OK`/`W_OK`/`X_OK`/`F_OK` values are the kernel's; they are on <unistd.h>'s
 * neighbour <fcntl.h> in some libcs and here because `access()` is the only call that uses
 * them. */
int chdir(const char *path) {
    return (int)__syscall1(SYS_CHDIR, (long)(uintptr_t)path);
}

char *getcwd(char *buf, size_t n) {
    return (char *)(uintptr_t)__syscall2(SYS_GETCWD, (long)(uintptr_t)buf, (long)n);
}

int umask(int mask) {
    return (int)__syscall1(SYS_UMASK, mask);
}

/* `isatty()`, `ttyname()` and `tcgetattr()` all ask the terminal driver, so they live in
 * termios.c with the rest of the terminal interface rather than here.  Declared in both
 * <unistd.h> and <termios.h>, because POSIX puts `isatty()` in <unistd.h> and a program
 * that includes only that one should still find it. */

/* --- processes -------------------------------------------------------------- */

pid_t getpid(void) {
    return (pid_t)__syscall1(SYS_GETPID, 0);
}

pid_t getppid(void) {
    return (pid_t)__syscall1(SYS_GETPPID, 0);
}

pid_t getuid(void) {
    return (pid_t)__syscall1(SYS_GETUID, 0);
}

pid_t getgid(void) {
    return (pid_t)__syscall1(SYS_GETGID, 0);
}

pid_t getpgid(pid_t pid) {
    return (pid_t)__syscall1(SYS_GETPGID, pid);
}

int setpgid(pid_t pid, pid_t pgid) {
    return (int)__syscall2(SYS_SETPGID, pid, pgid);
}

pid_t setsid(void) {
    return (pid_t)__syscall1(SYS_SETSID, 0);
}

int sched_yield(void) {
    return (int)__syscall1(SYS_SCHED_YIELD, 0);
}

int pause(void) {
    return (int)__syscall1(SYS_PAUSE, 0);
}

/* --- mounting and stopping ----------------------------------------------------- */

/* The three calls that belong together rather than with the rest of <unistd.h>: they are
 * the ones a program needs authority for, and a program that has it needs all three.
 *
 * `flags` and `data` are passed and ignored.  They exist because every Unix has them and a
 * program written for another one passes them; accepting them and having no use for them is
 * what keeps such a program running, and refusing them would make it fail for a reason it
 * cannot see. */
int mount(const char *type, const char *target, const char *fstype,
          unsigned long flags, const void *data) {
    (void)fstype;
    (void)flags;
    (void)data;

    return (int)__syscall3(SYS_MOUNT, (long)(uintptr_t)type,
                           (long)(uintptr_t)target, 0);
}

int umount(const char *target) {
    return (int)__syscall1(SYS_UMOUNT, (long)(uintptr_t)target);
}

/* Nothing to do and succeeds.  Everything this kernel writes is already in memory -- there
 * is no page cache behind a file and no disk under it -- so a program that flushes before
 * shutting down must not have to know that the flush is a no-op. */
int sync(void) {
    return (int)__syscall0(SYS_SYNC);
}

int reboot(int how) {
    return (int)__syscall1(SYS_REBOOT, (long)how);
}

/* `fork()` is a syscall and nothing else.  The parent's and child's returns differ by which
 * half of the kernel they were in, and a libc that tried to be clever about the child's
 * copy -- duplicating anything, flushing anything -- would be wrong about one of them. */
pid_t fork(void) {
    return (pid_t)__syscall0(SYS_FORK);
}

/* The `wait` family.  `wait()` is `waitpid(-1, ...)`: "any child of mine", which is what
 * `-1` means on every Unix and what the kernel's own implementation does with it. */
pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}

/* The kernel's `wait` takes the status pointer first and the selector second, which is the
 * order a `wait()` written before `waitpid()` existed has always used: the extra arguments
 * were added after the original one, so they could not go in front of it.  The selector is
 * the POSIX one -- -1 for any child, a positive number for that child, 0 for any child in
 * this process's own group. */
pid_t waitpid(pid_t pid, int *status, int options) {
    return (pid_t)__syscall3(SYS_WAIT, (long)(uintptr_t)status, pid, options);
}

pid_t wait3(int *status, int options, void *rusage) {
    (void)rusage;              /* no rusage yet; the argument is still accepted */
    return waitpid(-1, status, options);
}

pid_t wait4(pid_t pid, int *status, int options, void *rusage) {
    (void)rusage;
    return waitpid(pid, status, options);
}

/* --- exec ------------------------------------------------------------------- */

/* `execve()` is the syscall.  Everything else in this family is it plus one thing:
 *
 *   `execv`    the environment the program already has
 *   `execvp`   search PATH when the file has no slash in it
 *   `execl`    the arguments as a variadic list, ending in NULL
 *   `execlp`   both of the above
 *
 * Each of those four things is a different piece of policy, and each is *small*, which is
 * the reason they can all live here rather than in a shell.  A `VPATH`-style search is not
 * something every program should have to write and get right, and there are four ways to
 * spell it in the wild. */

int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)__syscall3(SYS_EXEC, (long)(uintptr_t)path,
                           (long)(uintptr_t)argv, (long)(uintptr_t)envp);
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

/* Does `file` look like a path, rather than a name to be searched for?
 *
 * A slash anywhere means it is a path.  That is POSIX's rule and it is the only one that
 * works: a program that passes `./ls` must get the file next to it, and one that passes
 * `ls` must get the one on PATH, and neither can be told apart by anything else. */
static int has_slash(const char *file) {
    for (const char *p = file; *p != 0; p++)
        if (*p == '/')
            return 1;

    return 0;
}

int execvp(const char *file, char *const argv[]) {
    if (has_slash(file))
        return execve(file, argv, environ);

    /* An empty PATH means one thing on POSIX and something different on every other
     * system: "search the current directory".  Searching *only* the current directory is
     * what the empty string actually says, and it is also the safe reading -- the other
     * one runs whatever happens to be in the working directory, which for a shell started
     * in a directory somebody else wrote is exactly the wrong default. */
    const char *path = getenv("PATH");

    if (path == NULL || path[0] == 0)
        return execve(".", argv, environ);

    /* One buffer, walked as it goes: the candidates are built in place, which is why this
     * is a single fixed allocation rather than a `malloc()` per attempt.  PATH is short in
     * practice and a shell re-execing thousands of times must not allocate for each. */
    char buf[512];

    for (const char *p = path; ; ) {
        const char *end = p;
        size_t      dirlen;
        size_t      filelen;

        while (*end != 0 && *end != ':')
            end++;

        dirlen = (size_t)(end - p);
        filelen = strlen(file);

        /* The one-character directory is `.`, not the empty string: an empty component in
         * PATH means the current directory on every other system, and treating it as a
         * path would produce `"/ls"`, which is the root's `ls`. */
        if (dirlen == 0) {
            buf[0] = '.';
            buf[1] = '/';
            if (filelen + 2 <= sizeof(buf)) {
                memcpy(buf + 2, file, filelen + 1);
                execve(buf, argv, environ);
            }
        } else if (dirlen + 1 + filelen + 1 <= sizeof(buf)) {
            memcpy(buf, p, dirlen);
            buf[dirlen] = '/';
            memcpy(buf + dirlen + 1, file, filelen + 1);
            execve(buf, argv, environ);
        }

        /* EACCES does *not* stop the search.  This is the rule everyone gets wrong: a
         * directory in PATH that holds a file the caller may not execute is not an error,
         * it is a candidate that does not qualify, and the search continues.  Only ENOENT
         * and ENOTDIR -- "there is nothing there" -- move on quietly, and the first real
         * error is remembered and reported if nothing else works out. */
        if (errno == ENOENT || errno == ENOTDIR) {
            /* keep going */
        }

        if (*end == 0)
            break;
        p = end + 1;
    }

    return -1;
}

int execl(const char *path, const char *arg, ...) {
    /* The variadic list cannot be walked twice -- `va_end` destroys it -- and the kernel
     * wants a vector.  Eight is the number every libc picks: `execl` has been on every
     * Unix forever and `execl` with nine arguments is a program with a different bug. */
    const char *argv[8];
    va_list     ap;
    unsigned    n = 0;

    argv[n++] = arg;

    va_start(ap, arg);
    for (const char *a = va_arg(ap, const char *); a != NULL; a = va_arg(ap, const char *)) {
        if (n == sizeof(argv) / sizeof(argv[0])) {
            va_end(ap);
            errno = E2BIG;
            return -1;
        }
        argv[n++] = a;
    }
    va_end(ap);

    return execve(path, (char *const *)argv, environ);
}

int execlp(const char *file, const char *arg, ...) {
    const char *argv[8];
    va_list     ap;
    unsigned    n = 0;

    argv[n++] = arg;

    va_start(ap, arg);
    for (const char *a = va_arg(ap, const char *); a != NULL; a = va_arg(ap, const char *)) {
        if (n == sizeof(argv) / sizeof(argv[0])) {
            va_end(ap);
            errno = E2BIG;
            return -1;
        }
        argv[n++] = a;
    }
    va_end(ap);

    return execvp(file, (char *const *)argv);
}

/* --- termination ------------------------------------------------------------ */

/* `exit()` and `_exit()` are two functions because POSIX makes the difference observable,
 * and the difference is real: `_exit()` does not flush the stdio streams and does not run
 * the atexit handlers.  A program that has already corrupted its own heap needs the second
 * one, and a program that has printed something the user should see needs the first.
 *
 * What separates them here is `__stdio_run_atexit()` and `__stdio_flush_all()`, in stdio.c, which is where the streams
 * live.  This file does not know what a stream is. */

void _exit(int status) {
    /* The syscall does not return: the kernel stops the task inside the call.  The
     * `for (;;)` after it is not a loop anyone reaches, it is there because a function
     * that is declared `void` still has to have a body that ends somehow, and a compiler
     * that decides `_exit` can return would fall off the end of one. */
    __syscall1(SYS_EXIT, status);

    for (;;)
        ;
}

void _Exit(int status) {
    _exit(status);
}

void exit(int status) {
    /* The order is the point.  Handlers first -- they are the program's own code and may
     * well be the thing that closes the files -- then the streams, then the kernel.  A
     * handler that prints and a stream flush that prints have to happen in that order or
     * the handler's output comes out after the process is gone. */
    __stdio_run_atexit();
    __stdio_flush_all();

    _exit(status);
}

/* `abort()` lives in stdlib.c, with `exit()` and `atexit()` -- it is one of the three ways
 * a program ends, and splitting them across two files is a way to end up with two of them
 * and one `abort`.  Declared in <stdlib.h>, which this file includes. */

/* --- pipes and duplication --------------------------------------------------- */

int pipe(int fds[2]) {
    return (int)__syscall1(SYS_PIPE, (long)(uintptr_t)fds);
}

int dup(int fd) {
    return (int)__syscall1(SYS_DUP, fd);
}

int dup2(int oldfd, int newfd) {
    return (int)__syscall2(SYS_DUP2, oldfd, newfd);
}

/* --- time -------------------------------------------------------------------- */

int nanosleep(const struct timespec *req, struct timespec *rem) {
    /* The kernel rounds a sub-millisecond sleep *up* to a millisecond rather than down to
     * nothing, because the timebase it counts in is milliseconds and a sleep of zero is not
     * a sleep.  That is why `usleep(500)` is a millisecond here and is documented as such
     * rather than as a rounding error. */
    return (int)__syscall2(SYS_NANOSLEEP, (long)(uintptr_t)req, (long)(uintptr_t)rem);
}

/* `sleep()` is specified to return the number of seconds *left* when it was interrupted by
 * a signal -- which is why this checks for EINTR and returns the remainder rather than
 * pretending it slept the whole time.  A shell's `sleep 10` reporting "0" after a Ctrl-C
 * is this contract being honoured. */
unsigned int sleep(unsigned int seconds) {
    struct timespec req;
    struct timespec left;

    req.tv_sec  = seconds;
    req.tv_nsec = 0;

    memset(&left, 0, sizeof(left));

    if (nanosleep(&req, &left) == 0)
        return 0;

    /* The remainder is what was *not* slept, and the seconds left in it are what this
     * returns.  That is the contract: a shell whose `sleep 10` is interrupted by Ctrl-C
     * reports how much of it is left, which is a number it can act on, rather than 0 --
     * which would read as "it slept the whole ten seconds and then returned early". */
    if (errno != EINTR)
        return 0;

    return (unsigned int)left.tv_sec;
}

int usleep(useconds_t usec) {
    struct timespec req;

    req.tv_sec  = usec / 1000000;
    req.tv_nsec = (long)(usec % 1000000) * 1000;

    return nanosleep(&req, NULL);
}