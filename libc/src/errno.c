/* minx - `errno`'s storage, and the name for an error.
 *
 * Two small things that are in one file because they are the same thing: what `errno` is
 * made of, and what the numbers in it mean.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Where `errno` lives.
 *
 * A function returning a pointer rather than a global, because that is what lets `errno`
 * become thread-local without changing a line of any program that reads it.  The macro in
 * <errno.h> is `(*__errno_location())`, and everything that would have to change if the
 * storage moved is in this one function.
 *
 * A plain static, not thread-local, and the reason is stated in <errno.h>: a process has
 * its own address space so the global is already private to the process, and two *threads*
 * inside one process would share it.  This kernel has no threads -- a task is a process --
 * so there is nothing to be wrong about yet, and when there is, this is the one line that
 * changes. */
static int errno_storage;

int *__errno_location(void) {
    return &errno_storage;
}

/* --- the names -------------------------------------------------------------------- */

/* One table, in errno number order, with the gaps filled by a name for the numbers this
 * kernel defines but that are not commonly used.
 *
 * The table is indexed by the error number, so a number with no entry has to be *handled*,
 * not merely skipped -- which is what makes it safe to add an errno to the kernel without
 * touching this file.  A program printing `strerror(137)` gets "error 137", which is
 * useless but not a crash; a program printing `strerror()` of a NULL would get one. */
const char *const errno_names[] = {
    [0]                = "no error",
    [EPERM]            = "operation not permitted",
    [ENOENT]           = "no such file or directory",
    [ESRCH]            = "no such process",
    [EINTR]            = "interrupted system call",
    [EIO]              = "input/output error",
    [ENXIO]            = "no such device or address",
    [E2BIG]            = "argument list too long",
    [ENOEXEC]          = "exec format error",
    [EBADF]            = "bad file descriptor",
    [ECHILD]           = "no child processes",
    [EAGAIN]           = "resource temporarily unavailable",
    [ENOMEM]           = "cannot allocate memory",
    [EACCES]           = "permission denied",
    [EFAULT]           = "bad address",
    [ENOTBLK]          = "block device required",
    [EBUSY]            = "resource busy or locked",
    [EEXIST]           = "file exists",
    [EXDEV]            = "cross-device link",
    [ENODEV]           = "no such device",
    [ENOTDIR]          = "not a directory",
    [EISDIR]           = "is a directory",
    [EINVAL]           = "invalid argument",
    [ENFILE]           = "too many open files in system",
    [EMFILE]           = "too many open files",
    [ENOTTY]           = "not a typewriter",
    [EFBIG]            = "file too large",
    [ENOSPC]           = "no space left on device",
    [ESPIPE]           = "illegal seek",
    [EROFS]            = "read-only file system",
    [EMLINK]           = "too many links",
    [EPIPE]            = "broken pipe",
    [ERANGE]           = "numerical result out of range",
    [ENAMETOOLONG]     = "file name too long",
    [ENOSYS]           = "function not implemented",
    [ENOTEMPTY]        = "directory not empty",
    [ELOOP]            = "too many levels of symbolic links",
    [ENOMSG]           = "no message of desired type",
};

char *strerror(int errnum) {
    static char unknown[32];
    const unsigned count = sizeof(errno_names) / sizeof(errno_names[0]);

    /* Both bounds are checked.  The low one because a program that reports a failed call
     * can easily pass a negative number by mistake, and the high one because this table
     * is a snapshot: a kernel that grows an errno would otherwise have this function read
     * past the end of it. */
    if (errnum >= 0 && (unsigned)errnum < count && errno_names[errnum] != NULL)
        return (char *)(uintptr_t)errno_names[errnum];

    snprintf(unknown, sizeof(unknown), "error %d", errnum);

    return unknown;
}