/* minx - reading a directory.
 *
 * `readdir()` over `getdents()`, one batch at a time.  The alternative -- asking the kernel
 * for one entry at a time -- is a syscall per entry, and a directory walk is the one place
 * where a program does that by the hundred.  So the libc keeps a buffer of entries and hands
 * out pointers into it, which is what every other libc does and the reason `readdir()`'s
 * result is only valid until the next call.
 *
 * Two things this file is careful about, and both are the same thing:
 *
 *   - The buffer holds *whole* records, and a record that does not fit is not a short one.
 *     `getdents()` already refuses to produce a partial record; the loop here stops on the
 *     first record whose `d_reclen` runs past what came back rather than trusting the
 *     count, because a count that disagrees with the bytes produces a name with no
 *     terminator, and every caller of `readdir()` treats that name as a C string.
 *
 *   - `.` and `..` are not filtered out.  Every Unix returns them, `ls -a` needs them, and
 *     `find` cannot terminate without `..`.  A libc that hid them would be a libc whose
 *     `readdir()` cannot be used to write `cd ..`.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* Where a `DIR` lives.
 *
 * Static storage, because a `readdir()` hands back a pointer into its own buffer and that
 * pointer has to stay valid until the next call -- which means the buffer cannot be on the
 * caller's stack, and cannot be reallocated while a pointer into it is held.  A fixed array
 * of a fixed size is therefore not a shortcut here, it is the only arrangement that gives
 * the contract `readdir()` promises.
 *
 * Sixteen at a time.  A program that opens sixteen directories and keeps them open -- which
 * is what a recursive walk with descriptors held does -- is a program this libc refuses, with
 * EMFILE set, which is a real answer.  A program that wanted more should be using `open()`
 * itself and `getdents()`. */
#define DIR_MAX 16

static DIR  dirs[DIR_MAX];
static int  dirs_used;

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);

    if (fd < 0)
        return NULL;

    return fdopendir(fd);
}

DIR *fdopendir(int fd) {
    if (dirs_used >= DIR_MAX) {
        /* Not closed on the caller's behalf: the descriptor was the caller's to pass in,
         * and closing it here would turn a "too many directories open" into a "that
         * descriptor is gone" on a path that had nothing to do with it. */
        errno = EMFILE;
        return NULL;
    }

    DIR *d = &dirs[dirs_used++];

    memset(d, 0, sizeof(*d));
    d->__dir_fd = fd;

    return d;
}

int closedir(DIR *d) {
    if (d == NULL) {
        errno = EINVAL;
        return -1;
    }

    int fd = d->__dir_fd;

    /* The slot is given back, so that the count means "directories currently open" rather
     * than "directories ever opened".  A `find` that visits a thousand directories and
     * closes each one would otherwise run out after sixteen.
     *
     * The index is computed by scanning rather than by `d - &dirs`, because `DIR *` and
     * `DIR[16]` are different types to the compiler even where they are the same bytes --
     * and a pointer subtraction that needs a cast to be legal is a cast away from being
     * wrong.  Sixteen entries, once per `closedir()`, is not a cost worth optimising. */
    size_t idx = 0;

    while (idx < (size_t)dirs_used && &dirs[idx] != d)
        idx++;

    if (idx < (size_t)dirs_used) {
        for (size_t i = idx; i + 1 < (size_t)dirs_used; i++)
            dirs[i] = dirs[i + 1];

        dirs_used--;
    }

    return close(fd);
}

int dirfd(DIR *d) {
    if (d == NULL) {
        errno = EINVAL;
        return -1;
    }

    return d->__dir_fd;
}

/* Fill the buffer.  Returns the number of bytes, or -1. */
static long dir_fill(DIR *d) {
    d->__at = 0;

    for (;;) {
        long n = __syscall3(SYS_GETDENTS, d->__dir_fd, (long)(uintptr_t)d->__buf,
                             (long)sizeof(d->__buf));

        if (n < 0) {
            /* This is the raw door, so it is a negative errno rather than -1 with `errno`
             * set, and the difference is the whole reason `__syscall_ret()` exists. */
            d->__count = 0;
            return -1;
        }

        d->__count = (size_t)n;

        return n;
    }
}

struct dirent *readdir(DIR *d) {
    if (d == NULL) {
        errno = EINVAL;
        return NULL;
    }

    /* The loop is because a `getdents()` that returns a record this libc cannot use -- one
     * longer than the buffer, which the kernel will not produce but which is exactly the
     * shape a corrupt or unexpected record would take -- should be skipped rather than
     * returned as a name with no terminator. */
    while (d->__at + sizeof(struct dirent) <= d->__count) {
        struct dirent *e = (struct dirent *)(void *)(d->__buf + d->__at);

        if (e->d_reclen < sizeof(struct dirent)) {
            /* Nothing usable in what came back.  Treat it as the end rather than looping:
             * a short record here means the stream is not what it claimed to be, and
             * reading past it would be reading whatever the allocator left. */
            d->__at = d->__count;
            break;
        }

        d->__at += e->d_reclen;

        /* The name is terminated here rather than trusted from the kernel, for the same
         * reason `readlink()` terminates what it reads: a name that is not terminated is a
         * name every caller reads past the end of. */
        e->d_name[sizeof(e->d_name) - 1] = 0;

        return e;
    }

    if (dir_fill(d) <= 0)
        return NULL;

    return readdir(d);
}

int readdir_r(DIR *d, struct dirent *entry, struct dirent **result) {
    struct dirent *e = readdir(d);

    if (e == NULL) {
        /* The reentrant form's whole reason to exist: the answer is in `*result`, not in
         * the return value, so two threads walking two directories cannot tell each other's
         * end-of-directory from their own. */
        if (result != NULL)
            *result = NULL;

        return 0;
    }

    if (entry != NULL)
        memcpy(entry, e, sizeof(*entry));

    if (result != NULL)
        *result = entry != NULL ? entry : e;

    return 0;
}

void rewinddir(DIR *d) {
    if (d == NULL)
        return;

    /* Straight back to offset 0, which is the beginning by definition -- and the reason
     * `rewinddir()` is cheap here rather than something that has to find an entry and ask
     * the kernel where it was. */
    lseek(d->__dir_fd, 0, SEEK_SET);

    d->__count = 0;
    d->__at    = 0;
}

long telldir(DIR *d) {
    if (d == NULL) {
        errno = EINVAL;
        return -1;
    }

    return lseek(d->__dir_fd, 0, SEEK_CUR);
}

void seekdir(DIR *d, long loc) {
    if (d == NULL)
        return;

    lseek(d->__dir_fd, loc, SEEK_SET);

    d->__count = 0;
    d->__at    = 0;
}

/* `getcwd()` lives in <unistd.h> and is a syscall here, so there is nothing for this file
 * to alias.  The declaration above exists only because <dirent.h> on some systems carries
 * it, and a program that includes only <dirent.h> and calls `getcwd()` should still link. */
char *getcwd_alias(void) {
    return getcwd(NULL, 0);
}