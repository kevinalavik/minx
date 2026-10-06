/* minx - per-process address spaces.
 *
 * Split out of vmm.c rather than added to it, because the interesting part is not
 * the mapping -- vmm_map_user() already does that -- it is the boundary.  A process's
 * address space is a PML4 whose upper half is the kernel's and whose lower half is its
 * own, and every path that maps, unmaps or walks memory has to resolve to whichever
 * one the running task owns.  Keeping that in one file means the "which tables?"
 * question has exactly one answer in the tree.
 *
 * Three things are worth stating, because each was a way to get this wrong:
 *
 *  - **The kernel half is shared by copying the top-level entries, not the tables
 *    behind them.**  The kernel image and the direct map are mapped into slot 511 and
 *    above; user space is slots 0 to 255.  Copying slot 511's *entry* into every new
 *    PML4 means every address space reaches the same physical page directories, so a
 *    mapping the kernel makes is visible everywhere for free, and one made on a
 *    process's behalf cannot disturb the others.  Sixteen pointers per space, versus
 *    copying four levels of kernel tables per process, is not a close call.
 *
 *  - **A page allocated for user space is zeroed.**  A user page that starts as
 *    whatever the recycled frame happened to hold is a user program reading another
 *    program's data -- not a kernel bug, an information leak that no page-table
 *    permission can prevent.
 *
 *  - **Nothing here loads CR3.**  The scheduler does, because it is the only place
 *    that knows a switch is happening; loading CR3 from a mapping function would let
 *    an address space change under a processor in the middle of something.
 */
#include "vmm.h"

#include "kprintf.h"
#include "pmm.h"
#include "string.h"

/* Allocate one zeroed frame and return its physical address. */
static uint64_t alloc_zeroed_phys(void) {
    uint64_t phys = pmm_alloc_phys_page();

    if (phys == 0)
        return 0;

    memset((void *)phys_to_virt(phys), 0, page_size);
    return phys;
}

int vmm_address_space_create(page_table_t *out) {
    if (out == NULL || !vmm_active())
        return -1;

    page_table_t *k = vmm_kernel_tables();
    uint64_t pml4_phys = alloc_zeroed_phys();

    if (pml4_phys == 0)
        return -1;

    out->pml4 = pml4_phys;
    out->pml4_v = (uint64_t *)phys_to_virt(pml4_phys);
    out->pdpt_v = NULL;

    /* The kernel half, shared by pointer.  Slots 256 and above cover
     * 0xffff800000000000 and up, which is where the kernel image, the direct map,
     * the heap window and the APIC windows all live.  Slots 0 to 255 cover the rest
     * of the 48-bit space, which is where user space lives and which starts empty. */
    uint64_t *dst = out->pml4_v;
    const uint64_t *src = k->pml4_v;

    for (unsigned i = 256; i < PML4_ENTRY_COUNT; i++)
        dst[i] = src[i];

    for (unsigned i = 0; i < 256; i++)
        dst[i] = 0;

    /* Slot 0 now belongs to the process, so the identity mapping the processor needs
     * by physical address has to be made again here -- into *this* space's own tables,
     * walked from its own slot 0, so that a program mapping anything nearby cannot
     * disturb the kernel's copy and destroy() frees only what this space owns. */
    if (vmm_map_identity_for_new_space(out) != 0) {
        pmm_free_phys_page(pml4_phys);
        memset(out, 0, sizeof(*out));
        return -1;
    }

    return 0;
}

int vmm_address_space_destroy(page_table_t *tables) {
    if (tables == NULL || !vmm_active())
        return -1;

    /* Walk each PML4 slot below 256 down to its leaves, freeing every table on the
     * way.  Only the tables: the *pages* a user mapping covers belong to whoever
     * allocated them, and a caller that wants them gone has to say so.  Guessing
     * here would free pages the kernel is still handing out.
     *
     * A leaf can be a huge page rather than a pointer, so presence and hugeness are
     * tested together -- a huge leaf has nothing below it to free. */
    for (unsigned slot = 0; slot < 256; slot++) {
        uint64_t entry = tables->pml4_v[slot];

        if (!pte_present(entry))
            continue;

        uint64_t *pdpt = (uint64_t *)phys_to_virt(pte_phys(entry));

        for (unsigned a = 0; a < 512; a++) {
            uint64_t e1 = pdpt[a];

            if (!pte_present(e1))
                continue;

            uint64_t *pd = (uint64_t *)phys_to_virt(pte_phys(e1));

            for (unsigned b = 0; b < 512; b++) {
                uint64_t e2 = pd[b];

                if (!pte_present(e2) || pte_huge(e2))
                    continue;
                pmm_free_phys_page(pte_phys(e2));
            }

            pmm_free_phys_page(pte_phys(e1));
        }

        pmm_free_phys_page(pte_phys(entry));
    }

    pmm_free_phys_page(tables->pml4);
    memset(tables, 0, sizeof(*tables));
    return 0;
}

void vmm_address_space_switch(page_table_t *tables) {
    if (tables == NULL || tables->pml4 == 0)
        return;

    /* Loading the same address space is a full serialising round trip and an
     * expensive TLB flush for nothing.  Comparing before writing is what lets a
     * context switch between two kernel tasks skip it entirely -- which is every
     * switch there is until user space exists. */
    if (pt_read_cr3() == tables->pml4)
        return;

    pt_write_cr3(tables->pml4);
}

uint64_t vmm_user_page_alloc(void) {
    return alloc_zeroed_phys();
}

void vmm_user_page_free(uint64_t phys) {
    if (phys != 0)
        pmm_free_phys_page(phys);
}
