/* minx - the process system calls that are not files and not signals.
 *
 * Declared here rather than in syscall.h because syscall.h is the *boundary* -- the frame,
 * the dispatcher, the pointer checks -- and this is a list of handlers.  Keeping the list
 * out of the boundary means adding a call is one function and one line, and the boundary
 * file does not grow a hundred declarations nobody reads. */
#ifndef MINX_PROCSYS_H
#define MINX_PROCSYS_H

#include "cpu.h"

long sys_getppid(regs_t *regs);
long sys_getuid(regs_t *regs);
long sys_geteuid(regs_t *regs);
long sys_setuid(regs_t *regs);
long sys_getgid(regs_t *regs);
long sys_setsid(regs_t *regs);
long sys_setpgid(regs_t *regs);
long sys_getpgid(regs_t *regs);
long sys_uname(regs_t *regs);
long sys_nanosleep(regs_t *regs);
long sys_sched_yield(regs_t *regs);
long sys_times(regs_t *regs);
long sys_getrusage(regs_t *regs);
long sys_reboot(regs_t *regs);

#endif /* MINX_PROCSYS_H */
