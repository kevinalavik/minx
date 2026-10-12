/* minx - errno.
 *
 * One global per process, not one per thread.  Every process here has its own address
 * space, so a plain global is already private to the process -- which is the reason this is
 * simple rather than a mistake.  Two threads in one process would share it, and the fix
 * for that is a TLS model in the kernel that does not exist yet; recorded in
 * docs/STATUS.md rather than pretended around.
 *
 * A macro over a function, the way every other libc does it, so that `errno` can become
 * thread-local without changing a single line of any program that reads it.  The macro is
 * `(*__errno_location())` for exactly that reason and no other: the indirection is what
 * would let the storage move.
 */
#ifndef MINX_ERRNO_H
#define MINX_ERRNO_H

/* The negative numbers a call returns on failure.  Small, distinct, and all distinct from
 * every call number and from every plausible successful result. */
#define EPERM    1   /* operation not permitted                            */
#define ENOENT   2   /* no such file or directory                         */
#define ESRCH    3   /* no such process                                   */
#define EINTR    4   /* interrupted                                       */
#define EIO      5   /* input/output error                                */
#define ENXIO    6   /* no such device or address                          */
#define E2BIG    7   /* argument list too long                            */
#define ENOEXEC  8   /* exec format error                                 */
#define EBADF    9   /* bad file descriptor                               */
#define ECHILD  10   /* no child processes                                */
#define EAGAIN  11   /* resource temporarily unavailable                  */
#define ENOMEM  12   /* cannot allocate memory                            */
#define EACCES  13   /* permission denied                                 */
#define EFAULT  14   /* bad address                                       */
#define ENOTBLK 15   /* block device required                             */
#define EBUSY   16   /* resource busy                                     */
#define EEXIST  17   /* file exists                                       */
#define EXDEV   18   /* cross-device link                                 */
#define ENODEV  19   /* no such device                                    */
#define ENOTDIR 20   /* not a directory                                   */
#define EISDIR  21   /* is a directory                                    */
#define EINVAL  22   /* invalid argument                                  */
#define ENFILE  23   /* too many open files in system                     */
#define EMFILE  24   /* too many open files                               */
#define ENOTTY  25   /* not a typewriter                                  */
#define EFBIG   27   /* file too large                                    */
#define ENOSPC  28   /* no space left on device                           */
#define ESPIPE  29   /* illegal seek                                      */
#define EROFS   30   /* read-only filesystem                              */
#define EMLINK  31   /* too many links                                    */
#define EPIPE   32   /* broken pipe                                       */
#define ERANGE  34   /* numerical result out of range                     */
#define ENAMETOOLONG 36  /* file name too long                             */
#define ENOSYS  38   /* function not implemented                          */
#define ENOTEMPTY 39  /* directory not empty                             */
#define ELOOP   40   /* too many symbolic links encountered              */
#define ENOMSG  42   /* no message of the requested type                  */

/* `EAGAIN` and `EWOULDBLOCK` are the same number on every Unix with sockets. */
#define EWOULDBLOCK EAGAIN

/* The name of an error, for `perror()`.  In <errno.h> because that is where every program
 * that wants it already is -- `perror()` itself takes an errno, so a program printing one
 * has included this header already. */
char *strerror(int errnum);

/* The storage, and the one function that knows where it is. */
extern int *__errno_location(void);
#define errno (*__errno_location())

#endif /* MINX_ERRNO_H */