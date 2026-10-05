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
#include "elf.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "task.h"
#include "vectors.h"
#include "vmm.h"

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
    if (!vmm_is_user_addr(addr))
        return false;

    /* Wrapping is not a range: `addr + bytes` coming back below `addr` means the end
     * is past the top of the address space, and every check below would be applied to
     * addresses the program never meant. */
    if (bytes != 0 && addr + bytes < addr)
        return false;

    page_table_t *tables = vmm_tables();

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
    switch (fd) {
    case 1:                       /* stdout */
    case 2:                       /* stderr */
        /* The framebuffer and the serial mirror, so a program's output is in a
         * screenshot and in a log.  Going through console_write() rather than putting
         * characters out one at a time keeps the two in step, which matters because the
         * serial mirror is what the automated tests read. */
        console_write((const char *)buf, bytes);
        return (long)bytes;

    case 0:                       /* stdin */
        errno_return(EBADF);

    default:
        /* Anything else is not a descriptor this kernel has.  EBADF rather than a
         * silent success: a program that believes it wrote somewhere is worse off than
         * one that knows it did not. */
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

    (void)buf;
    (void)count;

    /* There is nothing to read from yet: no pipe, no file, no keyboard on a
     * descriptor.  A read that waits for data has to put the task on a wait queue and
     * be woken later, which is phase 7.  ENOSYS says the door exists and the room
     * behind it does not.  What it must not do is block here with interrupts disabled,
     * which would stop the machine rather than the program. */
    (void)fd;
    errno_return(ENOSYS);
}

long sys_open(regs_t *regs) {
    (void)regs;
    errno_return(ENOSYS);
}

long sys_close(regs_t *regs) {
    (void)regs;
    errno_return(ENOSYS);
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
    int code = (int)(int64_t)arg(regs, 0);

    syscall_note_exit(code);

    /* Does not return, and nothing may touch this frame afterwards: the task's stack is
     * about to be abandoned, and the handler it was called from is running on it. */
    task_exit();

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
     * check is two instructions. */
    if (regs->rax > 0 && (uint64_t)regs->rax >= USER_SPACE_END) {
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
task_t *program_load_and_run(const char *name, const void *image, size_t bytes,
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
    uint64_t *frame = (uint64_t *)phys_to_virt(virt_to_phys(USER_STACK_TOP)
                                               - USER_FRAME_BYTES);

    frame[2] = r.entry;            /* RIP */
    frame[4] = r.entry_rsp;        /* RSP */

    /* A virtual address: this is what the CPU loads into RSP. */
    t->saved_rsp = USER_STACK_TOP - USER_FRAME_BYTES - USER_CTX_BYTES;
    t->started = false;

    if (task_wakeup(t) != 0) {
        kwarn("program: could not queue %s", name);
        return NULL;
    }

    kprintf("program: %s loaded, entry %#lx, %u segment(s), %lu bytes, stack %#lx\n",
            name, (unsigned long)r.entry, r.segments, (unsigned long)r.bytes,
            (unsigned long)r.entry_rsp);

    return t;
}

int syscall_init(void) {
    for (unsigned i = 0; i < SYS_MAX; i++)
        handlers[i] = NULL;

    handlers[SYS_WRITE] = sys_write;
    handlers[SYS_READ] = sys_read;
    handlers[SYS_OPEN] = sys_open;
    handlers[SYS_CLOSE] = sys_close;
    handlers[SYS_WRITEV] = sys_writev;
    handlers[SYS_EXIT] = sys_exit;
    handlers[SYS_BRK] = sys_brk;
    handlers[SYS_GETPID] = sys_getpid;

    /* DPL 3: the gate has to be reachable from ring 3, which is the entire point.  A
     * kernel gate is DPL 0, and a user `int $0x80` against one raises #GP -- which is
     * correct, and would look here like a broken program rather than a missing door. */
    idt_set_gate(VECTOR_SYSCALL, isr_stub_128, 0, 3);
    isr_install_handler(VECTOR_SYSCALL, syscall_dispatch);

    return 0;
}
