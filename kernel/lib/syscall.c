/* minx - user/kernel boundary.
 *
 * A user program reaches the kernel through one vector, 128, with `int $0x80`.  It goes
 * through the ordinary interrupt path: the same assembly stub, the same dispatcher,
 * the same `regs_t` frame, and `iretq` to get back out.  That is deliberate, and
 * docs/DECISIONS.md explains why the `syscall` instruction is not the door yet even
 * though it is the better instruction.
 *
 * The rule everything here follows from: **a program can make the kernel do something
 * wrong, but it cannot make the kernel touch an address it chose.**  Every pointer a
 * program hands over is checked against the caller's own page tables by
 * syscall_user_range_ok() before anything dereferences it.  Checking is a walk, not a
 * fault and a recovery: faulting would need the fault handler to recognise a fault it
 * caused on purpose, and would have to re-run a half-completed string copy.
 *
 * Two more things that are decisions rather than consequences:
 *
 *  - **The call numbers are this kernel's own, not Linux's.**  Linux's numbers are a
 *    contract with every binary ever built against it; a number that meant something
 *    else here would turn a program's mistake into a silent wrong answer instead of a
 *    reported error.
 *
 *  - **A call that cannot be completed returns an error rather than blocking.**  A read
 *    with nothing to read has to block eventually, but blocking inside the kernel means
 *    the task has to be put on a wait queue and something has to wake it -- which is
 *    phase 7's problem.  Until then ENOSYS says plainly that the door exists and the
 *    room behind it does not, which is honest and which a program can act on.  What it
 *    must never do is block with interrupts disabled, because that stops the machine.
 */
#include "fscalls.h"
#include "memsys.h"
#include "procsys.h"
#include "power.h"
#include "signal.h"
#include "tty.h"
#include "vfs.h"
#include "syscall.h"

#include "console.h"
#include "fd.h"
#include "programs.h"
#include "elf.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "apic.h"
#include "task.h"
#include "vectors.h"
#include "vmm.h"
#include "serial.h"

/* --- the table ---------------------------------------------------------- */

typedef long (*syscall_fn_t)(regs_t *regs);

static syscall_fn_t handlers[SYS_MAX];

const char *syscall_name(unsigned nr) {
    static const char *const names[SYS_MAX] = {
        [SYS_READ]       = "read",
        [SYS_WRITE]      = "write",
        [SYS_OPEN]       = "open",
        [SYS_CLOSE]      = "close",
        [SYS_LSEEK]      = "lseek",
        [SYS_FSTAT]      = "fstat",
        [SYS_STAT]       = "stat",
        [SYS_LSTAT]      = "lstat",
        [SYS_FSYNC]      = "fsync",
        [SYS_READLINK]   = "readlink",
        [SYS_GETDENTS]   = "getdents",
        [SYS_DUP]        = "dup",
        [SYS_DUP2]       = "dup2",
        [SYS_SYNC]       = "sync",
        [SYS_MKDIR]      = "mkdir",
        [SYS_RMDIR]      = "rmdir",
        [SYS_UNLINK]     = "unlink",
        [SYS_RENAME]     = "rename",
        [SYS_LINK]       = "link",
        [SYS_SYMLINK]    = "symlink",
        [SYS_CHMOD]      = "chmod",
        [SYS_CHOWN]      = "chown",
        [SYS_TRUNCATE]   = "truncate",
        [SYS_FTRUNCATE]  = "ftruncate",
        [SYS_UTIMES]     = "utimes",
        [SYS_CHDIR]      = "chdir",
        [SYS_GETCWD]     = "getcwd",
        [SYS_UMASK]      = "umask",
        [SYS_MOUNT]      = "mount",
        [SYS_UMOUNT]     = "umount",
        [SYS_ACCESS]     = "access",
        [SYS_FCNTL]      = "fcntl",
        [SYS_IOCTL]      = "ioctl",
        [SYS_EXIT]       = "exit",
        [SYS_BRK]        = "brk",
        [SYS_WRITEV]     = "writev",
        [SYS_GETPID]     = "getpid",
        [SYS_GETPPID]    = "getppid",
        [SYS_FORK]       = "fork",
        [SYS_WAIT]       = "wait",
        [SYS_PIPE]       = "pipe",
        [SYS_EXEC]       = "exec",
        [SYS_SETSID]     = "setsid",
        [SYS_SETPGID]    = "setpgid",
        [SYS_GETPGID]    = "getpgid",
        [SYS_UNAME]      = "uname",
        [SYS_GETUID]     = "getuid",
        [SYS_GETEUID]    = "geteuid",
        [SYS_SETUID]     = "setuid",
        [SYS_GETGID]     = "getgid",
        [SYS_NANOSLEEP]  = "nanosleep",
        [SYS_SCHED_YIELD]= "sched_yield",
        [SYS_GETCPU]     = "getcpu",
        [SYS_TIMES]      = "times",
        [SYS_GETRUSAGE]  = "getrusage",
        [SYS_REBOOT]     = "reboot",
        [SYS_PAUSE]      = "pause",
        [SYS_KILL]       = "kill",
        [SYS_SIGACTION]  = "sigaction",
        [SYS_SIGRETURN]  = "sigreturn",
        [SYS_SIGPROCMASK]= "sigprocmask",
        [SYS_SIGPENDING] = "sigpending",
        [SYS_MMAP]       = "mmap",
        [SYS_MUNMAP]     = "munmap",
        [SYS_MPROTECT]   = "mprotect",
    };

    return (nr < SYS_MAX && names[nr] != NULL) ? names[nr] : "?";
}

/* --- what a program did, for the tests and for a message ----------------- */

static volatile int last_errno;
static volatile uint64_t unknown_calls;

/* The last exit, as one publishable thing.
 *
 * `exit_seq` is odd exactly while a writer is between the two halves and even when it is
 * not, which is what lets a reader notice that it read across a write and ask again.  A
 * plain seqlock rather than a lock because the reader is a test polling once a millisecond
 * and the writer is a process that is on its way out: neither side can afford to block the
 * other, and neither side is in a hurry.
 *
 * `exit_pid` is deliberately not "which task exited" in the sense of the *right* answer --
 * it is whichever task exited last, which is all "the last exit" has ever meant here.  What
 * it buys is that the pair cannot be torn: a reader gets one task's status with that same
 * task's pid, or it is told to try again. */
static volatile uint32_t exit_seq;
static volatile int      exit_code = -1;
static volatile uint64_t exit_pid;

void syscall_set_errno(int err) { last_errno = err; }
int  syscall_last_errno(void)   { return last_errno; }

void syscall_last_errno_reset(void) {
    last_errno = 0;

    /* Odd-then-even rather than two plain stores, so that a reader spinning in
     * `syscall_exit_record()` cannot observe the reset half-applied and read a stale pid
     * with the fresh "nothing has exited" code. */
    __atomic_add_fetch(&exit_seq, 1, __ATOMIC_ACQ_REL);
    exit_code = -1;
    exit_pid  = 0;
    __atomic_add_fetch(&exit_seq, 1, __ATOMIC_ACQ_REL);
}

void syscall_note_exit(int code) {
    task_t *t = sched_current();

    __atomic_add_fetch(&exit_seq, 1, __ATOMIC_ACQ_REL);   /* odd: a write is in flight */
    exit_code = code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    exit_pid = (t != NULL) ? t->id : 0;
    __atomic_add_fetch(&exit_seq, 1, __ATOMIC_ACQ_REL);   /* even: the pair is whole */
}

void syscall_exit_record(int *code, uint64_t *pid) {
    for (;;) {
        uint32_t before = __atomic_load_n(&exit_seq, __ATOMIC_ACQUIRE);
        int c;
        uint64_t p;

        if (before & 1u)
            continue;

        c = exit_code;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        p = exit_pid;

        if (__atomic_load_n(&exit_seq, __ATOMIC_ACQUIRE) != before)
            continue;               /* a write happened; the pair is not the one we read */

        if (code != NULL)
            *code = c;
        if (pid != NULL)
            *pid = p;
        return;
    }
}

int      syscall_last_exit(void) {
    int code;

    syscall_exit_record(&code, NULL);
    return code;
}

uint64_t syscall_exit_pid(void) {
    uint64_t pid;

    syscall_exit_record(NULL, &pid);
    return pid;
}

void     syscall_note_unknown(unsigned long nr) {
    (void)nr;
    __atomic_add_fetch(&unknown_calls, 1, __ATOMIC_ACQ_REL);
}
uint64_t syscall_unknown_count(void) { return unknown_calls; }

/* --- arguments ---------------------------------------------------------- */

/* The argument registers, in order.
 *
 * The System V C ABI passes the first six integer arguments in rdi, rsi, rdx, rcx, r8
 * and r9, and a kernel boundary has the same shape -- Linux takes a program's arguments
 * in exactly those registers.  So reading them is picking the right field out of the
 * frame rather than translating anything.
 *
 * Reading all six is safe whatever the program passed: the frame always has six.  What
 * is *not* safe is a handler reading a sixth argument from a call that only has two,
 * because then the answer depends on what the program happened to leave in rcx.  So
 * each handler reads exactly the arguments its own call defines, and this only exists
 * to make those six lines one line each. */
