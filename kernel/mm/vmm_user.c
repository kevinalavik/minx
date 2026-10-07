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

/* --- copy-on-write twins ------------------------------------------------ */

/* Copy one leaf page table, dropping write permission on the writable leaves.
 *
 * And dropping it from the *source* as well, which is the half that makes this copy-on-
 * write rather than copy-on-write-for-the-child-only.  The two page tables are separate
 * frames, but they name the same physical pages, so the only bit that stands between the
 * processes writing over each other is in one of them -- and if only the twin's bit is
 * cleared then the parent writes straight through to the shared frame and the child's copy
 * is never made.  A copy-on-write scheme that protects only the child is not a scheme.
 *
 * Both leaves end up read-only and *both* processes fault on the first write, which is
 * also what makes the recorded entries worth having: each side restores its own. */
static bool cow_copy_pt(uint64_t *slot, uint64_t *src_v, uint64_t base,
                        cow_list_t *cow) {
    uint64_t frame = pmm_alloc_phys_page();

    if (frame == 0)
        return false;

    uint64_t *dst = (uint64_t *)phys_to_virt(frame);

    memcpy(dst, src_v, page_size);

    for (unsigned i = 0; i < PML4_ENTRY_COUNT; i++) {
        uint64_t e = dst[i];

        if (!pte_present(e) || !pte_writable(e) || pte_huge(e))
            continue;

        if (cow->count >= COW_MAX_PAGES) {
            cow->full = true;
            return false;
        }
        cow->page[cow->count].virt = base + (uint64_t)i * page_size;
        cow->page[cow->count].entry = e;
        cow->count++;
        dst[i] = e & ~PTE_WRITE;
        src_v[i] = e & ~PTE_WRITE;
    }

    /* With flags.  A bare frame number in a directory entry reads as *not present* --
     * bit 0 is the present bit and a physical address never has it set -- so the twin would
     * have a page directory pointing at a perfectly good leaf table that the CPU refuses to
     * walk into, and every access through it would fault.
     *
     * This is not a subtlety: it is what the entry looked like on the first run, and the
     * symptom was a forked child dying on the first instruction it tried to fetch, at the
     * address of `test %rax,%rax` after its `fork` returned.  Nothing else in the address
     * space was wrong, and nothing said so. */
    *slot = pte_make(frame, PTE_PRESENT | PTE_WRITE | PTE_USER);
    return true;
}

/* Build `dst` as a copy-on-write twin of `src`.
 *
 * Read-only pages are shared outright: nothing can ever write them, so nothing can ever
 * notice.  A writable page has its leaf duplicated instead, and write permission taken
 * away from *both* sides -- the parent's leaf is in a page table the two now share, so
 * there is only one bit to clear, and clearing it in the twin clears it for the parent
 * too.  That is the point: the first of the two to write takes a fault and gets a private
 * copy, and the other one finds the fault waiting for it as well.
 *
 * The duplication is at the level of the *leaf* page table, not the leaf, and not the page
 * directory.  A leaf has to be private, because the two processes must be able to point
 * it at different frames.  A page directory and a page directory pointer table can be
 * shared whenever every leaf under them is already read-only, and for a small program that
 * is nearly all of them -- so the tables a `fork` duplicates are one per 4 MiB of address
 * space that actually contains something writable, which for `hello` is one.
 *
 * Sharing a page table has a cost worth naming: a fault that *adds* a mapping would edit
 * the twin's tables and so the parent's.  Nothing in this kernel does that from ring 3 --
 * mmap arrives in phase 11 and will need this revisited -- and the alternative,
 * duplicating every table, would make `fork` cost megabytes. */

