/* minx - <sys/wait.h>
 *
 * How a child's exit is reported, and the one decoding rule every program depends on.
 */
#ifndef MINX_SYS_WAIT_H
#define MINX_SYS_WAIT_H

#include <sys/sysdep.h>

/* A child that exited normally has a status of 0..255.  A child killed by a signal has
 * *its own* status, which is not an exit code at all -- and a program that reads `status`
 * without decoding it will report a program that was killed as having "failed with status
 * 9", which is a fact about the kernel and not about the program.
 *
 * These three make the difference visible, which is the only reason they exist. */
#define WIFEXITED(s)      (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)    (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)    (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)       ((s) & 0x7f)
#define WIFSTOPPED(s)     (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)       (((s) >> 8) & 0xff)
#define WCOREDUMP(s)      ((s) & 0x80)

/* `waitid()`'s `WNOWAIT`, for the "tell me but do not consume it" case. */
#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8
#define WEXITED  4
#define WSTOPPED 3
#define WNOWAIT  0x1000000

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait3(int *status, int options, void *rusage);
pid_t wait4(pid_t pid, int *status, int options, void *rusage);

#endif /* MINX_SYS_WAIT_H */
