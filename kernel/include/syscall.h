/* minx - the user/kernel boundary.
 *
 * A program reaches the kernel through one vector, 128, with `int $0x80`.  The frame
 * that arrives is an ordinary `regs_t` in ring-3 shape, so the same dispatcher, the
 * same fault reporting and the same `iretq` serve a syscall and a device interrupt.
 * docs/DECISIONS.md says why this is the door rather than the `syscall` instruction.
 *
 * The one rule that everything else follows from: **a program can make the kernel do
 * something wrong, but it cannot make the kernel read or write an address it chose.**
 * Every pointer crosses through syscall_copy_from_user()/syscall_copy_to_user(), which
 * check the range against the caller's own page tables first.
 *
 * The call numbers are this kernel's own, grouped so that the ones added together are in
 * one place: read, write and the file calls at the bottom, the process calls in the
 * middle, the signal calls above them, and the memory calls at the top.  docs/SYSCALLS.md
 * is the same list written for a person.
 */
#ifndef MINX_SYSCALL_H
#define MINX_SYSCALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "pagetables.h"
#include "programs.h"

/* --- call numbers ------------------------------------------------------- */

/* Deliberately sparse and deliberately *not* Linux's numbers.
 *
 * Linux's are a contract with every binary ever compiled against it, and a kernel
 * that is not Linux must not pretend to be: a number that means something different
 * here would turn a program's mistake into a silent wrong answer rather than a
 * reported error.  These are this kernel's own, grouped so that `read`-shaped calls
 * start at 0 and grow upwards. */
enum {
    /* --- files --------------------------------------------------------- */
    SYS_READ    = 0,
    SYS_WRITE   = 1,
    SYS_OPEN    = 2,
    SYS_CLOSE   = 3,
    SYS_LSEEK   = 4,
    SYS_FSTAT   = 5,
    SYS_STAT    = 6,
    SYS_LSTAT   = 7,
    SYS_FSYNC   = 8,
    SYS_READLINK= 9,
    SYS_GETDENTS= 10,
    SYS_DUP     = 11,
    SYS_DUP2    = 12,
    SYS_SYNC    = 13,

    /* --- the filesystem, in memory -------------------------------------- */
    SYS_MKDIR   = 16,
    SYS_RMDIR   = 17,
    SYS_UNLINK  = 18,
    SYS_RENAME  = 19,
    SYS_LINK    = 20,
    SYS_SYMLINK = 21,
    SYS_CHMOD   = 22,
    SYS_CHOWN   = 23,
    SYS_TRUNCATE= 24,
    SYS_FTRUNCATE=25,
    SYS_UTIMES  = 26,
    SYS_CHDIR   = 27,
    SYS_GETCWD  = 28,
    SYS_UMASK   = 29,
    SYS_MOUNT   = 30,
    SYS_UMOUNT  = 31,
    SYS_ACCESS  = 32,

    /* --- descriptors and the terminal ----------------------------------- */
    SYS_FCNTL   = 33,
    SYS_IOCTL   = 34,

    /* --- processes ------------------------------------------------------ */
    SYS_EXIT    = 40,
    SYS_BRK     = 41,
    SYS_WRITEV  = 42,
    SYS_GETPID  = 43,
    SYS_GETPPID = 44,
    SYS_FORK    = 45,
    SYS_WAIT    = 46,
    SYS_PIPE    = 47,
    SYS_EXEC    = 48,
    SYS_SETSID  = 49,
    SYS_SETPGID = 50,
    SYS_GETPGID = 51,
    SYS_UNAME   = 52,
    SYS_GETUID  = 53,
    SYS_GETEUID = 54,
    SYS_SETUID  = 55,
    SYS_GETGID  = 56,
    SYS_NANOSLEEP = 57,
    SYS_SCHED_YIELD = 58,
    SYS_TIMES   = 59,
    SYS_GETRUSAGE = 60,
    SYS_REBOOT  = 61,
    SYS_PAUSE   = 62,

    /* --- signals -------------------------------------------------------- */
    SYS_KILL         = 64,
    SYS_SIGACTION    = 65,
    SYS_SIGRETURN    = 66,
    SYS_SIGPROCMASK  = 67,
    SYS_SIGPENDING   = 68,

    /* --- memory --------------------------------------------------------- */
    SYS_MMAP     = 72,
    SYS_MUNMAP   = 73,
    SYS_MPROTECT = 74,

    SYS_MAX = 96,
};

/* --- errors ------------------------------------------------------------- */

/* The negative numbers a call returns on failure.  Small, distinct, and all distinct
 * from every call number and from every plausible successful result. */
