/* minx - the file-related system calls.
 *
 * They live here rather than in syscall.c because that file is the *boundary* -- the
 * argument frame, the pointer checks, the dispatcher -- and this is one caller of it.  A
 * handler is a function from a frame to a number, and the frame handling is the same for
 * every one of them; only what the kernel does with the result differs.
 *
 * The rule that shapes all of them: **a program can make the kernel do something wrong,
 * but it cannot make the kernel touch an address it chose.**  Every path and every buffer
 * crosses the boundary through syscall_copy_from_user(), syscall_copy_to_user() or
 * syscall_copy_string(), which check against the caller's own page tables first.
 */
#ifndef MINX_FSCALLS_H
#define MINX_FSCALLS_H

#include <stddef.h>

#include "cpu.h"

/* Open, close and the whole file-call family.  Each takes the frame and returns the value
 * the program reads; an error is a negative number and also recorded for the message. */
long fs_sys_open(regs_t *regs);
long fs_sys_close(regs_t *regs);
long fs_sys_lseek(regs_t *regs);
long fs_sys_stat(regs_t *regs);
long fs_sys_lstat(regs_t *regs);
long fs_sys_fstat(regs_t *regs);
long fs_sys_fsync(regs_t *regs);
long fs_sys_sync(regs_t *regs);
long fs_sys_readlink(regs_t *regs);
long fs_sys_getdents(regs_t *regs);
long fs_sys_dup(regs_t *regs);
long fs_sys_dup2(regs_t *regs);

long fs_sys_mkdir(regs_t *regs);
long fs_sys_rmdir(regs_t *regs);
long fs_sys_unlink(regs_t *regs);
long fs_sys_rename(regs_t *regs);
long fs_sys_link(regs_t *regs);
long fs_sys_symlink(regs_t *regs);
long fs_sys_chmod(regs_t *regs);
long fs_sys_chown(regs_t *regs);
long fs_sys_truncate(regs_t *regs);
long fs_sys_ftruncate(regs_t *regs);
long fs_sys_utimes(regs_t *regs);
long fs_sys_chdir(regs_t *regs);
long fs_sys_getcwd(regs_t *regs);
long fs_sys_umask(regs_t *regs);
long fs_sys_mount(regs_t *regs);
long fs_sys_umount(regs_t *regs);
long fs_sys_access(regs_t *regs);

long fs_sys_fcntl(regs_t *regs);
long fs_sys_ioctl(regs_t *regs);

/* `read` and `write` when the descriptor names a file rather than a pipe.  The pipe and
 * console paths are in syscall.c because they were there first and the descriptor kind
 * decides; these two are the file half of the same dispatch. */
long fs_sys_read_fd(int fd, uint64_t buf, size_t bytes);
long fs_sys_write_fd(int fd, const void *buf, size_t bytes);

#endif /* MINX_FSCALLS_H */