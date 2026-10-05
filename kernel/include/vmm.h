/* minx - virtual memory manager.
 *
 * One set of page tables serves both the kernel and, from phase 6, user space:
 * the kernel is linked into the higher half at 0xffffffff80000000 and user
 * addresses live below 4 GiB, which is PML4 slot 0 against the kernel's slot
 * 511.  They never share a top-level entry, so a user mapping never has to
 * widen a kernel page directory.
 *
 * The direct map (phys_to_virt) covers all physical memory so the kernel can
 * reach any frame it owns.  Device MMIO is deliberately *not* in it: MMIO has to
 * be mapped uncacheable and with specific page sizes, and putting it in a
 * catch-all direct map is how a kernel ends up doing ordinary stores to a
 * device register.
 */
#ifndef MINX_VMM_H
#define MINX_VMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pagetables.h"
#include "types.h"

/* Build the kernel's own page tables and switch to them.
 *
 * After this returns the direct map is live, the kernel image is mapped at its
 * linked address, and every MMIO window the kernel has asked for through
 * vmm_map_mmio() before the call is reachable. */
int  vmm_init(void);

/* True once CR3 points at the kernel's own tables. */
bool vmm_active(void);

/* The kernel's own tables.  Only valid after vmm_init().
 *
 * Named separately from vmm_tables() because they are two different questions:
 * this one is "the tables the kernel was built with", which never changes and is
 * what an application processor's trampoline needs before there is a task at all.
 * vmm_tables() is "the tables in use right now", which is a task's own once user
 * space exists. */
page_table_t *vmm_kernel_tables(void);

/* The tables in use right now: the current task's own address space, or the
 * kernel's when nothing is running yet (during boot, and on an application
 * processor between arriving and entering the scheduler).
 *
 * Everything that maps, unmaps or walks memory goes through this rather than
 * through vmm_kernel_tables(), which is what makes a user mapping land in the
 * address space of the task that asked for it and nothing else. */
page_table_t *vmm_tables(void);

/* --- address spaces ----------------------------------------------------- */

/* A fresh address space: the kernel's upper half copied in, the lower half
 * empty.
 *
 * Copying rather than sharing the top-level entries is deliberate.  The kernel
 * lives at 0xffffffff80000000, which is PML4 slot 511, and user addresses live
 * below 4 GiB, which is slot 0 -- so entries 256..511 can be shared by pointer
 * between every address space and only slots 0..255 need to be per-process.
 * They are not shared here, for one reason: `CR4.SMAP` and the whole point of
 * SMAP is that a supervisor write through the direct map cannot reach a user
 * page, which requires the two halves to be separate top-level entries
 * genuinely separate -- and sharing them by pointer would put user mappings
 * into the kernel's walk.  Sixteen pointers per space is cheaper than the
 * subtlety.
 *
 * Returns 0 on success, -1 if a PML4 cannot be allocated. */
int  vmm_address_space_create(page_table_t *out);

/* Release a whole address space: every table it owns, and every user page.  The
 * kernel half is left alone -- those tables are the kernel's.  It is not called by
 * the scheduler; nothing exits a task yet -- but it is here because the
 * page-fault handler's "user page" fixup path needs somewhere to put a page and
 * a reader needs to know that a space can be thrown away. */
int  vmm_address_space_destroy(page_table_t *tables);

/* Load `tables` into CR3.  The caller is responsible for having flushed CR3's
 * previous contents if anything it mapped has changed; a context switch between
 * two address spaces does not need to, because the TLB is tagged with the CR3
 * value that filled it. */
void vmm_address_space_switch(page_table_t *tables);

/* One zeroed, user-accessible page: allocate, zero, return its physical address,
 * or 0 if there is no memory.  The mapping into an address space is the
 * caller's business -- vmm_map_user() against the tables it wants. */
uint64_t vmm_user_page_alloc(void);
void     vmm_user_page_free(uint64_t phys);

/* Where user space lives.  Chosen so a user pointer can never be mistaken for a
 * kernel one, and so `((uint64_t)ptr >> 47)` is a reliable test: everything
 * below 0x0000800000000000 is user, everything from 0xffff800000000000 up is
 * the kernel. */
#define USER_SPACE_END   0x0000800000000000ull
#define USER_STACK_TOP    0x00007ffffffff000ull
#define USER_STACK_SIZE   0x0000000000010000ull   /* 64 KiB */
#define USER_IMAGE_BASE   0x0000000000400000ull   /* where a PT_LOAD is placed */

static inline bool vmm_is_user_addr(uint64_t addr) {
    return addr < USER_SPACE_END;
}


/* Map a device register window: uncacheable, present and writable, and not
 * user accessible.  Must be called before vmm_init() for the LAPIC and IOAPIC,
 * because they are needed immediately afterwards. */
int  vmm_map_mmio(uint64_t phys, size_t bytes, const char *name);
int  vmm_unmap_mmio(uint64_t phys);

/* Map a physical range at the same virtual address, below 4 GiB.  For the
 * application-processor trampoline, which needs to be fetching from low memory
 * while it builds the PDPTE it crosses the rest of memory with. */
int  vmm_map_low_identity(uint64_t phys, size_t bytes);

/* Point the reset vector (0xfffff000) at `phys`, which should be a page holding
 * a wait-for-startup-IPI loop.  See the comment in vmm.c: firmware's own
 * behaviour there cannot be assumed. */
int  vmm_map_reset_vector(uint64_t phys);

/* Reserve a virtual address range for a kernel allocator to fill in later, by
 * mapping it through `pages`.  Used for the heap, which grows by attaching
 * regions the physical allocator hands out. */
int  vmm_map_kernel_virt(uint64_t virt, uint64_t phys, size_t bytes,
                         bool executable);
int  vmm_unmap_kernel_virt(uint64_t virt, size_t bytes);
int  vmm_set_nx_range(uint64_t virt, size_t bytes, bool nx);

/* Release the bootloader's memory.
 *
 * Not done by vmm_init(), because the bootloader starts the application
 * processors itself and leaves them running on its own page tables: its frames
 * stay in use until each of them has been handed an entry point and switched to
 * the kernel's address space. */
int  vmm_release_boot_tables(void);

/* User-space mappings.  Phase 6 fills these in; they exist now so the layout
 * and the page fault handler's user/kernel split can be tested. */
int  vmm_map_user(uint64_t virt, uint64_t phys, size_t bytes, bool writable);

/* As vmm_translate(), but into a named address space rather than the running task's. */
uint64_t vmm_translate_in(page_table_t *tables, uint64_t virt);

/* As vmm_map_user(), but into a named address space rather than the running task's.
 *
 * Needed by the ELF loader, which fills in a task's address space before that task has
 * ever been scheduled: vmm_map_user() would map into the loader's own tables, succeed,
 * and leave the program with no code at its entry point. */
int  vmm_map_into(page_table_t *tables, uint64_t virt, uint64_t phys, size_t bytes,
                  bool writable);
int  vmm_unmap_user(uint64_t virt, size_t bytes);

/* Total pages mapped, for /proc/meminfo and the phase-11 accounting check. */
typedef struct {
    uint64_t kernel_pages;
    uint64_t user_pages;
    uint64_t mmio_pages;
    uint64_t huge_pages;
} vmm_stats_t;

void vmm_get_stats(vmm_stats_t *out);

/* Translate a linear address through the kernel's tables. */
uint64_t vmm_translate(uint64_t virt);

#endif /* MINX_VMM_H */