/* TEMP diagnostic. */
int vmm_fork_aspace(page_table_t *dst, page_table_t *src, cow_list_t *cow) {
    memset(cow, 0, sizeof(*cow));

    uint64_t pml4_phys = alloc_zeroed_phys();

    if (pml4_phys == 0)
        return -1;

    dst->pml4 = pml4_phys;
    dst->pml4_v = (uint64_t *)phys_to_virt(pml4_phys);
    dst->pdpt_v = NULL;

    const uint64_t *k = vmm_kernel_tables()->pml4_v;

    for (unsigned i = 256; i < PML4_ENTRY_COUNT; i++)
        dst->pml4_v[i] = k[i];

    for (unsigned i = 0; i < 256; i++) {
        uint64_t top = src->pml4_v[i];

        if (!pte_present(top))
            continue;

        /* The child's own PDPT: its PD entries may have to be rewritten below, so this
         * one cannot be shared. */
        uint64_t pdpt_phys = pmm_alloc_phys_page();

        if (pdpt_phys == 0)
            return -1;

        uint64_t *pdpt = (uint64_t *)phys_to_virt(pdpt_phys);

        memset(pdpt, 0, page_size);
        memcpy(pdpt, (const uint64_t *)phys_to_virt(pte_phys(top)), page_size);


        for (unsigned a = 0; a < PML4_ENTRY_COUNT; a++) {
            uint64_t mid = pdpt[a];
            uint64_t *src_mid;
            uint64_t *mid_v;
            bool any_writable = false;

            if (!pte_present(mid) || pte_huge(mid))
                continue;

            src_mid = (uint64_t *)phys_to_virt(pte_phys(mid));

            /* Is there anything writable anywhere under this directory?  If not, the whole
             * directory can be shared with the parent untouched: nothing in it can ever
             * change, so there is nothing for the two processes to disagree about. */
            for (unsigned b = 0; b < PML4_ENTRY_COUNT && !any_writable; b++) {
                uint64_t leaf = src_mid[b];
                const uint64_t *pt;

                if (!pte_present(leaf) || pte_huge(leaf))
                    continue;

                pt = (const uint64_t *)phys_to_virt(pte_phys(leaf));

                for (unsigned c = 0; c < PML4_ENTRY_COUNT; c++) {
                    uint64_t l = pt[c];

                    if (pte_present(l) && !pte_huge(l) && pte_writable(l)) {
                        any_writable = true;
                        break;
                    }
                }
            }

            if (!any_writable)
                continue;               /* every leaf under it is read-only: share it */

            /* It does hold something writable, so the entries below are about to be
             * rewritten -- which means this directory cannot be shared either.
             *
             * Rewriting it in the parent's copy is the mistake this replaces: the PD entry
             * is one word, and the two processes share the *page directory pointer table*
             * only because nothing had needed to differ yet.  Point the parent's entry at
             * the child's new leaf table and the parent loses its own writable one, adopts
             * the child's read-only copy, has no copy-on-write list to consult, and faults
             * on the first thing it writes -- with no way for the kernel to tell that fault
             * from any other.  That is exactly how this was found. */
            uint64_t pd_phys = pmm_alloc_phys_page();

            if (pd_phys == 0)
                return -1;

            mid_v = (uint64_t *)phys_to_virt(pd_phys);
            memcpy(mid_v, src_mid, page_size);
            pdpt[a] = pte_make(pd_phys, PTE_PRESENT | PTE_WRITE | PTE_USER);

            for (unsigned b = 0; b < PML4_ENTRY_COUNT; b++) {
                uint64_t leaf = mid_v[b];
                uint64_t *pt;
                bool writable_here = false;

                if (!pte_present(leaf) || pte_huge(leaf))
                    continue;

                pt = (uint64_t *)phys_to_virt(pte_phys(leaf));

                for (unsigned c = 0; c < PML4_ENTRY_COUNT; c++) {
                    uint64_t l = pt[c];

                    if (pte_present(l) && !pte_huge(l) && pte_writable(l)) {
                        writable_here = true;
                        break;
                    }
                }

                if (!writable_here)
                    continue;           /* nothing writable under this one: share it */

                uint64_t base = ((uint64_t)i << 39) | ((uint64_t)a << 30) |
                                ((uint64_t)b << 21);

                if (!cow_copy_pt(&mid_v[b], pt, base, cow))
                    return -1;
            }
        }

        dst->pml4_v[i] = pte_make(pdpt_phys, PTE_PRESENT | PTE_WRITE | PTE_USER);
    }

    if (vmm_map_identity_for_new_space(dst) != 0)
        return -1;

    return 0;
}

/* Resolve a write fault on a page this address space shares with its twin.
 *
 * Returns true if the fault was ours to handle, in which case the leaf now points at a
 * private copy and the faulting instruction can simply be re-executed.  False means it was
 * not a copy-on-write fault: the address is not in the list, or it is not one of the pages
 * this address space gave up, and the caller should report it as what it is. */
bool vmm_cow_fault(page_table_t *tables, cow_list_t *cow, uint64_t virt) {
    if (tables == NULL || cow == NULL)
        return false;

    /* Align first.  CR2 is the exact byte the instruction touched -- a store four bytes
     * into a page reports an address four bytes into the page -- while the list holds
     * pages, and the loop below compares them.  Without this the search simply never
     * matches and every copy-on-write fault is reported as an ordinary page fault, which
     * is what a forked child writing its first byte sees. */
    virt &= ~(uint64_t)(page_size - 1);

    for (unsigned i = 0; i < cow->count; i++) {
        if (cow->page[i].virt != virt)
            continue;

        uint64_t old_leaf = pt_get(tables, virt);
        uint64_t new_phys = alloc_zeroed_phys();

        if (new_phys == 0)
            return false;

        /* Copy through the direct map, not by mapping the frame into this address space:
         * the frame is the parent's, it is writable in the kernel's tables, and mapping it
         * in only to unmap it again would make the common case of a fault two operations
         * that both need a page of tables. */
        uint64_t old_phys = pte_phys(old_leaf);

        if (old_phys != 0 && old_phys != new_phys) {
            memcpy((void *)phys_to_virt(new_phys), (const void *)phys_to_virt(old_phys),
                   page_size);
        }

        if (!pt_replace_user_leaf(tables, virt, new_phys, cow->page[i].entry))
            return false;

        return true;
    }

    return false;
}