uint64_t syscall_arg(regs_t *regs, unsigned n) {
    switch (n) {
    case 0:  return regs->rdi;
    case 1:  return regs->rsi;
    case 2:  return regs->rdx;
    case 3:  return regs->rcx;
    case 4:  return regs->r8;
    case 5:  return regs->r9;
    default: return 0;
    }
}

/* Every handler in this file and in signal.c reads its arguments through this. */
static inline uint64_t arg(regs_t *regs, unsigned n) {
    return syscall_arg(regs, n);
}

/* --- checking a program's pointer ---------------------------------------- */

bool syscall_user_range_ok(uint64_t addr, size_t bytes, bool write) {
    return syscall_user_range_ok_in(vmm_tables(), addr, bytes, write);
}

/* As syscall_user_range_ok(), but against a named address space rather than the running
 * task's.
 *
 * The running task's is the right answer for a syscall, where the running task *is* the
 * caller.  It is the wrong answer for the ELF loader, which asks whether an address in a
 * program that has not started yet is mapped -- and asking its own tables instead reports
 * that it is not, which sends the loader into a fallback that maps over a page the
 * program already has. */
bool syscall_user_range_ok_in(page_table_t *tables, uint64_t addr, size_t bytes,
                              bool write) {
    if (!vmm_is_user_addr(addr))
        return false;

    /* Wrapping is not a range: `addr + bytes` coming back below `addr` means the end
     * is past the top of the address space, and every check below would be applied to
     * addresses the program never meant. */
    if (bytes != 0 && addr + bytes < addr)
        return false;

    if (tables == NULL)
        return false;

    /* Every page the range touches, *including the last one*.
     *
     * `off < bytes` steps by a page from the start, so it checks the pages at the start of
     * each page-sized step -- and a range that does not end on a boundary is never checked
     * at its far end.  `read(fd, buf, 4096)` with `buf` one byte before a page boundary
     * passes a check that looked at one page and copied across two.
     *
     * That is the difference between EFAULT and a kernel page fault, and it is worth being
     * exact about which is which: the check is the only thing standing between a program's
     * pointer and a `memcpy` in supervisor mode, so every byte it does not look at is a
     * byte the CPU will fault on. */
    /* Every page the range touches.
     *
     * The page at the *start* of each 4096-byte step is not the page at the end of the
     * range, and a range that starts near the end of a page runs into the next one.  So
     * this walks *page starts*: the first byte of the range, then the start of the next
     * page, until the start is at or past the end.
     *
     * Getting this wrong is not a small thing: the check is the only thing between a
     * program's pointer and a `memcpy` in supervisor mode.  A version that tested
     * `off < bytes` with `off += page_size` accepted `read(fd, buf, 256)` with `buf` 176
     * bytes below the top of a page, and the copy then ran off the end of the program's
     * stack into a page that is not there -- which is a kernel panic in a shell that had
     * done nothing wrong.
     *
     * An empty range is a range with no pages in it, and is legal wherever its address is
     * a legal user address.  `phase6-pointers` checks exactly that, because a kernel that
     * answered "no" for an empty range cannot distinguish "you passed nothing" from "you
     * passed a bad pointer" -- and neither can a program. */
    if (bytes == 0)
        return true;

    uint64_t end = addr + bytes;

    for (uint64_t a = addr; a < end;
         a = (a & ~(uint64_t)(page_size - 1)) + page_size) {
        pte_t leaf = pt_get(tables, a);

        if (!pte_present(leaf) || !pte_user(leaf))
            return false;
        if (write && !pte_writable(leaf))
            return false;
    }

    return true;
}

bool syscall_copy_from_user(void *dst, uint64_t src, size_t bytes) {
    if (bytes == 0)
        return true;

    if (!syscall_user_range_ok(src, bytes, false)) {
        /* Named, because "EFAULT" is a statement about a pointer and this is the only place
         * that knows which pointer.  A program that hands over a bad one gets an error; the
         * next person reading the log gets the address and the length, which is the whole
         * difference between "something was wrong with a pointer" and "this is where". */
        task_t *self = sched_current();

        kprintf("[warn] read of %#lx+%zu from task %lu refused: not mapped\n",
                (unsigned long)src, bytes,
                self != NULL ? (unsigned long)self->id : 0);
        return false;
    }

    __builtin_memcpy(dst, (const void *)(uintptr_t)src, bytes);
    return true;
}

/* Make every page of a write range writable in the *calling task's* address space,
 * resolving any copy-on-write page it finds on the way.
 *
 * This exists because `fork` takes write permission away from *both* sides -- that is what
 * makes the pair copy-on-write rather than a child protected and a parent writing straight
 * through to the shared frame -- and the only thing that normally gives the permission back
 * is the processor taking a write fault.
 *
 * Which does not happen here.  `syscall_user_range_ok_in()` walks the range first, on
 * purpose, and a page that is read-only *because it is about to be copied* is
 * indistinguishable in that walk from a page that is read-only because the program was not
 * allowed to write it.  So the check said no and the copy never happened.
 *
 * The symptom was not a crash and not a wrong answer, which is what made it take this long:
 * a process that had forked could no longer `read` into a buffer on its own stack, because
 * the kernel refused the address.  `fork` followed by a `wait` that reports a status through
 * a pointer -- the most ordinary thing a process does -- failed with EFAULT, and EFAULT is a
 * statement about the pointer, so the report pointed at the caller instead of at `fork`.
 *
 * Resolving it here rather than faulting is the same choice the rest of this file makes:
 * a walk that knows what it is looking at, rather than a fault the handler has to recognise
 * as one it caused on purpose.  A page that is read-only for any *other* reason is left
 * alone, and the check afterwards refuses it exactly as before.
 *
 * Returns nothing: whether the range is writable is still the range check's answer, and
 * this only removes the one reason that answer would be wrong. */
/* A ring-0 write into a user page needs the same preparation, and this is where the
 * reason stops being subtle.
 *
 * A `fork` leaves the frames it shares read-only in *both* address spaces, and a ring-0
 * store to a read-only user page is not stopped by the processor unless `CR4.WP` is set.
 * It is not -- QEMU refuses to have the bit set at all (see `CR4_WP` in
 * kernel/include/pagetables.h) -- so the store goes straight through into the *shared*
 * physical frame, updating it for the twin as well.  The victim is the kernel's own
 * syscall return: `syscall_dispatch()` writes the result into the program's frame, on the
 * program's stack, which `fork` has just taken write permission away from.  The parent
 * wrote its child's pid into the frame the child is about to read, so the child returned
 * from `fork()` holding a non-zero pid, took the parent's branch, and ran the program a
 * second time -- two copies, interleaved, writing over each other.
 *
 * Every kernel-side store into a user page goes through here for that reason.  There are
 * few enough of them to name: this function itself, `syscall_dispatch()`'s return value,
 * and signal delivery's patching of the frame. */
void syscall_prepare_write(uint64_t addr, size_t bytes) {
    task_t *t = sched_current();
    page_table_t *tables = vmm_tables();

    if (t == NULL || t->cow == NULL || t->cow->count == 0 || tables == NULL)
        return;

    if (!vmm_is_user_addr(addr) || (bytes != 0 && addr + bytes < addr))
        return;

    for (uint64_t off = 0; off < bytes; off += page_size) {
        pte_t leaf = pt_get(tables, addr + off);

        if (!pte_present(leaf) || !pte_user(leaf) || pte_writable(leaf))
            continue;

        /* Not ours to fix: the page is read-only for a reason of its own, and the range
         * check that follows will say so with the right error. */
        (void)vmm_cow_fault(tables, t->cow, addr + off);
    }
}

/* The write side, and it says the same thing for the same reason.
 *
 * `syscall_prepare_write()` first, because a page a `fork` left read-only is a page this
 * program is allowed to write and the walk cannot tell that apart from one it is not. */
bool syscall_copy_to_user(uint64_t dst, const void *src, size_t bytes) {
    if (bytes == 0)
        return true;

    syscall_prepare_write(dst, bytes);

    if (!syscall_user_range_ok(dst, bytes, true)) {
        kprintf("[warn] write of %#lx+%zu to user space refused: not mapped\n",
                (unsigned long)dst, bytes);
        return false;
    }

    __builtin_memcpy((void *)(uintptr_t)dst, src, bytes);
    return true;
}

