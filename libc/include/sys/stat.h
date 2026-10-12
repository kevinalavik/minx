/* minx - <sys/stat.h>
 *
 * The result of `stat()`.  The layout is the kernel's own, copied in one go, so the two
 * definitions have to agree exactly -- which is why this one is written out rather than
 * built from the kernel's header, and why the copy is here and not in <stdio.h>.
 */
#ifndef MINX_SYS_STAT_H
#define MINX_SYS_STAT_H

#include <sys/sysdep.h>
#include <time.h>

/* The file types, as bits in st_mode. */
#define S_IFMT   0170000u
#define S_IFDIR  0040000u
#define S_IFREG  0100000u
#define S_IFLNK  0120000u
#define S_IFCHR  0020000u
#define S_IFBLK  0060000u
#define S_IFIFO  0010000u
#define S_IFSOCK 0140000u

/* The permission bits. */
#define S_ISUID   04000
#define S_ISGID   02000
#define S_ISVTX   01000
#define S_IRWXU   0700
#define S_IRUSR   0400
#define S_IWUSR   0200
#define S_IXUSR   0100
#define S_IRWXG   0070
#define S_IRGRP   0040
#define S_IWGRP   0020
#define S_IXGRP   0010
#define S_IRWXO   0007
#define S_IROTH   0004
#define S_IWOTH   0002
#define S_IXOTH   0001

/* The type predicates.  Not macros over a mask alone: each one has to compare `mode & S_IFMT`
 * against the *whole* mask, because a mode with no type bits at all is not a directory. */
int S_ISDIR(mode_t m);
int S_ISREG(mode_t m);
int S_ISLNK(mode_t m);
int S_ISCHR(mode_t m);
int S_ISBLK(mode_t m);
int S_ISFIFO(mode_t m);
int S_ISSOCK(mode_t m);

/* The mode argument bits, which are a subset of the permission bits: a program cannot
 * create a file with a set-user-id bit by passing it to `open()`, because that bit is a
 * property of the *filesystem*, not a request. */
#define S_IRWXU_MODE 0700

struct stat {
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

int stat(const char *path, struct stat *out);
int lstat(const char *path, struct stat *out);
int fstat(int fd, struct stat *out);
int chmod(const char *path, mode_t mode);
int chown(const char *path, uid_t uid, gid_t gid);
int utimes(const char *path, const struct timeval times[2]);
int truncate(const char *path, off_t len);
int ftruncate(int fd, off_t len);
int mkdir(const char *path, mode_t mode);

#endif /* MINX_SYS_STAT_H */
