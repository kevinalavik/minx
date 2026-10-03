/* minx - read-only inspection of the active page tables.
 *
 * Two jobs: find out whether a linear address is actually mapped before
 * dereferencing it (the higher-half direct map deliberately excludes MMIO, so
 * the local APIC is not reachable through it until the kernel builds its own
 * tables in phase 3), and later decide whether a user pointer is worth copying.
 */
#include "pagemap.h"

#include "cpu.h"
#include "io.h"
#include "types.h"

#define PTE_PRESENT (1u << 0)
#define PTE_WRITE   (1u << 1)
#define PTE_USER    (1u << 2)
#define PTE_WRITETHRU (1u << 3)
#define PTE_HUGE    (1u << 7)

#define ADDR_MASK 0x000ffffffffff000ull
#define ADDR_SHIFT 12

/* Walk the four levels.  Returns the physical address the linear address maps
 * to, or 0 when it is not mapped. */
uint64_t vmm_translate(uint64_t virt, bool *writable) {
    if (writable)
        *writable = false;

    uint64_t table = read_cr3() & ~0xfffull;
    uint64_t addr = virt & ADDR_MASK;

    for (int level = 4; level >= 1; level--) {
        uint64_t shift = (uint64_t)(level - 1) * 9 + ADDR_SHIFT;
        uint64_t index = (addr >> shift) & 0x1ff;
        uint64_t *entry = (uint64_t *)(phys_to_virt(table) + index * 8);
        uint64_t value = *entry;

        if ((value & PTE_PRESENT) == 0)
            return 0;

        if (writable)
            *writable = (value & PTE_WRITE) != 0;

        /* A huge page ends the walk early. */
        if ((value & PTE_HUGE) != 0 && level <= 3) {
            uint64_t base = value & 0x000fffffffe00000ull;
            uint64_t mask = level == 3 ? 0x1fffffull : 0x3fffffull;
            return base | (addr & mask);
        }

        table = value & 0x000ffffffffff000ull;
    }

    return table;   /* the leaf's physical frame */
}

bool vmm_is_mapped(uint64_t virt) {
    return vmm_translate(virt, NULL) != 0;
}

bool vmm_is_writable(uint64_t virt) {
    bool writable = false;
    if (vmm_translate(virt, &writable) == 0)
        return false;
    return writable;
}

/* Find any linear address that maps `phys`, given a preference order.  The
 * bootloader's higher-half direct map covers RAM but not MMIO, while its
 * identity map may still cover both, so the caller tries both. */
bool vmm_find_mapping(uint64_t phys, uint64_t *virt_out) {
    /* The candidates depend on `phys`, so they cannot be a static table. */
    uint64_t candidates[2];
    candidates[0] = phys_to_virt(phys);   /* higher-half direct map */
    candidates[1] = phys;                 /* identity map          */

    for (size_t i = 0; i < array_len(candidates); i++) {
        if (vmm_is_mapped(candidates[i])) {
            *virt_out = candidates[i];
            return true;
        }
    }
    return false;
}