#define EPERM  1
#define ENOENT 2
#define ESRCH   3
#define EINTR  4
#define EIO    5
#define EBADF  9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY  16
#define ENOEXEC 8
#define EISDIR   21
#define EINVAL   22
#define ENFILE   23
#define EMFILE   24
#define ENOTDIR 20
#define ENOSPC   28
#define ESPIPE   29
#define EROFS    30
#define ENAMETOOLONG 36
#define ENOTEMPTY   39
#define EMLINK      31
#define EPIPE  32
#define EEXIST 17
#define ENODEV 19
#define ENOSYS 38
#define ENOTTY 25
#define ECHILD 10
#define ELOOP  40
#define ERANGE 34
#define E2BIG   7

/* The `wait` options, which live beside the errors because both are "answers a caller gets
 * back from a call" rather than things a call returns. */
#define WNOHANG 1
#define EXDEV   18
#define ELOOP   40
#define ERANGE  34
#define EOVERFLOW 75
#define EWOULDBLOCK EAGAIN

/* Record the error a handler wants to return, for the reporting the tests do.
 *
 * The handler still returns the value itself; this is for the *message*.  Without it a
 * program's only evidence that a call failed is the number it got back, and "the
 * program printed nothing and exited 0" and "the program printed nothing and its
 * write failed with a fault" look identical from outside. */
void syscall_set_errno(int err);
int  syscall_last_errno(void);

/* Set the error and `return` the conventional negative value, so a handler reads as
 * one line rather than two. */
#define errno_return(e)  do { syscall_set_errno(e); return -(long)(e); } while (0)

/* --- iovec -------------------------------------------------------------- */

#define IOV_MAX 16

struct iovec {
    uint64_t base;
    uint64_t len;
};

/* --- dispatch ----------------------------------------------------------- */

extern void isr_stub_128(void);

/* Install the gate and the handler table.  Called once, before user space exists. */
int  syscall_init(void);

/* The single entry point for every syscall, installed as the handler for
 * VECTOR_SYSCALL.  Returns the call's result in `regs->rax`, which is where the
 * program will read it. */
void syscall_dispatch(regs_t *regs);

/* The name of a call, for a message.  "?" for a number nothing is installed for. */
const char *syscall_name(unsigned nr);

/* The i'th integer argument of the call this frame carries, in the System V order:
 * rdi, rsi, rdx, rcx, r8, r9.  Exposed because a handler defined in another file is still
 * a handler, and having each one open-code the same six-way switch would put six copies of
 * the same mistake in the tree. */
uint64_t syscall_arg(regs_t *regs, unsigned n);

/* --- copying between the boundary ---------------------------------------- */

/* True when every page the range covers is mapped, user-accessible, and -- for a
 * write -- writable, in the *calling task's* address space.
 *
 * This is a walk of the page tables rather than a fault and recover, and that is a
 * deliberate choice.  Faulting and recovering would need the fault handler to
 * recognise "this fault was mine, on purpose" and would have to re-run the faulting
 * instruction, which for a string copy is a partially completed one.  Walking first
 * is exact, needs no recovery, and a range it rejects is a range that would have
 * faulted. */
bool syscall_user_range_ok(uint64_t addr, size_t bytes, bool write);

/* As above, against a named address space.  For callers that are not the running task --
 * the ELF loader, filling in a program that has not started. */
bool syscall_user_range_ok_in(page_table_t *tables, uint64_t addr, size_t bytes,
                              bool write);

/* Resolve any copy-on-write page in a range the caller is about to write to.
 *
 * Call this before syscall_user_range_ok() on a *write*.  A page a fork has read-only is
 * about to be copied, not refused, and the range check cannot tell the two apart. */
void syscall_prepare_write(uint64_t addr, size_t bytes);

/* Copy out of / into user memory.  These are the only ways a program-supplied address
 * should ever be dereferenced, and both check before touching. */
bool syscall_copy_from_user(void *dst, uint64_t src, size_t bytes);
bool syscall_copy_to_user(uint64_t dst, const void *src, size_t bytes);

/* Read a NUL-terminated string out of user memory, into a kernel buffer of at most
 * `max` bytes including the terminator.  Returns the length, or -EFAULT if the bytes are
 * not there and -ENAMETOOLONG if there is no terminator inside `max` -- a program that
 * hands over an unterminated block has handed over bytes, not a name, and reading past it
 * to find out would be reading a page it never offered.
 *
 * Two rules make this different from copying a fixed-size block, and both of them were
 * bugs: it stops at the NUL rather than at `max`, so a nine-byte path does not need 256
 * mapped bytes; and it never reads past the end of the page the cursor is on, so a string
 * at the top of a program's stack does not need the page above that one either.
 *
 * `syscall_copy_cstring()` is that function.  `syscall_copy_string()` is the same thing
 * under the name the filesystem half uses, and both live in kernel/lib/syscall.c. */
