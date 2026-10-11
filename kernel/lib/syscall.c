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
#include "syscall.h"

#include "console.h"
#include "fd.h"
#include "programs.h"
#include "elf.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "task.h"
#include "vectors.h"
#include "vmm.h"
#include "serial.h"

/* --- the table ---------------------------------------------------------- */

typedef long (*syscall_fn_t)(regs_t *regs);

static syscall_fn_t handlers[SYS_MAX];

const char *syscall_name(unsigned nr) {
    static const char *const names[SYS_MAX] = {
        [SYS_READ]   = "read",
        [SYS_WRITE]  = "write",
        [SYS_OPEN]   = "open",
        [SYS_CLOSE]  = "close",
        [SYS_EXIT]   = "exit",
        [SYS_BRK]    = "brk",
        [SYS_WRITEV] = "writev",
    [SYS_PIPE]   = "pipe",
        [SYS_GETPID] = "getpid",
    };

    return (nr < SYS_MAX && names[nr] != NULL) ? names[nr] : "?";
}

/* --- what a program did, for the tests and for a message ----------------- */

static volatile int last_errno;
static volatile int last_exit = -1;
static volatile uint64_t exit_pid;
static volatile uint64_t unknown_calls;

void syscall_set_errno(int err) { last_errno = err; }
int  syscall_last_errno(void)   { return last_errno; }
void syscall_last_errno_reset(void) { last_errno = 0; last_exit = -1; exit_pid = 0; }

void syscall_note_exit(int code) {
    last_exit = code;
    task_t *t = sched_current();

    exit_pid = (t != NULL) ? t->id : 0;
}

