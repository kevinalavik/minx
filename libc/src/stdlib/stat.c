/* minx - the stat family.
 *
 * One copy of the `struct stat` the kernel fills in, and three entry points over it.  The
 * structure is copied by the kernel in one `copy_to_user`, so the layout here has to be the
 * kernel's exactly -- field for field, in the same order, with the same widths -- and the
 * only way to notice that it is not is a program that reads a field and gets a plausible
 * wrong answer.
 *
 * That is why `libctest` reads `st_size` and `st_mode` and *not* just "did stat succeed":
 * a structure whose fields are shifted by one still returns zero.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int stat(const char *path, struct stat *out) {
    return (int)__syscall2(SYS_STAT, (long)(uintptr_t)path, (long)(uintptr_t)out);
}

int lstat(const char *path, struct stat *out) {
    return (int)__syscall2(SYS_LSTAT, (long)(uintptr_t)path, (long)(uintptr_t)out);
}

int fstat(int fd, struct stat *out) {
    return (int)__syscall2(SYS_FSTAT, fd, (long)(uintptr_t)out);
}

int chmod(const char *path, mode_t mode) {
    return (int)__syscall2(SYS_CHMOD, (long)(uintptr_t)path, mode);
}

int chown(const char *path, uid_t uid, gid_t gid) {
    return (int)__syscall3(SYS_CHOWN, (long)(uintptr_t)path, uid, gid);
}

int utimes(const char *path, const struct timeval times[2]) {
    return (int)__syscall2(SYS_UTIMES, (long)(uintptr_t)path, (long)(uintptr_t)times);
}

int truncate(const char *path, off_t len) {
    return (int)__syscall2(SYS_TRUNCATE, (long)(uintptr_t)path, len);
}

int ftruncate(int fd, off_t len) {
    return (int)__syscall2(SYS_FTRUNCATE, fd, len);
}

int mkdir(const char *path, mode_t mode) {
    return (int)__syscall2(SYS_MKDIR, (long)(uintptr_t)path, mode);
}

/* --- the type predicates -------------------------------------------------------- */

/* All six are the same shape, and the shape is the point.
 *
 * A mode with no type bits at all -- `0644`, which is what a program passes to `chmod()` --
 * is not a regular file, it is a mode with no type.  Testing `mode & S_IFREG` instead of
 * `(mode & S_IFMT) == S_IFREG` says it *is*, and that is how `ls` ends up printing a
 * directory as a file: the mode came from somewhere that lost the high bits. */
int S_ISDIR(mode_t m)  { return (m & S_IFMT) == S_IFDIR;  }
int S_ISREG(mode_t m)  { return (m & S_IFMT) == S_IFREG;  }
int S_ISLNK(mode_t m)  { return (m & S_IFMT) == S_IFLNK;  }
int S_ISCHR(mode_t m)  { return (m & S_IFMT) == S_IFCHR;  }
int S_ISBLK(mode_t m)  { return (m & S_IFMT) == S_IFBLK;  }
int S_ISFIFO(mode_t m) { return (m & S_IFMT) == S_IFIFO; }
int S_ISSOCK(mode_t m) { return (m & S_IFMT) == S_IFSOCK; }

/* --- `access()` ------------------------------------------------------------------ */

/* The `R_OK`/`W_OK`/`X_OK`/`F_OK` values live in <unistd.h>'s neighbour in most libcs, and
 * here because <unistd.h> does not include <sys/stat.h> -- a program that includes only
 * <unistd.h> and calls `access()` would otherwise not have the constants. */
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

/* `access()` is a syscall of its own and is in <unistd.h>, because POSIX puts it there and
 * because it does not touch a `struct stat`.  It is declared there; this comment is here
 * only to say that the constants are the ones the kernel checks against the *caller's*
 * uid, which is every process's uid 1 on this machine, and that `F_OK` is therefore
 * "does it exist" rather than "does it exist and this process may not read it". */