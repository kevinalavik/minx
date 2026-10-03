/* minx - the higher-half direct map offset.
 *
 * phys_to_virt() is used everywhere, from the physical allocator to the page
 * table walk, and every one of those uses is only correct if the offset matches
 * the one the bootloader actually mapped.  Limine randomises it, so the kernel
 * asks for it through the protocol instead of assuming a fixed window.
 *
 * The variable is a plain `uint64_t` rather than a constant so it can be filled
 * in before anything else runs: kmain() calls hhdm_init() first, and the value
 * is only ever read afterwards.
 */
#include "hhdm.h"

#include "kprintf.h"
#include "types.h"

uint64_t hhdm_offset;

/* Used until the bootloader answers, and if it cannot: the window the kernel
 * itself will map once it owns its page tables.  Keeping it as the fallback means
 * a machine whose bootloader says nothing still gets a usable direct map. */
#define HHDM_DEFAULT 0xffff800000000000ULL

void hhdm_set(uint64_t offset) {
    hhdm_offset = offset;
}

void hhdm_init(void) {
    hhdm_set(HHDM_DEFAULT);

    const struct limine_hhdm_response *hhdm = limine_hhdm_request.response;

    if (hhdm == NULL) {
        kprintf("[warn] hhdm: the bootloader did not report an offset; "
                "assuming %#lx\n", (unsigned long)hhdm_offset);
        return;
    }

    hhdm_set(hhdm->offset);

    kprintf("hhdm: direct map offset %#lx (revision %lu)\n",
            (unsigned long)hhdm->offset, (unsigned long)hhdm->revision);
}