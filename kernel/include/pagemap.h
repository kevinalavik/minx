/* minx - read-only inspection of the active page tables. */
#ifndef MINX_PAGEMAP_H
#define MINX_PAGEMAP_H

#include <stdbool.h>
#include <stdint.h>

/* Physical address the linear address maps to, or 0 when unmapped. */
uint64_t vmm_translate(uint64_t virt, bool *writable);

bool vmm_is_mapped(uint64_t virt);
bool vmm_is_writable(uint64_t virt);

/* Find a usable linear address for a physical one, preferring the higher-half
 * direct map and falling back to the identity map.  False when neither is
 * mapped, which is how MMIO such as the local APIC looks before the kernel owns
 * its own page tables. */
bool vmm_find_mapping(uint64_t phys, uint64_t *virt_out);

#endif /* MINX_PAGEMAP_H */