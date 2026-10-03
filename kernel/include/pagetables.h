/* minx - raw x86-64 page table manipulation.
 *
 * Only the bits this kernel uses, and only the level-N accessors.  Every
 * address here is a *physical* address of a table, plus the HHDM linear address
 * it can be read through; the table walk in pagemap.c works the same way and
 * the two must agree on the bit layout.
 */
#ifndef MINX_PAGETABLES_H
#define MINX_PAGETABLES_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

#define PML4_INDEX(virt) (((uint64_t)(virt) >> 39) & 0x1ff)
#define PDPT_INDEX(virt) (((uint64_t)(virt) >> 30) & 0x1ff)
#define PD_INDEX(virt)   (((uint64_t)(virt) >> 21) & 0x1ff)
#define PT_INDEX(virt)   (((uint64_t)(virt) >> 12) & 0x1ff)

#define PML4_ENTRY_COUNT 512
#define PDPT_ENTRY_COUNT 512
#define PD_ENTRY_COUNT   512
#define PT_ENTRY_COUNT   512

typedef uint64_t pte_t;

/* Entry bits. */
#define PTE_PRESENT     (1ull << 0)
#define PTE_WRITE       (1ull << 1)
#define PTE_USER        (1ull << 2)
#define PTE_WRITETHRU   (1ull << 3)
#define PTE_CACHE_MASK  (3ull << 6)
#define PTE_GLOBAL      (1ull << 8)
#define PTE_HUGE        (1ull << 7)
#define PTE_NX          (1ull << 63)

#define PTE_ADDR_MASK   0x000ffffffffff000ull

/* Page sizes the hardware encodes in the PS bit, and so the in-page masks a
 * translation has to add back for a leaf that is not a 4 KiB table entry. */
#define HUGE_2MIB      0x200000ull
#define HUGE_1GIB      0x40000000ull

#define PTE_FLAGS_MASK  0x707ff00000000000ull | PTE_NX | PTE_USER

/* Cacheability shorthands, selecting one of the four PAT encodings. */
#define PTE_CACHE_WB    (0ull << 6)   /* write-back: ordinary RAM       */
#define PTE_CACHE_WT    (1ull << 6)   /* write-through                  */
#define PTE_CACHE_UC    (2ull << 6)   /* uncacheable: MMIO              */
#define PTE_CACHE_WC    (3ull << 6)   /* write-combining: framebuffer   */

/* Compose an entry for a physical frame with the given flags. */
static inline pte_t pte_make(uint64_t phys, uint64_t flags) {
    return (phys & PTE_ADDR_MASK) | (flags & ~PTE_ADDR_MASK);
}

static inline bool pte_present(pte_t e)    { return (e & PTE_PRESENT) != 0; }
static inline bool pte_writable(pte_t e)   { return (e & PTE_WRITE) != 0; }
static inline bool pte_user(pte_t e)       { return (e & PTE_USER) != 0; }
static inline bool pte_huge(pte_t e)       { return (e & PTE_HUGE) != 0; }
static inline bool pte_nx(pte_t e)         { return (e & PTE_NX) != 0; }
static inline uint64_t pte_phys(pte_t e)   { return e & PTE_ADDR_MASK; }

/* Flags that a new mapping gets by default: present, writable, no user access,
 * cacheable write-back, and executable only if not NX. */
#define PTE_DEFAULT_FLAGS (PTE_PRESENT | PTE_WRITE | PTE_CACHE_WB)

/* The four levels, as opaque physical addresses. */
typedef struct {
    uint64_t pml4;      /* physical address of the top level          */
    uint64_t *pml4_v;   /* HHDM linear address, for direct indexing    */
    uint64_t *pdpt_v;
} page_table_t;

/* Control registers. */
uint64_t pt_read_cr3(void);
void     pt_write_cr3(uint64_t cr3);
uint64_t pt_read_cr4(void);
void     pt_write_cr4(uint64_t cr4);
uint64_t pt_read_efer(void);
void     pt_enable_nx(void);

#define CR4_PAE       (1ull << 5)
#define CR4_SMEP      (1ull << 7)
#define CR4_SMAP      (1ull << 20)

#define EFER_NXE      (1ull << 11)

/* Invalidate one linear address, or everything. */
void pt_flush_tlb(void);
void pt_invalidate(uint64_t virt);

/* Walk and create.  `create` selects whether missing tables are allocated (which
 * needs a page allocator) or reported as absent. */
pte_t pt_get(page_table_t *tables, uint64_t virt);
pte_t pt_get_create(page_table_t *tables, uint64_t virt, bool user);

/* Physical address of the table `levels` above a leaf: 1 = PDPT, 2 = PD,
 * 3 = PML4.  0 when it does not exist and `create` is false, or when the
 * allocator is exhausted. */
uint64_t pt_parent_table(page_table_t *tables, uint64_t virt, int levels,
                         bool create);

/* Replace an existing 2 MiB mapping with 512 4 KiB pages. */
bool pt_split_2mib(page_table_t *tables, uint64_t virt, uint64_t phys2mib,
                   uint64_t flags);

/* Map a single 4 KiB page.  Returns false if an incompatible mapping exists. */
bool pt_map_page(page_table_t *tables, uint64_t virt, uint64_t phys,
                 uint64_t flags);

/* Map a range with the same flags on every page. */
bool pt_map_range(page_table_t *tables, uint64_t virt, uint64_t phys,
                  size_t bytes, uint64_t flags);

/* Huge mappings: 2 MiB pages at PD level and 1 GiB pages at the PDPT level.
 * Both require the caller to have reserved whole aligned regions. */
bool pt_map_2mib(page_table_t *tables, uint64_t virt, uint64_t phys,
                 size_t bytes, uint64_t flags);
bool pt_map_1gib(page_table_t *tables, uint64_t virt, uint64_t phys,
                 size_t bytes, uint64_t flags);

bool pt_unmap_page(page_table_t *tables, uint64_t virt);

/* Permissions, for the page-fault handler's "fix it up" case. */
bool pt_set_user(page_table_t *tables, uint64_t virt, bool user);
bool pt_set_writable(page_table_t *tables, uint64_t virt, bool writable);
bool pt_clear_nx(page_table_t *tables, uint64_t virt);
bool pt_set_nx(page_table_t *tables, uint64_t virt, bool nx);

#endif /* MINX_PAGETABLES_H */