/* Copy a NUL-terminated string out of user space.
 *
 * The three rules, and each of them is a bug that happened here:
 *
 *  - **Stop at the NUL, not at the buffer.**  A path is nine bytes and asking for 256
 *    mapped bytes of it is asking the program to have 256 mapped bytes.  A string that
 *    ends within the same page as its start must never require the page after it.
 *
 *  - **Never step past the end of the page the cursor is on.**  Even the bounded version
 *    reads in chunks, and a chunk that crosses a page boundary reads the next page whether
 *    the string needs it or not.  The chunk is capped at the page boundary, so a string
 *    anywhere on a mapped page is readable without its page's neighbour.
 *
 *  - **Check the range for the chunk, not for `max`.**  `syscall_copy_from_user` already
 *    checks every page the *chunk* touches, which is what makes the second rule work.
 *
 * This is what every path, and every `argv` and `envp` entry, is copied with.  A `sh` that
 * `exec`s something it was handed near the top of its own stack used to be refused by the
 * kernel, because the kernel insisted on 256 readable bytes.
 *
 * Returns the length, or -errno. */
long syscall_copy_cstring(uint64_t src, char *dst, size_t max) {
    if (max == 0)
        return -EFAULT;

    size_t got = 0;

    while (got + 1 < max) {
        /* The rest of the page the cursor is on, which is as much as can be read
         * without asking for a page the string may not need. */
        size_t page_left = page_size - (size_t)((src + got) & (uint64_t)(page_size - 1));
        size_t chunk = max - 1 - got;

        if (chunk > 64)
            chunk = 64;
        if (chunk > page_left)
            chunk = page_left;
        if (chunk == 0)
            break;

        if (!syscall_copy_from_user(dst + got, src + got, chunk))
            return -EFAULT;

        for (size_t i = 0; i < chunk; i++) {
            if (dst[got + i] == 0)
                return (long)(got + i);
        }

        got += chunk;
    }

    return -ENAMETOOLONG;
}

/* --- descriptors --------------------------------------------------------- */

long sys_fd_write(int fd, const void *buf, size_t bytes) {
    fd_entry_t *e = fd_get(fd);

    if (e == NULL)
        /* Not a descriptor this task has open.  EBADF rather than a silent success: a
         * program that believes it wrote somewhere is worse off than one that knows it
         * did not. */
        errno_return(EBADF);

    switch (e->kind) {
    case FD_CONSOLE:
        /* The framebuffer and the serial mirror, so a program's output is in a
         * screenshot and in a log.  Going through console_write() rather than putting
         * characters out one at a time keeps the two in step, which matters because the
         * serial mirror is what the automated tests read. */
        console_write((const char *)buf, bytes);
        return (long)bytes;

    case FD_PIPE:
        /* Writing a read end is a mistake in the program, not a condition to wait out.
         * Blocking here would wait for data that only this end is not allowed to consume
         * and that nobody else will write, so the program would hang instead of being
         * told. */
        if (e->pipe_end != PIPE_WRITE)
            errno_return(EBADF);

        return pipe_write((pipe_t *)e->obj, (uint64_t)(uintptr_t)buf, bytes);

    case FD_FILE:
        return fs_sys_write_fd(fd, buf, bytes);

    default:
        errno_return(EBADF);
    }
}

long sys_fd_read(int fd, void *buf, size_t bytes) {
    fd_entry_t *e = fd_get(fd);

    if (e == NULL)
        errno_return(EBADF);

    switch (e->kind) {
    case FD_PIPE:
        /* A pipe with something buffered returns what is there without waiting for the
         * full count: the writer has no way to know how much the reader wants, so a read
         * that insisted on all of it could deadlock against its own buffer. */
        if (e->pipe_end != PIPE_READ)
            errno_return(EBADF);

        return pipe_read((pipe_t *)e->obj, (uint64_t)(uintptr_t)buf, bytes);

    case FD_FILE:
        /* The file half of the dispatch, including a terminal: reading /dev/tty is a
         * `read` of a file descriptor, and the line discipline is what makes it a line at
         * a time rather than a keystroke at a time. */
        return fs_sys_read_fd(fd, (uint64_t)(uintptr_t)buf, bytes);

    case FD_CONSOLE:
        /* The console descriptor *is* the terminal, so reading it is a read through the
         * line discipline -- which is where every program on this machine gets its standard
         * input from.  `/sbin/init` opens `/dev/console` and `dup2`s it onto 0, 1 and 2, so
         * this case is not a special case: it is the shell's own input.
         *
         * It used to answer ENOSYS, on the grounds that "0, 1 and 2 belong to the process
         * table and name the screen, which has nothing to read".  The screen has nothing to
         * read; the terminal has.  With ENOSYS here, a shell at its prompt called
         * `read(0, &c, 1)`, got an error, and spun on it for ever: the keystrokes reached
         * the discipline's queue and nothing ever took them out.  A machine with a keyboard,
         * a terminal, a working line discipline and a shell on the console, in which typing
         * does nothing. */
        return tty_read((uint64_t)(uintptr_t)buf, bytes);

    default:
        errno_return(EBADF);
    }
}

long sys_write_to_fd(int fd, uint64_t base, uint64_t len) {
    if (len == 0)
        return 0;

    /* Checked here as well as in sys_fd_write() because the buffer comes from the
     * program's iovec rather than from its registers, and this is the first place the
     * two paths meet. */
    if (!syscall_user_range_ok(base, (size_t)len, false))
        errno_return(EFAULT);

    return sys_fd_write(fd, (const void *)(uintptr_t)base, (size_t)len);
}

/* --- the calls ----------------------------------------------------------- */

long sys_write(regs_t *regs) {
    int fd = (int)(int64_t)arg(regs, 0);
    uint64_t buf = arg(regs, 1);
    size_t count = (size_t)arg(regs, 2);

    if (count == 0)
        return 0;

    /* The range is checked as a whole, so a program that hands over one unmapped byte
     * in the middle of a buffer gets nothing at all rather than a partial write whose
     * length depends on where the hole was.  Whole-buffer semantics are what every
     * program expects from `write` to a terminal, and a short write for a reason the
     * program cannot see is worse than a clear error. */
    if (!syscall_user_range_ok(buf, count, false))
        errno_return(EFAULT);

    return sys_fd_write(fd, (const void *)(uintptr_t)buf, count);
}

long sys_read(regs_t *regs) {
    int fd = (int)(int64_t)arg(regs, 0);
    uint64_t buf = arg(regs, 1);
    size_t count = (size_t)arg(regs, 2);

    if (count == 0)
        return 0;

    /* The whole range is checked before anything is copied, for the same reason `write`
     * checks it: one unmapped byte in the middle must not produce a partial read whose
     * length depends on where the hole was.
     *
     * The destination is a *write* into this process's own address space, so any page a
     * fork has left read-only for copying has to be resolved first -- see
     * syscall_prepare_write().  Without it every read a process makes after a fork fails
     * with EFAULT on a perfectly good buffer, which is what this did. */
    syscall_prepare_write(buf, count);

    if (!syscall_user_range_ok(buf, count, true))
        errno_return(EFAULT);

    return sys_fd_read(fd, (void *)(uintptr_t)buf, count);
}

long sys_pipe(regs_t *regs) {
    return pipe_create(arg(regs, 0));
}

/* `exec(path, argv, envp)`.
 *
 * The calling task's address space and stack are replaced by a new program's.  `argv` and
 * `envp` are arrays of pointers into the *old* address space, and the new program's stack is
 * built before the old one is unmapped -- reading them after would be reading a page table
 * that is no longer there.
 *
 * `envp` is accepted and ignored for now, because there is nothing to put in it: the
 * registry knows a program's name and its image and nothing else, and phase 8 brings the
 * environment with the filesystem.  Saying so is better than quietly handing the new
 * program an empty one and letting it wonder.
 */
/* How many entries a user vector has, read one pointer at a time and bounded.
 *
 * Counted rather than trusted because a user vector has no length: the only terminator is
 * a NULL, and a caller that forgot one would otherwise have the kernel walk until it hit
 * an unmapped page and take a fault on the caller's behalf -- turning a program's mistake
 * into a kernel oops.  `MAX_EXEC_VECS` is the ceiling; a program with more arguments than
 * that gets them truncated and a message, which is a usable answer where a fault is not. */
#define MAX_EXEC_VECS 256

/* One processor's `exec` scratch: the argument and environment strings, copied out of the
 * calling program and validated before the loader builds the new stack around them.  Sized
 * so that a program with the maximum number of arguments and a generous 32 bytes each still
 * fits, which is far more than anything the shell builds. */
typedef struct {
    char  strs[8192];
    char *argv[MAX_EXEC_VECS + 1];
    char *envp[MAX_EXEC_VECS + 1];
    size_t used;
} exec_arena_t;

static int copy_user_vector(uint64_t from, const char **into, int max) {
    int n = 0;

    if (from == 0)
        return 0;

    while (n < max) {
        uint64_t ptr = 0;

        if (!syscall_copy_from_user(&ptr, from + (uint64_t)n * sizeof(uint64_t),
                                    sizeof(ptr)))
            return -1;

        if (ptr == 0)
            break;

        if (!syscall_user_range_ok(ptr, 1, false))
            return -1;

        into[n] = (const char *)(uintptr_t)ptr;
        n++;
    }

    return n;
}

