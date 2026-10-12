/* minx - <fcntl.h>
 *
 * The open flags and the control calls.  The flag *numbers* are the kernel's, which are
 * the same numbers every other Unix uses, so a program's source says `O_CREAT` and means
 * it -- and so this header has to agree with kernel/include/vfs.h exactly.
 */
#ifndef MINX_FCNTL_H
#define MINX_FCNTL_H

#include <sys/sysdep.h>

/* How a descriptor is opened.  The access mode is the low two bits and everything else is
 * a separate bit, which is what lets `open(path, O_RDWR | O_CREAT)` be one number. */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003      /* the access mode on its own */

#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

#define O_NONBLOCK  0x0800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x20000

/* The same values, spelled as a symbol another program can compare against -- which is
 * what a program that opens a directory and wants to check it opened a *directory* needs,
 * as opposed to a program that merely wanted to open one. */
int open(const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);

#endif /* MINX_FCNTL_H */
