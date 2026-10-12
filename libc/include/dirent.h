/* minx - <dirent.h>
 *
 * Reading a directory, the way every other Unix spells it.
 *
 * A `DIR` is a buffer and a position, and the position is the offset `getdents()` gave
 * back rather than anything the libc invented -- which is what makes a directory walk
 * resumable across the two calls that every long-named directory needs.  The libc cannot
 * read the whole directory into memory in one call and hand out pointers into it, because
 * it does not know how big the directory is and cannot reallocate a buffer a caller is
 * holding pointers into.
 */
#ifndef MINX_DIRENT_H
#define MINX_DIRENT_H

#include <sys/sysdep.h>
#include <sys/types.h>

/* The `d_type` values, the same ones <sys/vfs.h> gives the kernel.  They are duplicated
 * here because a program that includes only <dirent.h> -- which is what a directory-reading
 * program does -- should find them. */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

/* One entry.
 *
 * `d_ino` is zero for the two names the kernel synthesises, `.` and `..`: they are not
 * nodes, and a program that uses `d_ino` to tell two directories apart will find them equal
 * -- which is true, and not useful.  `d_type` is real for them, because a walk that did not
 * have to `stat` them would be much cheaper, and `find` is exactly that walk. */
struct dirent {
    unsigned long  d_ino;
    unsigned long  d_off;        /* the offset to resume from */
    unsigned short d_reclen;
    unsigned char  d_type;
    unsigned char  d_pad[7];
    char           d_name[256];
};

/* A directory being read.
 *
 * `__dir_fd` is the descriptor and `__buf` the batch of entries `getdents()` last returned,
 * because that is what a `readdir()` over a directory *is*: refill a buffer, hand out
 * pointers into it, advance.  The alternative -- asking the kernel for one entry at a time
 * -- is a syscall per entry, and a `ls` of a large directory is the case where that hurts.
 *
 * `__count` is how many entries the last `getdents()` produced and `__at` which of them is
 * next, which is what makes the buffer's entries independent of each other: a caller that
 * keeps a `struct dirent *` across another `readdir()` is holding a pointer into a buffer
 * that has been refilled, and on every other Unix that pointer is stale too.  The thing to
 * copy, if a pointer has to survive, is `*entry`. */
typedef struct __DIR {
    int             __dir_fd;
    unsigned char   __buf[8192];
    size_t          __count;
    size_t          __at;
} DIR;

DIR  *opendir(const char *path);
DIR  *fdopendir(int fd);
int   readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result);
struct dirent *readdir(DIR *dirp);
int   closedir(DIR *dirp);
void  rewinddir(DIR *dirp);
long  telldir(DIR *dirp);
void  seekdir(DIR *dirp, long loc);

/* `dirfd()` is BSD rather than POSIX and is in <dirent.h> on every system that has it.  A
 * program that wants the descriptor of an open directory -- to `fstat()` it, or to
 * `fchdir()` into it -- needs it, and this kernel's `open()` has no `AT_EMPTY_PATH` to fall
 * back on. */
int   dirfd(DIR *dirp);

#endif /* MINX_DIRENT_H */