/* Everything `exec` does, with the name and the vectors already in the kernel's hands.
 *
 * Declared before its own definition because it calls itself: the `#!` path re-enters it
 * with the interpreter.
 *
 * Split out from `sys_exec()` because the `#!` path calls it *again*, with an interpreter
 * and a rebuilt argument vector, and a second copy of the load-and-replace half of exec
 * would be a second thing to get wrong.  The split is also where the recursion is bounded:
 * an interpreter that is itself a script goes round once more, and a script whose
 * interpreter is a script whose interpreter is... runs out of kernel stack, which is the
 * honest place for that to end.
 *
 * `name` is writable because the `#!` path builds the interpreter's name in it. */
static long do_exec(regs_t *regs, char *name,
                    const char *const *exec_argv, const char *const *exec_envp)
{
    (void)arg(regs, 0);
    (void)arg(regs, 1);
    (void)arg(regs, 2);

    /* Two sources of bytes, and which one is used is decided by the *name*, not by a flag:
     *
     *   - a path with a slash is a file.  `exec("/bin/sh")`, `exec("./build")` and
     *     `exec("../a.out")` are all files, and reading them from the filesystem is the
     *     only thing that makes them work.
     *   - a bare name is a program the kernel knows by name -- the registry -- which is
     *     what `hello`, `forktest` and every other self-test program is started as.  The
     *     registry is still here because there is no filesystem path for a program the
     *     kernel carries inside itself.
     *
     * Deciding by the name rather than by a flag is what lets `exec("hello")` and
     * `exec("/hello")` both do the obvious thing without a program having to say which. */
    const program_image_t *reg = strchr(name, '/') != NULL ? NULL : program_find(name);
    void *image = NULL;
    size_t bytes = 0;
    uint32_t stack_pages = 8;
    const char *exec_name = name;

    if (reg != NULL) {
        image = (void *)(uintptr_t)reg->image;
        bytes = reg->bytes;
        stack_pages = reg->stack_pages;
        exec_name = reg->name;
    } else {
        vpath_t p;
        long r = vfs_resolve(sched_current(), name, true, &p);

        if (r != 0) {
            kprintf("exec: %s: %ld\n", name, r);
            errno_return(r == -ENOENT ? ENOENT : -ENOENT);
        }

        if (ramfs_node_kind(p.ino) != RAMFS_FILE)
            errno_return(EACCES);

        /* The `n` of an EACCES is not an errno and the error is about the file, so it is
         * said here rather than returned as a number nobody can read. */
        uint32_t mode = ramfs_node_mode(p.ino);

        if ((mode & 0111) == 0 && sched_current() != NULL &&
            sched_current()->uid != 0) {
            kprintf("exec: %s is not executable\n", name);
            errno_return(EACCES);
        }

        r = vfs_read_whole(&p, &image, &bytes);

        if (r != 0) {
            kprintf("exec: %s could not be read (%ld)\n", name, r);
            errno_return(ENOEXEC);
        }

        /* A script: `#!` on the first line.
         *
         * The kernel does not *run* a script -- it runs the interpreter the script names,
         * with the script's path in the interpreter's arguments.  That is the whole trick,
         * and doing it here rather than in every program is what makes `#!/bin/sh` work for
         * a program that has never heard of shell scripts.
         *
         * The line is parsed the way every Unix parses it, which is not quite the way a
         * person reads it:
         *
         *   `#!`, optional spaces, the interpreter's path up to the next space or the end
         *   of the line, then everything to the end of the line as *one* argument.
         *
         * "As one argument" is the part that surprises people and the part that matters.
         * The argument is an operand, not shell input: `#!/bin/sh -e -x` has to reach
         * `/bin/sh` as three arguments, and word-splitting it here would also make a
         * script path with a space in it unnameable.
         *
         * The line is bounded rather than NUL-terminated: the image is whatever the
         * filesystem gave, and reading past the first line would read past the end of the
         * buffer for a file whose entire content is `#!`. */
        if (bytes >= 2 && ((const char *)image)[0] == '#' &&
            ((const char *)image)[1] == '!') {
            const char *line = (const char *)image + 2;
            size_t      n    = bytes - 2;
            size_t      i    = 0;

            while (i < n && (line[i] == ' ' || line[i] == '\t'))
                i++;

            size_t ipath = i;

            while (i < n && line[i] != ' ' && line[i] != '\t')
                i++;

            size_t ilen = i - ipath;

            while (i < n && (line[i] == ' ' || line[i] == '\t'))
                i++;

            size_t arg_at = i;

            while (i < n && line[i] != '\n' && line[i] != '\r')
                i++;

            size_t alen = i - arg_at;

            /* Trailing spaces are not part of the argument.  Leading ones were already
             * skipped, and a trailing space is what a file with CRLF endings turns into --
             * which is a script somebody wrote on another machine, and it should run. */
            while (alen > 0 && (line[arg_at + alen - 1] == ' ' ||
                                line[arg_at + alen - 1] == '\t'))
                alen--;

            /* Declared here rather than where it is filled in, because the check that
             * bounds it comes first.  And `sizeof(interp)`, never `sizeof(name)`: `name` is
             * a pointer parameter -- the recursion below passes a different buffer -- so
             * `sizeof` of it is eight, and a bound of eight refuses every interpreter whose
             * path is longer than that, which is all of them. */
            static char interp[128];
            static char script_arg[VFS_NAME_MAX];
            static char one[256];

            /* A `#!` line naming nothing.  ENOEXEC rather than a message alone: the file
             * *is* executable and the kernel could not run it, which is exactly what
             * ENOEXEC is for. */
            if (ilen == 0 || ilen >= sizeof(interp)) {
                ramfs_image_free(image);
                kprintf("exec: %s has a #! line that names no interpreter\n", name);
                errno_return(ENOEXEC);
            }

            memcpy(interp, line + ipath, ilen);
            interp[ilen] = 0;

            /* The script's own name, kept because `name` is the interpreter's buffer by
             * the time the vector is used, and because a script that refers to things
             * beside itself has to find them the way its author wrote them: resolving the
             * path here would change what `$0` says. */
            memcpy(script_arg, name, strlen(name) + 1);

            /* The new vector, in POSIX order: the interpreter, its optional argument, the
             * script, and then whatever the caller had after the script's name.  So
             * `execve("./s.sh", ["./s.sh", "a"], env)` of a `#!/bin/sh -x` line reaches
             * `/bin/sh` as `["/bin/sh", "-x", "./s.sh", "a"]` -- which is what a shell
             * would have built by hand, and the reason a script does not need to know
             * anything about being a script. */
            const char *newv[MAX_EXEC_VECS + 4];
            int   nn = 0;
            int   have = 0;

            while (have < MAX_EXEC_VECS && exec_argv[have + 1] != NULL)
                have++;

            if (have + 3 > MAX_EXEC_VECS + 1) {
                ramfs_image_free(image);
                kprintf("exec: %s is a script and there is no room for its interpreter\n",
                        script_arg);
                errno_return(E2BIG);
            }

            newv[nn++] = interp;

            if (alen > 0) {
                if (alen >= sizeof(one)) {
                    ramfs_image_free(image);
                    kprintf("exec: %s has a #! argument too long to pass\n", script_arg);
                    errno_return(ENOEXEC);
                }

                memcpy(one, line + arg_at, alen);
                one[alen] = 0;
                newv[nn++] = one;
            }

            newv[nn++] = script_arg;

            for (int k = 1; k <= have; k++)
                newv[nn++] = exec_argv[k];

            newv[nn] = NULL;

            ramfs_image_free(image);

            /* Once more, with the interpreter.  The recursion is bounded by the stack and
             * not by this code, which is deliberate: a chain of scripts each naming the
             * next is a real thing to want, and truncating it would be a rule nobody asked
             * for. */
            return do_exec(regs, interp, newv, exec_envp);
        }
    }

    task_t *t = sched_current();

    uint64_t entry = 0;
    uint64_t entry_rsp = 0;

    if (program_exec_into(t, exec_name, image, bytes, stack_pages,
                          exec_argv, exec_envp, &entry, &entry_rsp) == NULL) {
        if (reg == NULL)
            ramfs_image_free(image);
        errno_return(ENOEXEC);
    }

    if (reg == NULL)
        ramfs_image_free(image);

    /* Into the frame this call arrived on, which is the one the tail's `iretq` is about to
     * use.  Everything else in it is left alone: the selector and flag words are the same
     * for any program, and the nine general registers are restored from the ring-3 save the
     * same way they were for the old program. */
    regs->rip = entry;
    regs->rsp = entry_rsp;

    /* A successful `exec` never returns: the caller's frame has just been unmapped, so
     * there is nothing to return to.  The loader has pointed the task at the new program
     * and it resumes there the next time it is switched into -- so the return value here
     * is never observed, and is only reached on the failure paths above. */
    return 0;
    return 0;
}

