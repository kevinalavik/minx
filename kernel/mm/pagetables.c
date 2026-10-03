#include "pagetables.h"

#include "cpu.h"
#include "io.h"
#include "kprintf.h"
#include "pmm.h"
#include "string.h"
#include "types.h"

uint64_t pt_read_cr3(void) {
    uint64_t v;
    __asm__ volatile("movq %%cr3, %0" : "=r"(v));
    return v;
}

void pt_write_cr3(uint64_t cr3) {
    __asm__ volatile("movq %0, %%cr3" ::"r"(cr3) : "memory");
}

uint64_t pt_read_cr4(void) {
    uint64_t v;
    __asm__ volatile("movq %%cr4, %0" : "=r"(v));
    return v;
}

void pt_write_cr4(uint64_t cr4) {
    __asm__ volatile("movq %0, %%cr4" ::"r"(cr4) : "memory");
}

uint64_t pt_read_efer(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000080));
    return ((uint64_t)hi << 32) | lo;
}

void pt_enable_nx(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000080));
    uint32_t new_lo = lo | (1u << 11);   /* EFER.NXE */
    __asm__ volatile("wrmsr" ::"a"(new_lo), "d"(hi), "c"(0xc0000080) : "memory");
}

void pt_flush_tlb(void) {
    uint64_t cr3 = pt_read_cr3();
    __asm__ volatile("movq %0, %%cr3" ::"r"(cr3) : "memory");
}

