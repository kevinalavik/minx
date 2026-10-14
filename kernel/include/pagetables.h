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
#define PTE_COW         (1ull << 9)
#define PTE_HUGE        (1ull << 7)
#define PTE_NX          (1ull << 63)

/* `PTE_COW` is ours, not the processor's: bit 9 is one of the three the architecture leaves
 * available to the operating system.  It says "this page is read-only because a `fork` gave
 * its write permission up, and a task's copy-on-write list has an entry for it".
 *
 * It is not decoration.  Read-only on its own cannot tell the two cases apart: a page that
 * was never mapped writable (the text of a program, a shared read-only mapping) may be
 * shared with a twin outright, while a page that is read-only *because of a fork* must have a
 * private page table under it, or the copy that fixes one process up rewrites the other's
 * mapping and the two end up writing to the same page -- which is the one thing copy-on-write
 * exists to prevent.
 *
 * Without the bit, `vmm_fork_aspace()` duplicated a table for the second kind and shared it
 * for the first, which is right.  With the second kind's pages arriving at the second fork
 * already read-only, the second `fork` shared their table with the child, the child took a
 * copy-on-write fault and wrote its replacement into the shared table, and the parent --
 * whose entry now pointed at the child's private page and was writable -- wrote there too.
 * The shell's two nested `$( )` children did exactly that, and the parent's allocator
 * followed the list into the middle of its own text segment. */

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
static inline bool pte_cow(pte_t e)        { return (e & PTE_COW) != 0; }
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

/* CR4.WP, bit 16: "supervisor-mode writes are not allowed to write to read-only user
 * pages".  Architecturally it is exactly what this kernel's copy-on-write needs -- with
 * it clear, a ring-0 store to a page a `fork` gave up goes straight through into the frame
 * the twin shares, which is how a child used to return from `fork()` holding the *parent's*
 * pid.
 *
 * It is deliberately NOT set, because QEMU refuses the write with #GP: the accelerator
 * models PAE and SMEP but not WP, so `mov cr4` with bit 16 raises a general protection
 * fault and takes the machine down during `vmm_init()`.  Measured on QEMU 11.1.2 with both
 * `-accel kvm` and `-accel tcg,thread=multi`, on this project's test image.
 *
 * So the property is established the other way instead: every write the *kernel* makes
 * into a user page is preceded by `syscall_prepare_write()` (see kernel/lib/syscall.c),
 * which gives the page its own copy first.  `CR4_WP` is kept here so the bit is named
 * rather than rediscovered. */
#define CR4_WP        (1ull << 16)

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
/* Overwrite the leaf at `virt` outright, replacing whatever mapping is there.
 *
 * pt_map_page() deliberately refuses to replace a different frame, because a mapping
 * silently changing under a running program is how a bug becomes permanent.  Copy-on-write
 * is the one case where that is the whole point: the leaf *must* change from the parent's
 * frame to a private copy, and refusing is what stops it happening.  So the intent is
 * explicit in the name.
 *
 * Fails if `virt` is not present, or not user-accessible, or not a leaf rather than a
 * huge page. */
bool pt_replace_user_leaf(page_table_t *tables, uint64_t virt, uint64_t phys,
                          uint64_t flags);

bool pt_map_page(page_table_t *tables, uint64_t virt, uint64_t phys,
                 uint64_t flags);

/* Map a range with the same flags on every page. */
/* As pt_map_range(), with the intermediate tables marked user-accessible and the leaves
 * left supervisor-only: reachable by the walk, refused at the last step.
 *
 * For a mapping the processor needs by physical address -- the TSS -- which has to sit
 * at the linear address equal to its physical address.  See vmm_map_identity(). */
bool pt_map_supervised_range(page_table_t *tables, uint64_t virt, uint64_t phys,
                             size_t bytes, uint64_t flags);

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