/* `exec(path, argv, envp)`, with everything the caller's address space holds copied out
 * before anything is replaced.
 *
 * Every string is copied rather than pointed at, for the reason the name is: this call
 * replaces the address space, and a vector that still points into it is a vector of
 * addresses that no longer mean anything.  The arena is static rather than allocated,
 * which is what bounds the argument space: a program with more than 8 KiB of arguments
 * gets the first 8 KiB and a message on the console, and a program with more than
 * `MAX_EXEC_VECS` entries gets that many.  Neither is a crash, and both are answers a
 * shell can report.
 *
 *
 * **One per processor, and not one for the machine.**
 *
 * It was one `static` array, on the argument that this is not reentrant "and does not need
 * to be": `exec` replaces the program, so there is no second `exec` in flight from the same
 * task.  That is true and it is the wrong question.  A *pipeline* has two tasks execing at
 * the same moment on two processors, and they were writing at the same `used` offset and
 * handing each other the pointers they had just stored.
 *
 * The symptom is the shell's `ls /tmp/shtest-* | head -n 1` passing `-n` to `ls` about one
 * run in four.  `ls` was handed the *other* half's arguments -- `head -n 1` -- while the
 * path, which is a local and so was never shared, still said `/bin/ls`, and `ls` reported
 * "unknown option" for a flag it had never been given.  Nothing faulted: the pointers were
 * valid, they were just valid for somebody else, and both processes finished normally.
 *
 * Per processor is enough and needs no lock, because a task is on exactly one processor at
 * a time and `syscall_dispatch()` holds interrupts off for the whole of the handler, so
 * there is never more than one `exec` in flight on a given CPU.  It is also the answer that
 * scales: a machine with 64 processors runs 64 `exec`s at once and none of them wait. */
static exec_arena_t exec_arenas[MAX_CPU_COUNT];

long sys_exec(regs_t *regs) {
    uint64_t path = arg(regs, 0);
    uint64_t argv = arg(regs, 1);
    uint64_t envp = arg(regs, 2);

    if (!syscall_user_range_ok(path, 1, false))
        errno_return(EFAULT);

    char name[VFS_NAME_MAX];

    /* A string, not a block.
     *
     * This used to copy `VFS_NAME_MAX` bytes -- 256 of them -- and require all 256 to be
     * readable, on the grounds that "the whole block, or nothing".  That is a rule about
     * the *buffer* being all-or-nothing, which is true, and a rule about the *program's*
     * memory being all-or-nothing, which is not: a program whose path string ends 30 bytes
     * from the top of its own stack has a perfectly good path and no page above it.  Every
     * `sh` that `exec`s something it was handed near the end of its stack was refused.
     *
     * `syscall_copy_cstring()` stops at the NUL and never reads past the end of the page
     * the cursor is on, which is what a path is allowed to assume. */
    long nlen = syscall_copy_cstring(path, name, sizeof(name));

    if (nlen < 0) {
        /* -ENAMETOOLONG is the one answer here that is not about the pointer, and it keeps
         * its own number; the rest are the program's pointer being unusable. */
        errno_return(nlen == -ENAMETOOLONG ? ENAMETOOLONG : EFAULT);
    }

    exec_arena_t *a = &exec_arenas[percpu_self_index()];

    /* Copy one string into the arena, or say that there was no room.  Returning false is
     * not an error for the caller: a program whose arguments do not fit gets the ones that
     * do, which is what every Unix does when `ARG_MAX` is exceeded. */
    #define COPY_ONE(dst_index, src)                                              \
        do {                                                                      \
            size_t len = 0;                                                       \
                                                                                  \
            while (a->used + len + 1 < sizeof(a->strs) && len < 4096) {           \
                char ch;                                                          \
                                                                                  \
                if (!syscall_copy_from_user(&ch, (src) + len, 1))                 \
                    return -EFAULT;                                               \
                                                                                  \
                if (ch == 0)                                                      \
                    break;                                                        \
                                                                                  \
                a->strs[a->used + len] = ch;                                      \
                len++;                                                            \
            }                                                                     \
                                                                                  \
            a->strs[a->used + len] = 0;                                           \
            (dst_index) = a->strs + a->used;                                      \
            a->used += len + 1;                                                    \
        } while (0)

    {
        const char *raw[MAX_EXEC_VECS];
        int n = copy_user_vector(argv, raw, MAX_EXEC_VECS);

        if (n < 0)
            return -EFAULT;

        for (int i = 0; i < n; i++) {
            if (a->used >= sizeof(a->strs) - 1) {
                kprintf("exec: %s has arguments past %zu bytes; truncating\n",
                        name, sizeof(a->strs));
                break;
            }

            COPY_ONE(a->argv[i], (uint64_t)(uintptr_t)raw[i]);
        }

        a->argv[n] = NULL;
    }

    {
        const char *raw[MAX_EXEC_VECS];
        int n = copy_user_vector(envp, raw, MAX_EXEC_VECS);

        if (n < 0)
            return -EFAULT;

        for (int i = 0; i < n; i++) {
            if (a->used >= sizeof(a->strs) - 1)
                break;

            COPY_ONE(a->envp[i], (uint64_t)(uintptr_t)raw[i]);
        }

        a->envp[n] = NULL;
    }

    #undef COPY_ONE

    /* The arena is `char *[]` because that is what `COPY_ONE` fills in, and `do_exec`
     * promises not to write through either vector.  The cast says that once, here, rather
     * than making the whole path copy strings twice. */
    return do_exec(regs, name, (const char *const *)a->argv,
                   (const char *const *)a->envp);
}

long sys_open(regs_t *regs) {
    (void)regs;
    errno_return(ENOSYS);
}

long sys_close(regs_t *regs) {
    int fd = (int)(int64_t)arg(regs, 0);

    /* EBADF for a number that is not open, and for one of 0, 1 and 2: the program did
     * not have it to begin with, and the console belongs to the process table.  Linux
     * returns success for the second case and that is a question of taste, not one this
     * kernel gets to be quietly different about on the way to a shell that will use it. */
    if (!fd_close(fd))
        errno_return(EBADF);

    return 0;
}

long sys_writev(regs_t *regs) {
    int fd = (int)(int64_t)arg(regs, 0);
    uint64_t iov = arg(regs, 1);
    uint64_t count = arg(regs, 2);

    if (count > IOV_MAX)
        errno_return(EINVAL);

    if (count == 0)
        return 0;

    /* The whole vector array is copied in one go, so a program cannot hand over a list
     * whose fourth entry is on a different page from the first three and have the
     * kernel discover that halfway through. */
    size_t header_bytes = (size_t)count * sizeof(struct iovec);
    uint8_t header[sizeof(struct iovec) * IOV_MAX];

    if (!syscall_copy_from_user(header, iov, header_bytes))
        errno_return(EFAULT);

    const struct iovec *vec = (const struct iovec *)header;
    size_t total = 0;

    for (uint64_t i = 0; i < count; i++) {
        long wrote = sys_write_to_fd(fd, vec[i].base, vec[i].len);

        if (wrote < 0)
            return total > 0 ? (long)total : wrote;

        total += (size_t)wrote;
    }

    return (long)total;
}

long sys_exit(regs_t *regs) {
    int code = (int)(int32_t)arg(regs, 0);
    task_t *t = sched_current();

    syscall_note_exit(code);

    /* Records the status and publishes it *before* the flag that says it is there, then
     * wakes the parent -- which is what makes `wait` return at all.
     *
     * A parent blocked in `wait` is off the run queue with nothing to run it; nothing
     * would ever bring it back, and the process tree would stop at the first child.  The
     * wake has to happen while this task is still alive and still on a run queue, because
     * `task_wakeup()` publishes to *another* processor's queue and that processor may
     * reach the parent immediately. */
    if (t != NULL && t->parent != NULL)
        task_unblock(t->parent);

    /* Does not return, and nothing may touch this frame afterwards: the task's stack is
     * about to be abandoned, and the handler it was called from is running on it. */
    task_exit_with(code);

    for (;;)
        hlt();
}

long sys_brk(regs_t *regs) {
    return sys_brk_grow(regs);
}

long sys_getpid(regs_t *regs) {
    (void)regs;
    task_t *t = sched_current();

    return t != NULL ? (long)t->id : 0;
}

/* --- dispatch ------------------------------------------------------------ */

