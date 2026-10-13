/* minx - <unistd.h>
 *
 * The POSIX calls that take a file descriptor, a process, or nothing at all.  Each one is
 * a wrapper over <sys/sysdep.h> and nothing more: the interesting decisions belong to the
 * kernel, and a wrapper's only job is to get the arguments into registers in the order the
 * ABI wants them.
 */
#ifndef MINX_UNISTD_H
#define MINX_UNISTD_H

#include <sys/sysdep.h>
#include <time.h>

/* --- descriptors ------------------------------------------------------------ */

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);

int  close(int fd);
off_t lseek(int fd, off_t off, int whence);

int  open(const char *path, int flags, ...);
int  creat(const char *path, mode_t mode);

int  unlink(const char *path);
int  rmdir(const char *path);
int  remove(const char *path);            /* unlink, or rmdir for a directory */
int  rename(const char *from, const char *to);
int  link(const char *from, const char *to);
int  symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buf, size_t n);

int  access(const char *path, int mode);
int  chdir(const char *path);
char *getcwd(char *buf, size_t n);
int  umask(int mask);

int  isatty(int fd);
char *ttyname(int fd);

/* --- processes -------------------------------------------------------------- */

pid_t  getpid(void);
pid_t  getppid(void);
pid_t  fork(void);
int    execv(const char *path, char *const argv[]);
int    execve(const char *path, char *const argv[], char *const envp[]);
int    execvp(const char *file, char *const argv[]);
int    execl(const char *path, const char *arg, ...);
int    execlp(const char *file, const char *arg, ...);
void   _exit(int status);
void   exit(int status);
void   _Exit(int status);

pid_t  wait(int *status);
pid_t  waitpid(pid_t pid, int *status, int options);
pid_t  getpgid(pid_t pid);
int    setpgid(pid_t pid, pid_t pgid);
pid_t  setsid(void);
int    pause(void);
int    nanosleep(const struct timespec *req, struct timespec *rem);
int    sched_yield(void);

/* --- pipes and duplication -------------------------------------------------- */

int  pipe(int fds[2]);
int  dup(int fd);
int  dup2(int oldfd, int newfd);

/* --- time ------------------------------------------------------------------- */

unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);

/* --- the whence values, which <stdio.h> also wants -------------------------- */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* The access modes, for `access()`.
 *
 * Here and in <fcntl.h> because both headers want them and because they are two different
 * vocabularies that happen to share a spelling: these say "may I *do* this", `O_*` say "in
 * what mode may I *open* this", and confusing them is how a program ends up asking whether
 * a file is readable with the wrong constant. */
#define F_OK 0    /* does it exist                        */
#define X_OK 1    /* may I execute it                      */
#define W_OK 2    /* may I write it                        */
#define R_OK 4    /* may I read it                         */

/* --- the flags <fcntl.h> also wants ----------------------------------------- */
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4

/* FD_CLOEXEC is the only flag either direction has yet, and it is worth having now: a
 * program that opens a file and then forks and execs should not leak the descriptor into
 * the child, and the only way to ask for that is a flag that is *per descriptor*. */
#define FD_CLOEXEC 1

#endif /* MINX_UNISTD_H */