int      syscall_last_exit(void)  { return last_exit; }
uint64_t syscall_exit_pid(void)   { return exit_pid; }

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
static uint64_t arg(regs_t *regs, unsigned n) {
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

    for (uint64_t off = 0; off < bytes; off += page_size) {
        pte_t leaf = pt_get(tables, addr + off);

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

    if (!syscall_user_range_ok(src, bytes, false))
        return false;

    __builtin_memcpy(dst, (const void *)(uintptr_t)src, bytes);
    return true;
}

bool syscall_copy_to_user(uint64_t dst, const void *src, size_t bytes) {
    if (bytes == 0)
        return true;

    if (!syscall_user_range_ok(dst, bytes, true))
        return false;

    __builtin_memcpy((void *)(uintptr_t)dst, src, bytes);
    return true;
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

    case FD_CONSOLE:
        /* Reading the console needs the keyboard's input buffer to be a thing a task can
         * wait on, which is the shell's stdin and not this call's job to fake.  ENOSYS
         * says the door exists and the room behind it is not built.  What it must not do
         * is block here with interrupts disabled, which would stop the machine rather than
         * the program. */
        errno_return(ENOSYS);

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
     * length depends on where the hole was. */
    if (!syscall_user_range_ok(buf, count, true))
        errno_return(EFAULT);

    return sys_fd_read(fd, (void *)(uintptr_t)buf, count);
}

long sys_pipe(regs_t *regs) {
    return pipe_create(arg(regs, 0));
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
    uint64_t want = arg(regs, 0);
    task_t *t = sched_current();

    if (t == NULL)
        errno_return(ENOSYS);

    /* Growing a heap is phase 7, with an allocator behind it.  Reporting the current
     * break keeps a program that probes it -- which is what a loader does first --
     * working. */
    if (want == 0)
        return (long)t->brk;

    if (want < t->brk)
        errno_return(EINVAL);

    /* Not implemented, and saying so beats pretending: a program that believes its heap
     * grew will store into it and fault. */
    errno_return(ENOMEM);
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
                          size_t bytes, uint32_t stack_pages) {
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
                                   USER_IMAGE_BASE + sizeof(Elf64_Ehdr));

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

    /* Swap.  The old address space and the old stack go, and the old user frame is a
     * kernel alias into the old stack's page -- which has just been unmapped, so nothing
     * may read it after this point. */
    page_table_t *old_tables = t->tables;
    void *old_stack = t->stack;

    t->tables = fresh->tables;
    t->user_frame = fresh->user_frame;
    t->user_stack_phys = fresh->user_stack_phys;
    t->user_stack_pages = fresh->user_stack_pages;
    t->saved_rsp = fresh->saved_rsp;
    t->started = false;
    t->cow = NULL;             /* the old pages are gone; the list described them */

    fresh->tables = NULL;
    fresh->stack = NULL;
    kfree(fresh);

    vmm_address_space_destroy(old_tables);
    kfree(old_tables);
    kfree(old_stack);

    kprintf("exec: %s is now pid %lu, entry %#lx\n", name, (unsigned long)t->id,
            (unsigned long)r.entry);
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

/* Create a task and load one registered program into it.  Split out because the loader
 * body is now reached from two places and neither of them wants the other's half. */
task_t *program_start(const program_image_t *p) {
    const char *name = p->name;
    const void *image = p->image;
    size_t bytes = p->bytes;
    uint32_t stack_pages = p->stack_pages;

    task_t *t = task_create_user(name, 0, stack_pages, TASK_WEIGHT_DEFAULT);

    if (t == NULL) {
        kwarn("program: could not create a task for %s", name);
        return NULL;
    }

    /* Where the program headers are expected: inside the first PT_LOAD, at the offset
     * the ELF header says.  The caller passes the file's base and the loader works out
     * the rest, because the link address and the file offset can differ. */
    elf_load_result_t r = elf_load(image, bytes, t, stack_pages,
                                   USER_IMAGE_BASE + sizeof(Elf64_Ehdr));

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
     * pointer where a selector belongs is a #GP whose error code is the low half of the
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
     * Waking the task makes it runnable, and on this machine the next tick can hand it
     * the processor while the report below is still being rendered.  The two then write
     * to the console a character at a time -- the program's own output interleaved with
     * this line, which reads as though the program had printed something strange:
     * `argc=1` came out as `argc=g1r` because "program:" was being emitted into the
     * middle of it.  Nothing was actually wrong with either line, and that is exactly
     * why it is worth fixing: a report that cannot be read is a report nobody has. */
    kprintf("program: %s loaded, entry %#lx, %u segment(s), %lu bytes, stack %#lx\n",
            name, (unsigned long)r.entry, r.segments, (unsigned long)r.bytes,
            (unsigned long)r.entry_rsp);

    if (task_wakeup(t) != 0) {
        kwarn("program: could not queue %s", name);
        return NULL;
    }

    return t;
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
    task_t *parent = sched_current();

    if (parent == NULL || parent->child == NULL) {
        errno_return(ENOENT);
    }

    for (;;) {
        for (task_t *c = parent->child; c != NULL; c = c->sibling) {
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
                    int32_t status = (int32_t)c->exit_code;

                    if (!syscall_copy_to_user(status_ptr, &status, sizeof(status)))
                        errno_return(EFAULT);
                }

                return (long)c->id;
            }
        }

        task_wait();
    }
}

int syscall_init(void) {
    for (unsigned i = 0; i < SYS_MAX; i++)
        handlers[i] = NULL;

    handlers[SYS_WRITE] = sys_write;
    handlers[SYS_READ] = sys_read;
    handlers[SYS_OPEN] = sys_open;
    handlers[SYS_CLOSE] = sys_close;
    handlers[SYS_WRITEV] = sys_writev;
    handlers[SYS_PIPE] = sys_pipe;
    handlers[SYS_EXIT] = sys_exit;
    handlers[SYS_BRK] = sys_brk;
    handlers[SYS_GETPID] = sys_getpid;
    handlers[SYS_FORK] = sys_fork;
    handlers[SYS_WAIT] = sys_wait;

    /* DPL 3: the gate has to be reachable from ring 3, which is the entire point.  A
     * kernel gate is DPL 0, and a user `int $0x80` against one raises #GP -- which is
     * correct, and would look here like a broken program rather than a missing door. */
    idt_set_gate(VECTOR_SYSCALL, isr_stub_128, 0, 3);
    isr_install_handler(VECTOR_SYSCALL, syscall_dispatch);

    return 0;
}