void syscall_dispatch(regs_t *regs) {
    unsigned long nr = regs->rax;
    syscall_fn_t fn = nr < SYS_MAX ? handlers[nr] : NULL;

    if (fn == NULL) {
        /* An unknown call number is the program's mistake, not a fault, and it gets an
         * error and carries on.  Panicking would let any program with a bug in it take
         * the machine down, which is the wrong direction for the boundary to fail in. */
        syscall_note_unknown(nr);
        syscall_set_errno(ENOSYS);
        regs->rax = (uint64_t)(int64_t)-ENOSYS;
        return;
    }

    /* Interrupts off across the handler.
     *
     * A handler runs on the *task's* kernel stack -- the TSS's RSP0, because `int
     * $0x80` from ring 3 switched it -- and that stack is shared with everything else
     * the task does.  A tick in the middle of a `write` would land on it, and the
     * scheduler would switch away with the handler's frame half-built.  So interrupts
     * are off, and a handler that has to wait for something has to say so by blocking
     * the *task*, which is what the wait queues are for. */
    uint64_t flags = irq_save();

    /* Before the result is written, not after: the frame is the program's own stack, and a
     * `fork` may have taken write permission away from it a moment ago.
     *
     * The frame is built as a copy of the one the syscall was entered with, so for every
     * syscall after a `fork` this page is one of the pages the parent and the child share.
     * Writing `rax` into it from ring 0 goes into the *shared* frame -- the processor does
     * not stop a supervisor store to a read-only user page without `CR4.WP`, which this
     * kernel cannot have (see `CR4_WP`) -- so the child read back the value the parent had
     * just written there.  For `fork` that value is the child's own pid: the child did not
     * get a zero, so it did not know it was a child, and it ran the parent's half of the
     * program beside a copy of itself.
     *
     * One page walk per syscall, and it returns immediately for a task with nothing shared,
     * which is every task that has never forked. */
    syscall_prepare_write((uint64_t)(uintptr_t)regs, sizeof(*regs));

    regs->rax = (uint64_t)fn(regs);

    irq_restore(flags);

    /* A handler that returned a kernel address would hand the program a pointer into
     * the kernel.  Nothing should, but this is the last place it can be caught and the
     * check is two instructions.
     *
     * The comparison has to be *signed*.  A handler reports an error as a negative value,
     * and the frame holds registers unsigned, so -9 is 0xfffffffffffffff7: it is greater
     * than zero, it is greater than USER_SPACE_END, and this turned every error into
     * EFAULT.  The program could not tell a bad descriptor from an unmapped buffer -- both
     * came back as -14 -- which is exactly the distinction the boundary exists to make,
     * and it was invisible because every test only asked for "some error". */
    if ((int64_t)regs->rax > 0 && (uint64_t)regs->rax >= USER_SPACE_END) {
        syscall_set_errno(EFAULT);
        regs->rax = (uint64_t)(int64_t)-EFAULT;
    }

}

/* Load `image` into a brand new user task and queue it to run.
 *
 * This is the whole of "run a program": an address space, a stack, the segments, the
 * initial stack frame, and onto a run queue.  Everything else -- which terminal, what
 * environment, what arguments -- arrives with the filesystem in phase 8, and this is
 * deliberately the part that will not have to change. */
task_t *program_exec_into(task_t *t, const char *name, const void *image,
                          size_t bytes, uint32_t stack_pages,
                          const char *const *argv, const char *const *envp,
                          uint64_t *entry_out, uint64_t *rsp_out) {
    if (t == NULL || t->tables == NULL) {
        kwarn("exec: %s has no address space to replace", name != NULL ? name : "?");
        return NULL;
    }

    /* Built in a scratch task and only swapped in once it is complete.
     *
     * Doing it in place is the obvious thing and it is wrong: the load allocates frames,
     * maps segments and builds a stack, and any of those can fail.  A program that fails
     * half way through an in-place `exec` has no program, no stack, and no way to report
     * why -- it is a corpse that used to be a process.  Here, a failure leaves the old
     * program running and the error reaching the program that asked, which is the only
     * behaviour that leaves anything useful behind. */
    task_t *fresh = task_create_user(name, 0, stack_pages, TASK_WEIGHT_DEFAULT);

    if (fresh == NULL) {
        kwarn("exec: could not build %s", name);
        return NULL;
    }

    elf_load_result_t r = elf_load(image, bytes, fresh, stack_pages,
                                   USER_IMAGE_BASE + sizeof(Elf64_Ehdr),
                                   argv, envp);

    if (!r.ok || fresh->user_frame == NULL ||
        !vmm_translate_in(fresh->tables, r.entry) ||
        !syscall_user_range_ok_in(fresh->tables, r.entry, 1, false)) {
        kwarn("exec: %s would not load", name);
        vmm_address_space_destroy(fresh->tables);
        kfree(fresh->tables);
        fresh->tables = NULL;
        kfree(fresh->stack);
        fresh->stack = NULL;
        return NULL;
    }

    uint64_t *frame = fresh->user_frame;

    frame[0] = r.entry;
    frame[3] = r.entry_rsp;
    frame[4] = GDT_USER_DATA_RPL3;
    fresh->saved_rsp = USER_STACK_TOP - USER_FRAME_BYTES - USER_CTX_BYTES;
    fresh->started = false;

    /* Swap.
     *
     * The task keeps its own *kernel* stack: this call is running on it, the scheduler
     * resumes the task on it, and an exception taken the instant after this returns would
     * land on it.  What goes is the scratch task's -- it was only ever a place to build the
     * new program before there was anywhere to put it -- and the old address space.
     *
     * The old user frame is a kernel alias into the old stack's page, which stops being
     * mapped the moment the old tables are destroyed, so nothing may read `t->user_frame`
     * after the swap either. */
    page_table_t *old_tables = t->tables;
    void *scratch_stack = fresh->stack;

    /* The new program's signals start clean: caught signals go back to their default
     * disposition and anything pending is dropped, while the *blocked* set is kept.  That
     * asymmetry is deliberate and is POSIX's.  A new program must not run the old one's
     * handler -- the handler's address is a user address in an address space that no longer
     * exists, so leaving one installed would be a #PF on the first delivery to it.  The
     * blocked set, on the other hand, is the process's own choice about what it wants to
     * see, and `exec` is not a signal that anything has gone wrong. */
    memset(t->sig_slots, 0, sizeof(t->sig_slots));
    t->sig_pending = 0;
    t->sig_active = 0;
    t->sig_stopped = 0;

    t->tables = fresh->tables;
    t->user_frame = fresh->user_frame;
    t->user_stack_phys = fresh->user_stack_phys;
    t->user_stack_pages = fresh->user_stack_pages;
    t->saved_rsp = fresh->saved_rsp;
    t->started = false;
    t->cow = NULL;             /* the old pages are gone; the list described them */

    /* The program break, which `build_stack()` set on the scratch task and which every
     * `sbrk` asks about.  It was missing here, and it is the kind of field whose absence
     * cannot be seen: `t->brk` kept whatever the *old* program left, which for a process
     * that the kernel started is zero, so `sbrk(0)` answered zero, the allocator took that
     * as the base of its heap, and handed out memory starting at address 0 and upwards --
     * straight through this program's own image.  The first `strdup()` wrote its string
     * over a hundred kilobytes of `.bss`, and a shell's variable table came back holding
     * pointers into itself.
     *
     * The symptom is nowhere near the cause.  Nothing failed, nothing was reported, and the
     * program that did it kept running: it was a later, unrelated lookup that walked into
     * the overwritten table.  A program started by the *kernel* was fine, because there
     * `elf_load()` runs on the task itself and sets the break directly -- which is why
     * every test in the tree passed and `sh`, which init starts with `exec`, did not. */
    t->brk = fresh->brk;
    t->brk_start = fresh->brk_start;

    fresh->tables = NULL;
    fresh->stack = NULL;
    kfree(scratch_stack);

    /* Off the all-tasks list and out of `/proc` before the memory goes.
     *
     * `task_create_user()` registered `fresh` -- it is a task like any other as far as
     * `task_create()` is concerned -- so freeing it without taking it off leaves the list
     * pointing at freed memory.  Every walk of that list then follows the dangling entry:
     * `ps` prints whatever is there now, `task_by_pid()` hands out a freed pointer, and
     * `/proc/<pid>/status` reads a `parent` field out of a recycled block.  That last one
     * is how this was found, as a page fault inside procfs reading a task that had been
     * freed an `exec` earlier. */
    task_unregister(fresh);
    kfree(fresh);

    /* Load the new address space *before* returning, because this call is going to return
     * into the old one.
     *
     * The task is running: the CPU's CR3 still names the address space that has just been
     * swapped out from under it, and the tail's `iretq` hands control to the new program in
     * whatever CR3 happens to say.  Returning first means the new program's first
     * instruction is fetched through the old program' page tables -- so it faults on an
     * address the loader mapped and reported mapped, which is a page fault that accuses the
     * loader of something it did not do.
     *
     * Safe to do here, mid-call, because the kernel half is the same in both: the task's
     * kernel stack, the direct map and everything else this call is about to run on are
     * mapped in the new address space exactly as in the old one.  Only the user half
     * differs, and nothing on the way out of here touches it. */
    vmm_address_space_switch(t->tables);

    /* Only now, with CR3 naming the new tables, is it safe to destroy the old ones. */
    vmm_address_space_destroy(old_tables);
    kfree(old_tables);

    if (entry_out != NULL)
        *entry_out = r.entry;
    if (rsp_out != NULL)
        *rsp_out = r.entry_rsp;

    kprintf("exec: %s is now pid %lu, entry %#lx\n", name, (unsigned long)t->id,
            (unsigned long)r.entry);
    return t;
}