long syscall_copy_cstring(uint64_t src, char *dst, size_t max);
long syscall_copy_string(uint64_t src, char *dst, size_t max);

/* --- descriptors --------------------------------------------------------- */

/* Write to a descriptor.  1 and 2 are the console -- both the framebuffer and the
 * serial mirror, so a program's output is visible in a screenshot and in a log.  Any
 * other descriptor is EBADF rather than a silent success, because a program that
 * believes it wrote somewhere is worse off than one that knows it did not. */
long sys_fd_write(int fd, const void *buf, size_t bytes);
long sys_fd_read(int fd, void *buf, size_t bytes);

/* As above, for a scatter/gather list, used by writev. */
long sys_write_to_fd(int fd, uint64_t base, uint64_t len);

/* --- running a program -------------------------------------------------- */

struct task;

/* Load `image` into a new user task, queue it, and return it.  `image` is the ELF
 * file's bytes; `bytes` how many there are.  Returns NULL and says why on the console
 * if anything about it is refused.
 *
 * The one call that turns three subsystems into a program.  It is here rather than in
 * the ELF loader because the loader knows how to map an image and knows nothing about
 * tasks, and this is where the two meet. */
struct task *program_load_and_run(const char *name, uint32_t stack_pages);

/* Create a task and start one registered program in it. */
struct task *program_start(const struct program_image *p);

/* Create a task and start the program at `path`, reading its bytes out of the filesystem.
 * The counterpart to program_load_and_run(): one starts a program the kernel carries inside
 * itself and one starts a file, and both end in the same loader. */
struct task *program_start_image(const char *name, const void *image, size_t bytes,
                                 uint32_t stack_pages);
struct task *program_start_path(const char *path, uint32_t stack_pages);

/* Load `image` into the *existing* task `t`, throwing away whatever address space and
 * frame it had.  This is `exec`, and it is split from program_load_and_run() because
 * creating a task and replacing one are different jobs: the first allocates a stack and an
 * address space, the second frees both and builds new ones, and a program cannot be allowed
 * to fail half way through and be left with neither.
 *
 * On failure the task is untouched -- still running the old program, still able to report
 * the error -- because the alternative is a process with no program and no way to be told
 * why.
 *
 * The new entry point and stack pointer come back through `entry_out` and `rsp_out`
 * because the caller has to patch them into something `saved_rsp` cannot reach.  An `exec`
 * is made by a task that is *running*, and such a task returns to ring 3 through the
 * interrupt frame it is in the middle of -- not through `saved_rsp`, which only says where
 * to resume it if it were switched out first.  Patching only `saved_rsp` builds a program
 * that starts correctly the next time it is scheduled and runs the *old* program right now,
 * which is the more confusing of the two failures because the first switch looks like it
 * worked. */
struct task *program_exec_into(struct task *t, const char *name, const void *image,
                               size_t bytes, uint32_t stack_pages,
                               const char *const *argv, const char *const *envp,
                               uint64_t *entry_out, uint64_t *rsp_out);

/* --- what a program did -------------------------------------------------- */

/* Called by the exit handler, and by anything else that ends a task, before the task's
 * stack is abandoned.  `code` is the program's own idea of how it went; the self tests
 * read it back through syscall_exit_record().
 *
 * The status and the task it belongs to are published *together*, and have to be.  A
 * program that forks has children whose exits are not its own report, so a reader has to be
 * able to ask whose exit it is looking at as well as what the exit was -- and it has to be
 * able to ask both without being able to catch them changing in between.  Two separate
 * words cannot do that: a child exiting between the reader's read of the status and its
 * read of the pid hands it one task's status next to another task's pid, a pair that never
 * existed.  The self tests hit exactly that, as "forktest exited 4" -- child 45's status
 * beside forktest's pid -- and as a program that reported it never exited at all.
 *
 * `syscall_last_exit()` and `syscall_exit_pid()` read the same pair one half at a time and
 * are for a caller that genuinely does not care whose it is. */
void syscall_note_exit(int code);

/* Both halves at once.  `*pid` is 0 and `*code` is -1 when nothing has exited since the
 * last reset. */
void syscall_exit_record(int *code, uint64_t *pid);

int  syscall_last_exit(void);
void syscall_last_errno_reset(void);
uint64_t syscall_exit_pid(void);

/* Counted so that a program calling a number nothing implements is visible as a count
 * and not only as whatever it did about it. */
void syscall_note_unknown(unsigned long nr);
uint64_t syscall_unknown_count(void);

#endif /* MINX_SYSCALL_H */