void pt_invalidate(uint64_t virt) {
    /* INVLPG takes the linear address; it deliberately does not flush the
     * whole TLB, because it can be called from inside the page fault handler. */
    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

static inline pte_t *entry_at(uint64_t table_phys, unsigned index) {
    return (pte_t *)(phys_to_virt(table_phys) + index * sizeof(pte_t));
}

/* Physical address of the table `levels` above a leaf: 1 = PDPT, 2 = PD,
 * 3 = PML4. */
uint64_t pt_parent_table(page_table_t *tables, uint64_t virt, int levels,
                         bool create) {
    uint64_t table = tables->pml4;
    static const unsigned shifts[3] = { 39, 30, 21 };

    for (int level = 0; level < levels; level++) {
        unsigned index = (unsigned)((virt >> shifts[level]) & 0x1ff);
        pte_t *slot = entry_at(table, index);

        if (!pte_present(*slot)) {
            if (!create)
                return 0;

            uint64_t frame = pmm_alloc_phys_page();
            if (frame == 0)
                return 0;

            /* Zeroing is not optional: a stale bit in an unused entry is a
             * live mapping as far as the MMU is concerned. */
            memset((void *)phys_to_virt(frame), 0, page_size);
            *slot = pte_make(frame, PTE_PRESENT | PTE_WRITE);
            pt_invalidate(virt);
        }

        table = pte_phys(*slot);
    }

    return table;
}

/* Walk down to the leaf entry without creating anything.
 *
 * Three levels in the loop, then the page table read separately.  Folding the
 * fourth level into the loop and re-reading the leaf afterwards looks equivalent
 * but is not: the loop body already consumed the leaf entry to advance `table`,
 * so the extra read needs the *page table's* physical address while what is in
 * hand is the leaf's.  Keeping the leaf read outside the loop makes the
 * distinction explicit. */
pte_t pt_get(page_table_t *tables, uint64_t virt) {
    uint64_t table = tables->pml4;
    static const unsigned shifts[3] = { 39, 30, 21 };

    for (int level = 0; level < 3; level++) {
        unsigned index = (unsigned)((virt >> shifts[level]) & 0x1ff);
        pte_t entry = *entry_at(table, index);

        if (!pte_present(entry))
            return 0;

        /* A huge page terminates the walk early; its address has to be masked
         * with the in-page offset, which callers of pt_get (a diagnostic helper)
         * do themselves rather than through this raw entry. */
        if (pte_huge(entry))
            return entry;

        table = pte_phys(entry);
    }

    return *entry_at(table, PT_INDEX(virt));
}

/* Walk down to the leaf entry, allocating tables as needed.
 *
 * The kernel is linked at 0xffffffff80000000 and user space lives below 4 GiB,
 * which is PML4 slot 0 against the kernel's slot 511: they never share a
 * top-level entry.  So a user mapping never has to widen an existing kernel
 * page directory, and a kernel-only table reached while mapping a user page is a
 * layout mistake worth reporting rather than papering over. */
static pte_t *walk_to_leaf(page_table_t *tables, uint64_t virt, bool user,
                           bool create) {
    uint64_t table = tables->pml4;
    static const unsigned shifts[3] = { 39, 30, 21 };

    for (int level = 0; level < 3; level++) {
        unsigned index = (unsigned)((virt >> shifts[level]) & 0x1ff);
        pte_t *slot = entry_at(table, index);

        if (!pte_present(*slot)) {
            if (!create)
                return NULL;

            uint64_t frame = pmm_alloc_phys_page();
            if (frame == 0)
                return NULL;

            memset((void *)phys_to_virt(frame), 0, page_size);

            uint64_t flags = PTE_PRESENT | PTE_WRITE;
            if (user)
                flags |= PTE_USER;
            *slot = pte_make(frame, flags);
            pt_invalidate(virt);
        } else if (user && !pte_user(*slot)) {
            return NULL;
        }

        table = pte_phys(*slot);
    }

    return entry_at(table, PT_INDEX(virt));
}

pte_t pt_get_create(page_table_t *tables, uint64_t virt, bool user) {
    pte_t *leaf = walk_to_leaf(tables, virt, user, true);
    return leaf != NULL ? *leaf : 0;
}

bool pt_map_page(page_table_t *tables, uint64_t virt, uint64_t phys,
                 uint64_t flags) {
    bool user = (flags & PTE_USER) != 0;
    pte_t *leaf = walk_to_leaf(tables, virt, user, true);
    if (leaf == NULL)
        return false;

    pte_t entry = pte_make(phys, flags);
    pte_t old = *leaf;

    /* Refuse to silently replace a different mapping: that is how a bug turns
     * into a machine that reads the wrong memory for the rest of its life. */
    if (pte_present(old) && pte_phys(old) != pte_phys(entry))
        return false;

    *leaf = entry;
    pt_invalidate(virt);
    return true;
}

bool pt_map_range(page_table_t *tables, uint64_t virt, uint64_t phys,
                  size_t bytes, uint64_t flags) {
    if (!IS_ALIGNED(virt, page_size) || !IS_ALIGNED(phys, page_size))
        return false;

    for (size_t off = 0; off < bytes; off += page_size) {
        if (!pt_map_page(tables, virt + off, phys + off, flags))
            return false;
    }
    return true;
}

/* Replace a 2 MiB page with 512 4 KiB pages, which is what user space reaching
 * a large kernel-owned mapping requires. */
bool pt_split_2mib(page_table_t *tables, uint64_t virt, uint64_t phys2mib,
                   uint64_t flags) {
    uint64_t pdt = pt_parent_table(tables, virt, 2, true);
    if (pdt == 0)
        return false;

    pte_t *slot = entry_at(pdt, PD_INDEX(virt));
    if (!pte_present(*slot) || !pte_huge(*slot))
        return false;
    if (pte_phys(*slot) != pte_phys(phys2mib))
        return false;

    uint64_t frame = pmm_alloc_phys_page();
    if (frame == 0)
        return false;

    void *table = (void *)phys_to_virt(frame);
    memset(table, 0, page_size);

    /* The huge entry's own flags carry over, minus the huge bit. */
    uint64_t leaf_flags = (*slot & ~PTE_HUGE) & ~PTE_ADDR_MASK;
    leaf_flags |= flags & PTE_USER;
    for (unsigned i = 0; i < PT_ENTRY_COUNT; i++)
        ((pte_t *)table)[i] = pte_make(phys2mib + i * page_size, leaf_flags);

    *slot = pte_make(frame, (*slot & ~PTE_HUGE) & ~PTE_ADDR_MASK);
    pt_invalidate(virt);
    return true;
}

bool pt_map_2mib(page_table_t *tables, uint64_t virt, uint64_t phys,
                 size_t bytes, uint64_t flags) {
    uint64_t pdt = pt_parent_table(tables, virt, 2, true);
    if (pdt == 0)
        return false;

    for (size_t off = 0; off < bytes; off += 0x200000ull) {
        pte_t *slot = entry_at(pdt, PD_INDEX(virt + off));
        pte_t entry = pte_make(phys + off, flags | PTE_HUGE);

        if (pte_present(*slot) &&
            (pte_phys(*slot) != pte_phys(entry) || !pte_huge(*slot)))
            return false;

        *slot = entry;
    }
    pt_invalidate(virt);
    return true;
}

bool pt_map_1gib(page_table_t *tables, uint64_t virt, uint64_t phys,
                 size_t bytes, uint64_t flags) {
    uint64_t pdpt = pt_parent_table(tables, virt, 1, true);
    if (pdpt == 0)
        return false;

    for (size_t off = 0; off < bytes; off += 0x40000000ull) {
        unsigned index = PDPT_INDEX(virt + off);
        pte_t *slot = entry_at(pdpt, index);
        pte_t entry = pte_make(phys + off, flags | PTE_HUGE);

        if (pte_present(*slot) && pte_phys(*slot) != pte_phys(entry))
            return false;

        *slot = entry;
    }
    pt_invalidate(virt);
    return true;
}

bool pt_unmap_page(page_table_t *tables, uint64_t virt) {
    pte_t *leaf = walk_to_leaf(tables, virt, false, false);
    if (leaf == NULL || !pte_present(*leaf))
        return false;

    *leaf = 0;
    pt_invalidate(virt);
    return true;
}

bool pt_set_user(page_table_t *tables, uint64_t virt, bool user) {
    pte_t *leaf = walk_to_leaf(tables, virt, user, true);
    if (leaf == NULL)
        return false;
    if (user)
        *leaf |= PTE_USER;
    else
        *leaf &= ~PTE_USER;
    pt_invalidate(virt);
    return true;
}

bool pt_set_writable(page_table_t *tables, uint64_t virt, bool writable) {
    pte_t *leaf = walk_to_leaf(tables, virt, false, false);
    if (leaf == NULL || !pte_present(*leaf))
        return false;
    if (writable)
        *leaf |= PTE_WRITE;
    else
        *leaf &= ~PTE_WRITE;
    pt_invalidate(virt);
    return true;
}

bool pt_clear_nx(page_table_t *tables, uint64_t virt) {
    pte_t *leaf = walk_to_leaf(tables, virt, false, false);
    if (leaf == NULL || !pte_present(*leaf))
        return false;
    *leaf &= ~PTE_NX;
    pt_invalidate(virt);
    return true;
}

bool pt_set_nx(page_table_t *tables, uint64_t virt, bool nx) {
    pte_t *leaf = walk_to_leaf(tables, virt, false, false);
    if (leaf == NULL || !pte_present(*leaf))
        return false;
    if (nx)
        *leaf |= PTE_NX;
    else
        *leaf &= ~PTE_NX;
    pt_invalidate(virt);
    return true;
}