/* Start a program named by a *path*, reading its bytes out of the filesystem.
 *
 * The counterpart to `program_load_and_run()`, which starts a program the kernel carries
 * inside itself.  Both end in the same `program_start_image()`, because loading an image
 * into a task is the same work either way and the only thing that differs is where the
 * bytes came from.
 *
 * Returns NULL with the reason on the console, which is what a boot log wants: a machine
 * that cannot find its init says so rather than reaching a prompt with nothing behind it. */
task_t *program_start_path(const char *path, uint32_t stack_pages) {
    vpath_t p;
    long r = vfs_resolve(sched_current(), path, true, &p);

    if (r != 0) {
        kwarn("program: %s does not resolve (%ld)", path, r);
        return NULL;
    }

    if (ramfs_node_kind(p.ino) != RAMFS_FILE) {
        kwarn("program: %s is not a regular file", path);
        return NULL;
    }

    void *image = NULL;
    size_t bytes = 0;

    if (vfs_read_whole(&p, &image, &bytes) != 0) {
        kwarn("program: %s could not be read", path);
        return NULL;
    }

    task_t *t = program_start_image(path, image, bytes, stack_pages);

    if (t != NULL)
        ramfs_image_free(image);    /* the task has its own copy in its own pages now */

    return t;
}

task_t *program_load_and_run(const char *name, uint32_t stack_pages) {
    const program_image_t *p = program_find(name);

    if (p == NULL) {
        kwarn("program: no program called %s", name != NULL ? name : "(null)");
        return NULL;
    }

    return program_start(p);
}

/* Create a task and load one image into it.  Split out from program_start() so that a
 * program named by a path and a program the kernel carries both end here: loading an image
 * into a task is the same work either way and the only thing that differs is where the
 * bytes came from. */
task_t *program_start_image(const char *name, const void *image, size_t bytes,
                            uint32_t stack_pages) {
    task_t *t = task_create_user(name, 0, stack_pages, TASK_WEIGHT_DEFAULT);

    if (t == NULL) {
        kwarn("program: could not create a task for %s", name);
        return NULL;
    }

    /* Where the program headers are expected: inside the first PT_LOAD, at the offset
     * the ELF header says.  The caller passes the file's base and the loader works out
     * the rest, because the link address and the file offset can differ. */
    elf_load_result_t r = elf_load(image, bytes, t, stack_pages,
                                   USER_IMAGE_BASE + sizeof(Elf64_Ehdr),
                                   NULL, NULL);

    if (!r.ok) {
        kwarn("program: %s would not load", name);
        vmm_address_space_destroy(t->tables);
        kfree(t->tables);
        t->tables = NULL;
        return NULL;
    }

    /* The entry and stack pointer the loader worked out have to go into the ring-3
     * frame that task_create_user() built a placeholder in.  The frame lives at the top
     * of the task's own stack, which the kernel can reach through the direct map. */
    /* `iretq` pops RIP, CS, RFLAGS, RSP and SS -- in that order, each one quadword above
     * the last -- so RIP is the first quadword of the frame and SS the fifth.  RSP is
     * *fourth*, not fifth: the two were the wrong way round here, and reading a stack
     * pointer where a selector belongs is a #GP whose error code is the low half of that
     * stack pointer.  task_create_user() lays the frame out in the order the CPU pops it
     * and records a kernel alias of the frame's page for exactly this.
     *
     * The alias cannot be recomputed here.  The page is mapped only in this task's
     * address space, so phys_to_virt() of the user address is arithmetic on a user
     * address: it wraps, and adding the offset back lands nowhere. */
    if (t->user_frame == NULL) {
        kwarn("program: %s has no ring-3 frame to patch", name);
        vmm_address_space_destroy(t->tables);
        kfree(t->tables);
        t->tables = NULL;
        return NULL;
    }
    uint64_t *frame = t->user_frame;

    frame[0] = r.entry;            /* RIP */
    frame[3] = r.entry_rsp;        /* RSP */
    frame[4] = GDT_USER_DATA_RPL3; /* SS   */

    /* A virtual address: this is what the CPU loads into RSP. */
    t->saved_rsp = USER_STACK_TOP - USER_FRAME_BYTES - USER_CTX_BYTES;
    t->started = false;

    /* The entry point has to be mapped in *this task's* address space, and this has to be
     * asked before the task is queued rather than after.  Asked afterwards it is too late
     * to report: the task has been scheduled, it has run, and it has already faulted on
     * this very address, so the check reports a consequence of the thing it should have
     * prevented -- and by then the machine may not be in a state to print in.
     *
     * Without the check, a loader that maps nothing reports success and the program takes
     * a page fault on its first instruction, with the loader nowhere in the report. */
    if (!vmm_translate_in(t->tables, r.entry) ||
        !syscall_user_range_ok_in(t->tables, r.entry, 1, false)) {
        kwarn("program: %s loaded, but its entry point %#lx is not mapped user-readable "
              "in the task's own address space", name, (unsigned long)r.entry);
        vmm_address_space_destroy(t->tables);
        kfree(t->tables);
        t->tables = NULL;
        return NULL;
    }

    /* Report before queueing, not after.
     *
     * Waking the task makes it runnable, and on this machine the next tick can hand it the
     * processor while the report below is still being rendered.  The two then write to the
     * console a character at a time -- the program's own output interleaved with this
     * line, which reads as though the program had printed something strange:
     * `argc=1` came out as `argc=g1r` because "program:" was being emitted into the
     * middle of it.  Nothing was actually wrong with either line, and that is exactly why
     * it is worth fixing: a report that cannot be read is a report nobody has. */
    kprintf("program: %s loaded, entry %#lx, %u segment(s), %lu bytes, stack %#lx\n",
            name, (unsigned long)r.entry, r.segments, (unsigned long)r.bytes,
            (unsigned long)r.entry_rsp);

    if (task_wakeup(t) != 0) {
        kwarn("program: could not queue %s", name);
        return NULL;
    }

    return t;
}

/* Create a task and load one registered program into it. */
task_t *program_start(const struct program_image *p) {
    return program_start_image(p->name, p->image, p->bytes, p->stack_pages);
}

/* --- fork and wait ------------------------------------------------------- */

/* `fork`: a new task, continuing this one where this one is, over an address space that
 * shares every read-only page and copies every writable one on first use.
 *
 * The child is not loaded and not started: it is a copy of the frame this call arrived
 * with, with `%rax` changed to zero, and an address space built by `vmm_fork_aspace()`.
 * That is why a forked child needs no entry stack and no ELF loader -- it is already
 * running, partway through an instruction stream its parent is in the middle of. */
long sys_fork(regs_t *regs) {
    task_t *parent = sched_current();

    if (parent == NULL || parent->tables == NULL) {
        /* A kernel task has no user address space to twin, and a fork that silently
         * produced something else would be worse than one that refuses. */
        errno_return(EPERM);
    }

    /* Refuse once there is nothing left to give: a forked task's kernel stack and its
     * task_t are 16 KiB plus change, and a program in a loop would otherwise take the heap
     * to zero and fault somewhere unrelated. */
    if (task_count() >= TASK_LIMIT) {
        errno_return(EAGAIN);
    }

    task_t *child = task_create_forked("fork", regs, parent->weight);

    if (child == NULL) {
        errno_return(ENOMEM);
    }

    if (task_attach_cow_twin(child) != 0) {
        errno_return(ENOMEM);
    }

    if (task_wakeup(child) != 0) {
        errno_return(ENOMEM);
    }


    return (long)child->id;
}

/* `wait`: block until one of this task's children has exited, and report which.
 *
 * Returns the child's identifier and stores its status, or -1 with ENOENT if there are no
 * children left at all.  ENOENT rather than ECHILD: there is one error number in this
 * kernel's ABI so far and inventing a second one to say "no children" when it means the
 * same thing as "nothing to wait for" is a distinction without a difference yet.
 *
 * Blocking is the whole reason `wait` is interesting: a parent that has to *wait* is a
 * task that must leave the run queue and be woken by its child's exit, which is the first
 * thing in this kernel where a task's progress depends on another task finishing. */
