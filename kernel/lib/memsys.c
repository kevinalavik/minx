/* minx - the memory system calls.
 *
 * `mmap`, `munmap` and `mprotect`, and the heap behind `brk`.  They are in one file
 * because they are three ways of asking the same question -- what is this process allowed
 * to touch -- and because the answer has to come from the same place for all of them: a
 * mapping made by one and a mapping made by another have to be describable by the same
 * rule, or `mprotect` on a `brk` page is a special case somebody has to remember.
 *
 * The model is the simplest one that is still honest, and the simplifications are the ones
 * a program can notice, so they are written down rather than left to be discovered:
 *
 *   - Anonymous memory only.  A file mapping is `mmap` on a descriptor, and every program
 *     here reads its files with `read`, which is what the kernel's own loader does.  A file
 *     mapping is the first thing to add when something needs one, and saying ENOSYS now is
 *     better than a mapping that maps the wrong bytes.
 *   - The heap is a bump.  `brk` grows it and never moves it, so a program that has taken
 *     an address in the heap has taken an address it keeps.  `free()` in the libc does not
 *     use `brk` for that reason.
 *   - `munmap` frees a range that is the top of the heap or the top of the mmap area, and
 *     says ENOMEM otherwise.  A general hole-punching unmap needs a free list, and a
 *     half-correct one gives back ranges that are still mapped -- which is worse than
 *     refusing.
 */
#include "memsys.h"

#include "kmalloc.h"
#include "string.h"
#include "syscall.h"
#include "task.h"
#include "vmm.h"

/* Where a process's `mmap` area starts and how far it may run.
 *
 * Above the program's own segments -- which the linker puts at 4 MiB and which end a few
 * megabytes later -- and far below the stack at the top of the address space.  The gap is
 * enormous and deliberately so: a process that maps a gigabyte is not a process this
 * kernel has to have thought about. */
#define MMAP_BASE   0x0000000010000000ull      /* 256 MiB */
#define MMAP_LIMIT  0x0000100000000000ull      /* 1 TiB   */

/* `mprotect` bits, and the ones this kernel knows. */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

/* `mmap` flags. */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

static bool next_page_free(task_t *t, uint64_t addr, size_t bytes) {
    for (uint64_t off = 0; off < bytes; off += page_size) {
        if (vmm_translate_in(t->tables, addr + off) != 0)
            return false;
    }
    return true;
}

long sys_mmap(regs_t *regs) {
    task_t *t = sched_current();
    uint64_t hint = syscall_arg(regs, 0);
    size_t length = (size_t)syscall_arg(regs, 1);
    uint32_t prot = (uint32_t)syscall_arg(regs, 2);
    uint32_t flags = (uint32_t)syscall_arg(regs, 3);

    if (t == NULL || t->tables == NULL)
        errno_return(ENOMEM);

    if (length == 0)
        errno_return(EINVAL);

    /* A file mapping needs a descriptor and this kernel has no page cache to back one, so
     * the only kind there is here is anonymous.  Said plainly rather than mapped to
     * something: a program that asked for a file mapping and got anonymous memory would
     * read zeroes and believe they were the file. */
    if ((flags & MAP_ANONYMOUS) == 0)
        errno_return(ENOMEM);

    size_t bytes = ALIGN_UP(length, page_size);
    uint64_t addr;

    if ((flags & MAP_FIXED) && hint != 0) {
        if (!vmm_is_user_addr(hint))
            errno_return(EINVAL);
        if (hint + bytes < hint || hint + bytes > MMAP_LIMIT)
            errno_return(EINVAL);
        if (!next_page_free(t, hint, bytes))
            errno_return(EBUSY);
        addr = hint;
    } else {
        /* A bump.  The hint is honoured when it is free and legal, because a program that
         * asks for an address has a reason and because honouring it is what makes a
         * second mapping with the same hint fail rather than silently move. */
        addr = hint;

        if (addr == 0 || !vmm_is_user_addr(addr) || addr < MMAP_BASE ||
            addr + bytes > MMAP_LIMIT || !next_page_free(t, addr, bytes))
            addr = t->mmap_next;

        if (addr + bytes > MMAP_LIMIT)
            errno_return(ENOMEM);
    }

    bool writable = (prot & PROT_WRITE) != 0;

    for (size_t off = 0; off < bytes; off += page_size) {
        uint64_t phys = vmm_user_page_alloc();

        if (phys == 0) {
            /* Give back what was taken for this call, so a failed mapping leaves no
             * address space consumed.  A mapping that fails halfway and keeps the pages it
             * had already taken is a leak that only shows up under memory pressure. */
            for (size_t back = 0; back < off; back += page_size) {
                uint64_t p = vmm_translate_in(t->tables, addr + back);

                if (p != 0) {
                    vmm_unmap_user(addr + back, page_size);
                    vmm_user_page_free(p);
                }
            }
            errno_return(ENOMEM);
        }

        if (vmm_map_into(t->tables, addr + off, phys, page_size, writable) != 0) {
            vmm_user_page_free(phys);

            for (size_t back = 0; back < off; back += page_size) {
                uint64_t p = vmm_translate_in(t->tables, addr + back);

                if (p != 0) {
                    vmm_unmap_user(addr + back, page_size);
                    vmm_user_page_free(p);
                }
            }
            errno_return(ENOMEM);
        }
    }

    if (addr + bytes > t->mmap_next)
        t->mmap_next = addr + bytes;

    /* Execute permission is honoured by making the pages not-NX, which is the only way to
     * get it back: the loader maps a program not-NX and nothing here turns NX on.  A PROT_EXEC
     * mapping therefore works, and one that is also PROT_WRITE is writable-and-executable,
     * which is a thing this kernel allows and docs/MEMORY.md says it allows. */
    if ((prot & PROT_EXEC) != 0) {
        for (size_t off = 0; off < bytes; off += page_size)
            (void)pt_set_nx(t->tables, addr + off, false);
    }

    (void)MAP_SHARED;
    (void)MAP_PRIVATE;
    return (long)addr;
}

