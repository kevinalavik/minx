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

/* The tables in use.  Only valid after vmm_init(). */
page_table_t *vmm_tables(void);

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
