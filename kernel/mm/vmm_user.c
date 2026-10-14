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

/* Give the child its own copy of every page under one leaf page table.
 *
 * This used to *mark* the writable leaves copy-on-write and record them, so that the first
 * write from either process would fault and be answered with a private copy.  That is the
 * textbook design and it is what the rest of this file is still built around -- `PTE_COW`,
 * `vmm_cow_fault()`, and the syscall door's `syscall_prepare_write()` all exist for it.
 *
 * It cannot work here.  The fault it depends on is a *write* fault on a page marked
 * read-only, and a write fault only happens if `CR4.WP` is set: with it clear the processor
 * lets a store to a read-only user page go straight through to the frame, silently, with no
 * trap at all.  And `CR4.WP` cannot be set on the emulator this kernel is built to run on --
 * QEMU treats bit 16 of CR4 as reserved and raises #GP for `mov %cr4` with it set, so
 * `vmm_init()` would panic before printing a line.  Measured on QEMU 11.1.2, on this
 * project's test image, with both `-accel kvm` and `-accel tcg,thread=multi`, and again
 * with `-cpu max`; the register form `mov %rdi, %cr4` is what faults, so it is not the
 * immediate form's restricted bit set.  `CR4_WP` is kept in `pagetables.h` with the
 * measurement, so the bit stays named rather than rediscovered.
 *
 * So the copying happens *here*, at the fork, instead of lazily at the first write.  That
 * costs a copy of every page of a process that forks even if it never writes to one, which
 * is the whole of the cost: memory proportional to the parent's address space times the
 * number of live children, all of it freed when they exit.
 *
 * What it buys is that after a fork the two address spaces share *no* user page at all, so
 * there is nothing for a write to go straight into and nothing to be detected late.  That
 * is not a tidier way to do copy-on-write; it is the only way to do it where a write to a
 * read-only page does not trap, and the difference is worth a day's worth of symptoms:
 *
 *   With marking, `shtest` forked, and the child resumed correctly -- `fork` returned zero,
 *   the frame said the right thing, the child branched on `pid == 0`.  But the child's next
 *   instruction was a `ret`, which reads the *return address* out of a stack page the
 *   parent still held.  The parent had already returned from its own `fork`, called
 *   `getpid`, called `trace`, and pushed two return addresses over the slot the child was
 *   about to read.  So the child popped the parent's live return address, landed past the
 *   `if (pid == 0)`, took the parent's branch, and called `waitpid()` on a `pid` read out
 *   of an argument slot.  `fork` returned non-zero in the child, intermittently, and no
 *   report anywhere said a page was shared.
 *
 *   Every one of those steps is something COW would have caught on the parent's first
 *   write to that page -- if a write to it trapped.
 *
 * Read-only pages are copied too, rather than shared on the grounds that "nothing can write
 * them, so nothing can notice".  Nothing stops a program writing to its own text, and a
 * write that went into the twin's would be the same class of bug as this one: silent
 * corruption of a process by its own sibling.  Copying them is the difference between "the
 * two share nothing" and "the two share nothing that anybody has tried to write yet".
 *
 * The budget is `COW_MAX_PAGES`, counted in `cow->copied` rather than in `cow->count`: the
 * list itself is now empty, because nothing is given up, and the accounting in
 * `task_attach_cow_twin()` -- which seeds the child with the parent's list and adds back
 * only what this fork took -- stays correct with nothing to add.
 *
 * `src_v` is the parent's table and is never written to.  That is the other half of why the
 * marking version had to clear both sides: there was one physical frame under two leaves
 * and one of the two bits had to change somewhere.  Here there is one frame per leaf and
 * nothing to change. */