long sys_wait(regs_t *regs) {
    uint64_t status_ptr = arg(regs, 0);

    /* Which child, and whether to block.  Both were added for a shell's job control, and
     * both are *additional* arguments rather than a change: argument 0 is still the status
     * pointer, so every caller that passed only that is unaffected and keeps meaning "any
     * child of mine".  Argument 1 defaults to 0 for the same reason, and 0 means any
     * child -- which is what the one-argument form always meant.
     *
     * `pid` is the POSIX selector: -1 for any child, a positive number for that child, 0
     * for any child in this process's own group.  The group case is the one POSIX
     * distinguishes and this kernel does not yet: a process group is its own id here (see
     * `task_create_forked()`), so "any child in my group" and "any child" are the same set
     * until `setpgid()` starts moving tasks between groups.  Saying so is better than
     * implementing the distinction and having it be a lie for every program that uses it.
     *
     * All three arguments are read, so all three must be passed.  The registers the SysV
     * ABI does not name are scratch, and a caller that supplies two arguments to a
     * three-argument call supplies a third one out of whatever it happened to be using --
     * which for `options` is a word whose bit 0 means `WNOHANG`, so `wait()` came back
     * with "nothing yet" for a child that had already exited. */
    int64_t     pid     = (int64_t)arg(regs, 1);
    uint64_t    options = arg(regs, 2);
    const bool  nohang  = (options & WNOHANG) != 0;
    task_t     *parent  = sched_current();

    if (pid < -1)
        errno_return(EINVAL);

    if (parent == NULL) {
        kprintf("[warn] wait: task 0 asked to wait for %ld\n", (long)pid);
        errno_return(ECHILD);
    }

    /* No children at all is ECHILD and not ENOENT.  They are different facts: ENOENT is
     * "that does not exist", and a parent with no children is not looking for something
     * that failed to be found -- it has run out of children, which is what ECHILD is for,
     * and what a shell's `wait` with nothing to wait for has to be able to tell apart from
     * an error. */
    if (parent->child == NULL) {
        kprintf("[warn] wait: task %lu (%s) has no children and asked to wait for %ld\n",
                (unsigned long)parent->id, parent->name, (long)pid);
        errno_return(ECHILD);
    }

    for (;;) {
        for (task_t *c = parent->child; c != NULL; c = c->sibling) {
            if (pid > 0 && (int64_t)c->id != pid)
                continue;

            if (__atomic_load_n(&c->exited, __ATOMIC_ACQUIRE)) {
                /* Unlink before reporting, so a second `wait` cannot collect the same
                 * child again -- and so the list does not grow a chain of already-reported
                 * children for a process that never reaps. */
                for (task_t **p = &parent->child; *p != NULL; p = &(*p)->sibling) {
                    if (*p == c) {
                        *p = c->sibling;
                        c->sibling = NULL;
                        break;
                    }
                }

                /* The status goes back through the ordinary pointer check, like every
                 * other pointer a program hands over.  A `wait` whose status pointer is
                 * unmapped is an error about the pointer, not about the child. */
                if (status_ptr != 0) {
                    /* The status word, in the shape every caller expects.
                     *
                     * POSIX packs it: a normal exit is the exit code in the *high byte*, so
                     * that the low seven bits are free to say "terminated by signal N"
                     * instead, and `WIFEXITED()` is "the low seven bits are zero" --
                     * `WIFEXITED((code << 8) | low)` is true for every code, and
                     * `WIFEXITED(signal)` is false for every signal but 0.
                     *
                     * Reporting the code itself instead makes an exit of 1 come back as 1,
                     * which every caller decodes as *killed by signal 1*.  A program that
                     * ran `exit 1` and a program that was killed by SIGHUP became
                     * indistinguishable, and the difference is exactly the one a shell's
                     * `$?` is for.
                     *
                     * `task_t.exit_code` is negative for a signal death (see
                     * `signal_send()`), which is how the two are told apart here. */
                    int32_t code = (int32_t)c->exit_code;
                    int32_t status = code < 0 ? (-code & 0x7f)
                                              : ((code & 0xff) << 8);

                    if (!syscall_copy_to_user(status_ptr, &status, sizeof(status)))
                        errno_return(EFAULT);
                }

                return (long)c->id;
            }
        }

        /* `WNOHANG` is not "do not wait", it is "do not wait *if there is nothing to
         * report*".  A caller that asks a live child to wait with WNOHANG still waits --
         * that is what it asked for -- and the difference only shows when the child is
         * still running, which is the case a job-control loop is in every time it looks.
         *
         * The status is deliberately not written in this case: POSIX says the contents are
         * unspecified, and writing a zero would be a zero that a caller who ignored the
         * return value would read as "exited successfully". */
        if (nohang)
            return 0;

        task_wait();
    }
}

int syscall_init(void) {
    for (unsigned i = 0; i < SYS_MAX; i++)
        handlers[i] = NULL;

    handlers[SYS_READ]    = sys_read;
    handlers[SYS_WRITE]   = sys_write;
    handlers[SYS_WRITEV]  = sys_writev;
    handlers[SYS_OPEN]    = fs_sys_open;
    handlers[SYS_CLOSE]   = fs_sys_close;
    handlers[SYS_LSEEK]   = fs_sys_lseek;
    handlers[SYS_FSTAT]   = fs_sys_fstat;
    handlers[SYS_STAT]    = fs_sys_stat;
    handlers[SYS_LSTAT]   = fs_sys_lstat;
    handlers[SYS_FSYNC]   = fs_sys_fsync;
    handlers[SYS_SYNC]    = fs_sys_sync;
    handlers[SYS_READLINK]= fs_sys_readlink;
    handlers[SYS_GETDENTS]= fs_sys_getdents;
    handlers[SYS_DUP]     = fs_sys_dup;
    handlers[SYS_DUP2]    = fs_sys_dup2;
    handlers[SYS_MKDIR]   = fs_sys_mkdir;
    handlers[SYS_RMDIR]   = fs_sys_rmdir;
    handlers[SYS_UNLINK]  = fs_sys_unlink;
    handlers[SYS_RENAME]  = fs_sys_rename;
    handlers[SYS_LINK]    = fs_sys_link;
    handlers[SYS_SYMLINK] = fs_sys_symlink;
    handlers[SYS_CHMOD]   = fs_sys_chmod;
    handlers[SYS_CHOWN]   = fs_sys_chown;
    handlers[SYS_TRUNCATE]= fs_sys_truncate;
    handlers[SYS_FTRUNCATE]= fs_sys_ftruncate;
    handlers[SYS_UTIMES]  = fs_sys_utimes;
    handlers[SYS_CHDIR]   = fs_sys_chdir;
    handlers[SYS_GETCWD]  = fs_sys_getcwd;
    handlers[SYS_UMASK]   = fs_sys_umask;
    handlers[SYS_MOUNT]   = fs_sys_mount;
    handlers[SYS_UMOUNT]  = fs_sys_umount;
    handlers[SYS_ACCESS]  = fs_sys_access;
    handlers[SYS_FCNTL]   = fs_sys_fcntl;
    handlers[SYS_IOCTL]   = fs_sys_ioctl;
    handlers[SYS_EXIT]    = sys_exit;
    handlers[SYS_BRK]     = sys_brk;
    handlers[SYS_GETPID]  = sys_getpid;
    handlers[SYS_GETPPID] = sys_getppid;
    handlers[SYS_FORK]    = sys_fork;
    handlers[SYS_WAIT]    = sys_wait;
    handlers[SYS_PIPE]    = sys_pipe;
    handlers[SYS_EXEC]    = sys_exec;
    handlers[SYS_SETSID]  = sys_setsid;
    handlers[SYS_SETPGID] = sys_setpgid;
    handlers[SYS_GETPGID] = sys_getpgid;
    handlers[SYS_UNAME]   = sys_uname;
    handlers[SYS_GETUID]  = sys_getuid;
    handlers[SYS_GETEUID] = sys_geteuid;
    handlers[SYS_SETUID]  = sys_setuid;
    handlers[SYS_GETGID]  = sys_getgid;
    handlers[SYS_NANOSLEEP] = sys_nanosleep;
    handlers[SYS_SCHED_YIELD] = sys_sched_yield;
    handlers[SYS_GETCPU]      = sys_getcpu;
    handlers[SYS_TIMES]   = sys_times;
    handlers[SYS_GETRUSAGE] = sys_getrusage;
    handlers[SYS_REBOOT]  = sys_reboot;
    handlers[SYS_KILL]    = sys_kill;
    handlers[SYS_SIGACTION] = sys_sigaction;
    handlers[SYS_SIGRETURN] = sys_sigreturn;
    handlers[SYS_SIGPROCMASK] = sys_sigprocmask;
    handlers[SYS_SIGPENDING] = sys_sigpending;
    handlers[SYS_PAUSE]   = sys_pause;
    handlers[SYS_MMAP]    = sys_mmap;
    handlers[SYS_MUNMAP]  = sys_munmap;
    handlers[SYS_MPROTECT]= sys_mprotect;

    /* DPL 3: the gate has to be reachable from ring 3, which is the entire point.  A
     * kernel gate is DPL 0, and a user `int $0x80` against one raises #GP -- which is
     * correct, and would look here like a broken program rather than a missing door. */
    idt_set_gate(VECTOR_SYSCALL, isr_stub_128, 0, 3);
    isr_install_handler(VECTOR_SYSCALL, syscall_dispatch);

    return 0;
}