long sys_munmap(regs_t *regs) {
    task_t *t = sched_current();
    uint64_t addr = syscall_arg(regs, 0);
    size_t length = (size_t)syscall_arg(regs, 1);

    if (t == NULL || t->tables == NULL)
        errno_return(ENOMEM);

    if (length == 0)
        errno_return(EINVAL);

    if (!IS_ALIGNED(addr, page_size))
        errno_return(EINVAL);

    size_t bytes = ALIGN_UP(length, page_size);

    /* Only the top of the area.  See the note at the top of this file: a general unmap needs
     * a free list, and a free list that is wrong gives back an address that is still
     * mapped, which is a use-after-free a program cannot see coming. */
    if (addr + bytes != t->mmap_next)
        errno_return(ENOMEM);

    for (size_t off = 0; off < bytes; off += page_size) {
        uint64_t phys = vmm_translate_in(t->tables, addr + off);

        if (phys == 0)
            errno_return(EINVAL);

        vmm_unmap_user(addr + off, page_size);
        vmm_user_page_free(phys);
    }

    t->mmap_next = addr;
    return 0;
}

long sys_mprotect(regs_t *regs) {
    task_t *t = sched_current();
    uint64_t addr = syscall_arg(regs, 0);
    size_t length = (size_t)syscall_arg(regs, 1);
    uint32_t prot = (uint32_t)syscall_arg(regs, 2);

    if (t == NULL || t->tables == NULL)
        errno_return(ENOMEM);

    if (length == 0)
        errno_return(EINVAL);
    if (!IS_ALIGNED(addr, page_size))
        errno_return(EINVAL);

    size_t bytes = ALIGN_UP(length, page_size);

    for (size_t off = 0; off < bytes; off += page_size) {
        uint64_t at = addr + off;
        pte_t leaf = pt_get(t->tables, at);

        if (!pte_present(leaf))
            errno_return(ENOMEM);

        /* A page a fork has made read-only for copying is not "already protected": it is
         * waiting to be copied, and turning it back to writable here would let a program
         * defeat copy-on-write by protecting its own pages.  Resolving the copy first is
         * what keeps the two mechanisms from being confused. */
        if (!pte_writable(leaf) && t->cow != NULL)
            (void)vmm_cow_fault(t->tables, t->cow, at);

        if (prot == PROT_NONE) {
            (void)pt_clear_nx(t->tables, at);
            if (!pt_set_user(t->tables, at, false))
                errno_return(ENOMEM);
            continue;
        }

        if (!pt_set_user(t->tables, at, true))
            errno_return(ENOMEM);

        if (!pt_set_writable(t->tables, at, (prot & PROT_WRITE) != 0))
            errno_return(ENOMEM);

        (void)pt_set_nx(t->tables, at, (prot & PROT_EXEC) == 0);
    }

    return 0;
}

/* `brk`: the heap.
 *
 * A bump, and the break only moves up.  `brk(0)` reports it, a larger value grows it and
 * reports the value it actually got, and a smaller one is refused -- which is what makes a
 * program that frees its whole heap fail rather than silently lose the memory under every
 * pointer it holds.  The libc's `free()` therefore does not use `brk`, and `docs/MEMORY.md`
 * says why. */
long sys_brk_grow(regs_t *regs) {
    task_t *t = sched_current();
    uint64_t want = syscall_arg(regs, 0);

    if (t == NULL || t->tables == NULL)
        errno_return(ENOMEM);

    if (want == 0)
        return (long)t->brk;

    if (want < t->brk)
        errno_return(EINVAL);

    size_t bytes = ALIGN_UP(want - t->brk, page_size);

    for (size_t off = 0; off < bytes; off += page_size) {
        uint64_t at = t->brk + off;
        uint64_t phys = vmm_user_page_alloc();

        if (phys == 0) {
            for (size_t back = 0; back < off; back += page_size) {
                uint64_t p = vmm_translate_in(t->tables, t->brk + back);

                if (p != 0) {
                    vmm_unmap_user(t->brk + back, page_size);
                    vmm_user_page_free(p);
                }
            }
            /* ENOMEM, and the break unchanged: a program that cannot get memory is told so
             * rather than given a break that names pages it does not have. */
            errno_return(ENOMEM);
        }

        if (vmm_map_into(t->tables, at, phys, page_size, true) != 0) {
            vmm_user_page_free(phys);

            for (size_t back = 0; back < off; back += page_size) {
                uint64_t p = vmm_translate_in(t->tables, t->brk + back);

                if (p != 0) {
                    vmm_unmap_user(t->brk + back, page_size);
                    vmm_user_page_free(p);
                }
            }
            errno_return(ENOMEM);
        }
    }

    t->brk += bytes;
    return (long)t->brk;
}