static bool fork_copy_page(uint64_t *slot, uint64_t *src_v, uint64_t base,
                           cow_list_t *cow) {
    uint64_t frame = pmm_alloc_phys_page();

    if (frame == 0)
        return false;

    uint64_t *dst = (uint64_t *)phys_to_virt(frame);

    memcpy(dst, src_v, page_size);

    for (unsigned i = 0; i < PML4_ENTRY_COUNT; i++) {
        uint64_t e = dst[i];

        if (!pte_present(e) || pte_huge(e))
            continue;

        /* Not the program's.  A supervisor-only mapping in the user half of the address
         * space is the kernel's own, and there is exactly one of them: the TSS, identity
         * mapped at the linear address equal to its physical address so that QEMU can
         * resolve the fetch through the page tables.  "Its own physical address" is the
         * whole of what it is for -- a copy at a different frame is not a copy the processor
         * will ever reach, and `vmm_map_identity_for_new_space()`, which runs just after
         * this and puts the identity mapping into every new address space, then finds a
         * different mapping already there and refuses.
         *
         * The refusal was reported as "could not map <the TSS's address> at its own
         * physical address", which names the innocent party: the address is the TSS's and
         * the fault is that the fork moved it.  Sharing it is not a compromise, it is the
         * requirement -- it is one page, it is the same page in every address space by
         * design, and there is nothing a process can do with it.
         *
         * A leaf the *kernel* maps at a low address with user access is a different thing
         * entirely and is copied like any other; this test is on the user bit, not on where
         * the address is. */
        if (!pte_user(e))
            continue;

        if (cow->copied >= COW_MAX_PAGES) {
            cow->full = true;
            return false;
        }

        /* A fresh frame with the parent's contents, and the parent's own flags kept exactly
         * as they are: data stays writable and text stays read-only, because a `fork` is
         * not a licence to make the program's text writable. */
        uint64_t copy = pmm_alloc_phys_page();

        if (copy == 0)
            return false;

        memcpy((void *)phys_to_virt(copy), (const void *)phys_to_virt(pte_phys(e)),
               page_size);

        /* A page marked copy-on-write belongs to an address space that has given it up; the
         * child has not been given it by this fork and must not inherit the mark, which
         * would claim a record of a give-up that never happened and make a later
         * `vmm_cow_fault()` copy a page that is already private. */
        dst[i] = pte_make(copy, e & ~PTE_COW);
        cow->copied++;
    }

    (void)base;                        /* only ever used for the recording above */

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

/* Duplicate a directory or leaf table for the twin without touching what is in it.
 *
 * For the tables that hold nothing a copy-on-write fault can happen on -- a program's text,
 * a shared read-only mapping -- but that still *can be added to*.  `brk()` adds a mapping
 * from ring 3, so a leaf the twin might map into must not be the one the parent also uses:
 * the new entry lands in the shared table, both processes see it, and the parent's next
 * `brk` replaces the twin's page with its own.  That is not a copy-on-write bug; it is two
 * address spaces sharing one mapping because a table was shared when it should not have
 * been, and the symptom is a child whose heap is silently the parent's.
 *
 * The cost is one page per table that has something in it, which for a small program is
 * under ten pages for the whole `fork`. */
static bool pt_copy_table(uint64_t *slot, const uint64_t *src_v) {
    uint64_t frame = pmm_alloc_phys_page();

    if (frame == 0)
        return false;

    memcpy((uint64_t *)phys_to_virt(frame), src_v, page_size);
    *slot = pte_make(frame, PTE_PRESENT | PTE_WRITE | PTE_USER);
    return true;
}

/* Build `dst` as a twin of `src` that shares no user page with it.
 *
 * A twin that shares nothing is what `fork_copy_page()` below does, and why; this is the
 * walk that gets there.  The textbook version shares every page and takes write permission
 * away from both sides, so that the first write faults and is answered with a private copy.
 * That needs `CR4.WP`, this kernel cannot have it, and the write never faults -- so the
 * page is shared for the rest of both processes' lives instead.  `fork_copy_page()` has the
 * full account and the symptoms it produced.
 *
 * Duplication is at the level of the leaf page table and above, never above the leaf.
 *
 *   A leaf has to be private: the two processes must be able to point it at different
 *   frames, which is the whole purpose.
 *
 *   A page directory has to be private for a different reason, and it is the one that is
 *   easy to miss.  `brk()` adds an entry to whichever table covers the address it is
 *   handing out, from ring 3, in whichever address space asked.  A shared directory gains
 *   the twin's mapping, the parent sees it, and the parent's own `brk` replaces the frame
 *   under it -- at which point the child's heap and the parent's heap are the same page and
 *   neither of them faults, because both are entitled to write it.  "Can be shared" used to
 *   mean "nothing under it is writable, so nothing in it can ever change", which is true of
 *   the *entries* and false of the *table*.  What that looked like was `$( $( ) )` in the
 *   shell: the outer capture forked, the inner one wrote its word, and the machine stopped
 *   with a protection violation on a heap page in the middle of a line that had already
 *   worked once.
 *
 *   A page directory pointer table can be shared when there is nothing at all under it.
 *
 * The copy budget is `cow->copied`, against `COW_MAX_PAGES`.  It is a budget and not a
 * record: nothing is given up -- the child has its own pages and the parent keeps its own
 * -- so `cow->count` is left exactly as `task_attach_cow_twin()` seeded it. */
int vmm_fork_aspace(page_table_t *dst, page_table_t *src, cow_list_t *cow) {
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

            /* Is there anything at all under this directory?  If not, there is nothing to
             * copy and nothing to disagree about, and it stays shared. */
            for (unsigned b = 0; b < PML4_ENTRY_COUNT && !any_writable; b++) {
                if (pte_present(src_mid[b]))
                    any_writable = true;
            }

            if (!any_writable)
                continue;               /* nothing under it: share it, and nothing can add */

            /* It holds something, so the entries below are about to be rewritten -- which
             * means this directory cannot be shared either.
             *
             * "Can be shared" used to mean "nothing under it is writable, so nothing in it
             * can ever change".  That is true of the *entries* and false of the *table*:
             * `brk()` adds an entry to whichever table covers the address it is handing out,
             * from ring 3, in whichever address space asked.  A shared table therefore gains
             * the twin's mapping, the parent sees it, and the parent's own `brk` replaces the
             * frame under it -- at which point the child's heap and the parent's heap are the
             * same page and neither of them faults, because both are entitled to write it.
             *
             * A directory the twin may add to has to be one of its own, whatever is in it.
             * This was found by a shell whose two nested `$( )` children corrupted its
             * allocator's free list, which is a long way from "two processes sharing a page
             * directory" and was read as a memory bug in the shell for several days. */
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

                    if (pte_present(l) && !pte_huge(l) && (pte_writable(l) || pte_cow(l))) {
                        writable_here = true;
                        break;
                    }
                }

                if (!writable_here) {
                    /* Nothing here can be written by either process -- but the twin's
                     * `brk()` can still add an entry, so the table is copied rather than
                     * shared.  See the directory above for why that is not optional. */
                    if (!pt_copy_table(&mid_v[b], pt))
                        return -1;

                    continue;
                }

                uint64_t base = ((uint64_t)i << 39) | ((uint64_t)a << 30) |
                                ((uint64_t)b << 21);

                if (!fork_copy_page(&mid_v[b], pt, base, cow))
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

        /* The leaf is this task's alone now, and writable, so the page is no longer one that was
         * given up -- and saying so is what lets a *later* fork share its table again, which
         * is the other half of why the bit exists.  The twin keeps the bit on its own side,
         * which is right: the twin has still given the page up. */
        if (!pt_replace_user_leaf(tables, virt, new_phys,
                                  (cow->page[i].entry & ~PTE_COW) | PTE_WRITE))
            return false;

        return true;
    }

    return false;
}
