#include "vmm.h"

#include "cpu.h"
#include "acpi.h"
#include "apic.h"
#include "boot.h"
#include "fb.h"
#include "hhdm.h"
#include "kprintf.h"
#include "pagemap.h"
#include "pmm.h"
#include "task.h"
#include "string.h"
#include "types.h"

/* Device windows to map once the kernel owns its tables.  The APIC windows are
 * the whole point: the bootloader's page tables map RAM but not MMIO, which is
 * why interrupts could not reach the CPU in phase 2. */
#define MAX_MMIO_WINDOWS 16

typedef struct {
    uint64_t virt;
    uint64_t phys;
    size_t   bytes;
    const char *name;
} mmio_window_t;

static struct {
    bool        active;
    page_table_t tables;
    uint64_t    kernel_pml4_phys;

    /* The bootloader's tables are released only once nothing is running on them,
     * which is after the application processors have been handed off. */
    bool        boot_tables_released;

    mmio_window_t mmio[MAX_MMIO_WINDOWS];
    unsigned      mmio_count;

    vmm_stats_t stats;

    /* The virtual address the heap starts at, in the kernel's window and well
     * clear of the image. */
    uint64_t heap_virt_base;
    uint64_t heap_virt_end;

    /* Where the kernel image actually is in physical memory, and where it is mapped.
     * vmm_kernel_phys() needs both: an image address is not a direct-map address, so the
     * usual virt_to_phys() conversion does not apply to one. */
    uint64_t image_virt;
    uint64_t image_phys;
} vmm;

bool vmm_active(void) { return vmm.active; }

/* The kernel's own tables.  Fixed for the life of the kernel: this is what the
 * direct map, the kernel image, the heap and every MMIO window are mapped into, and
 * an application processor's trampoline needs it before there is a task at all. */
page_table_t *vmm_kernel_tables(void) { return &vmm.tables; }

/* The tables in use right now.
 *
 * The current task's own address space once there is one, and the kernel's own
 * before that -- during boot, and on an application processor between arriving and
 * entering the scheduler.  Every mapping, unmapping and walk below goes through this
 * rather than through vmm_kernel_tables(), which is what makes a user mapping land in
 * the address space of the task that asked for it and in no other.
 *
 * A kernel task carries the kernel's tables in its `tables` field, so for every
 * kernel task the two are the same pointer and this is free. */
page_table_t *vmm_tables(void) {
    task_t *self = sched_current();

    if (self != NULL && self->tables != NULL)
        return self->tables;
    return &vmm.tables;
}

/* Translate through the kernel's own tables.  Before vmm_init() there are none,
 * so this falls back to walking CR3, which at that point is the bootloader's. */
uint64_t vmm_translate(uint64_t virt) {
    return vmm_translate_in(vmm_tables(), virt);
}

/* Map [phys, phys+bytes) at the linear address equal to its physical address.
 *
 * `tables` is the space to map into; the caller passes the kernel's own for the first
 * mapping and each new address space for its own copy.  Each space gets *its own*
 * page tables for this, walked from its own PML4, rather than a pointer copied from
 * the kernel's -- otherwise a process mapping anything in the same top-level slot
 * would be editing the kernel's tables, and vmm_address_space_destroy() would free
 * tables the kernel is still using.
 *
 * Not marked PTE_USER: nothing below ring 0 has any business here. */
static int map_identity_into(page_table_t *tables, uint64_t phys, uint64_t bytes) {
    if (!pt_map_supervised_range(tables, phys, phys, bytes,
                                 PTE_PRESENT | PTE_WRITE | PTE_CACHE_WB)) {
        kprintf("[warn] vmm: could not map %#lx..%#lx at its own physical "
                "address\n", (unsigned long)phys,
                (unsigned long)(phys + bytes));
        return -1;
    }

    return 0;
}

/* The one region every address space must reach by physical address.  See vmm.h. */
static uint64_t ident_phys;
static uint64_t ident_bytes;

int vmm_map_identity(uint64_t phys, uint64_t bytes) {
    if (!vmm.active || phys == 0 || bytes == 0)
        return -1;
    phys &= ~(uint64_t)(page_size - 1);
    bytes = ALIGN_UP(bytes, page_size);

    if (ident_bytes != 0) {
        if (ident_phys != phys || ident_bytes != bytes) {
            kprintf("[warn] vmm: the identity-mapped region is already %#lx..%#lx\n",
                    (unsigned long)ident_phys,
                    (unsigned long)(ident_phys + ident_bytes));
            return -1;
        }

        return 0;
    }

    if (map_identity_into(&vmm.tables, phys, bytes) != 0)
        return -1;

    ident_phys = phys;
    ident_bytes = bytes;

    kprintf("vmm: %#lx..%#lx also mapped at its own physical address, so the "
            "processor can reach it by physical address\n",
            (unsigned long)phys, (unsigned long)(phys + bytes));

    return 0;
}

int vmm_map_identity_for_new_space(page_table_t *tables) {
    if (ident_bytes == 0)
        return 0;

    return map_identity_into(tables, ident_phys, ident_bytes);
}

/* The physical address of a kernel-image address.
 *
 * `virt_to_phys()` subtracts the direct-map offset, which is only correct for an address
 * *in* the direct map.  The kernel image is not in the direct map -- it is mapped once, at
 * `__kernel_start`, from whatever physical base the bootloader chose -- so converting an
 * image address that way yields a number with no page behind it.  For 0xffffffff80xxxxxx
 * that number is 0x0000_8000_xxxx_xxxx, which reads like a plausible low physical address
 * and is the reason a TSS descriptor written with it looked fine and did nothing.
 *
 * The image's physical base is *asked for*, not derived: see vmm_init(), which walks the
 * bootloader's tables to find it, because the firmware memory map has no entry covering
 * physical page zero at all.
 */
uint64_t vmm_kernel_phys(uint64_t vaddr) {
    if (!vmm.active || vaddr < vmm.image_virt)
        return 0;

    return vmm.image_phys + (vaddr - vmm.image_virt);
}

/* As vmm_translate(), but against a named address space.
 *
 * Same reason as syscall_user_range_ok_in(): "is this address mapped" has to be asked of
 * the address space the address belongs to, and the running task's is only ever the right
 * one when the running task is the one being asked about. */
uint64_t vmm_translate_in(page_table_t *tables, uint64_t virt) {
    if (!vmm.active)
        return vmm_walk_translate(virt, NULL);

    if (tables == NULL)
        return 0;

    pte_t leaf = pt_get(tables, virt);
    if (leaf == 0)
        return 0;

    /* A leaf entry's address field is the start of whatever it maps, so the
     * offset inside the mapping has to be put back -- and the size of that
     * mapping is what the offset is measured against: a 4 KiB page has 12 bits of
     * it, a 2 MiB page has 21.  Without this every address in the first page of a
     * mapping translates to that page's base, which is right for exactly one
     * address out of every 4096, and the addresses being asked about are never
     * that one. */
    if (pte_huge(leaf))
        return pte_phys(leaf) + (virt & (HUGE_2MIB - 1));

    return pte_phys(leaf) + (virt & (page_size - 1));
}

/* --- MMIO requests ------------------------------------------------------ */

int vmm_map_mmio(uint64_t phys, size_t bytes, const char *name) {
    if (phys == 0 || bytes == 0)
        return -1;
    if (!IS_ALIGNED(phys, page_size))
        return -1;

    /* Round up to whole pages: a window that starts mid-page has to have the
     * whole page, or touching the neighbouring register faults. */
    size_t span = ALIGN_UP(bytes, page_size);

    if (vmm.active) {
        /* Already running: install the window immediately. */
        if (!pt_map_range(&vmm.tables, phys_to_virt(phys), phys, span,
                          PTE_PRESENT | PTE_WRITE | PTE_CACHE_UC))
            return -1;
        vmm.stats.mmio_pages += span / page_size;
        kprintf("vmm: mapped %lu KiB of %s at %#lx\n",
                (unsigned long)(span / 1024), name, (unsigned long)phys);
        return 0;
    }

    if (vmm.mmio_count >= MAX_MMIO_WINDOWS) {
        kprintf("[warn] vmm: no room left for the MMIO window %s\n", name);
        return -1;
    }

    vmm.mmio[vmm.mmio_count].virt = phys_to_virt(phys);
    vmm.mmio[vmm.mmio_count].phys = phys;
    vmm.mmio[vmm.mmio_count].bytes = span;
    vmm.mmio[vmm.mmio_count].name = name;
    vmm.mmio_count++;

    kprintf("vmm: %s at %#lx (%lu KiB) queued for mapping\n", name,
            (unsigned long)phys, (unsigned long)(span / 1024));
    return 0;
}

int vmm_unmap_mmio(uint64_t phys) {
    if (!vmm.active)
        return -1;

    for (unsigned i = 0; i < vmm.mmio_count; i++) {
        if (vmm.mmio[i].phys != phys)
            continue;

        for (size_t off = 0; off < vmm.mmio[i].bytes; off += page_size)
            pt_unmap_page(&vmm.tables, vmm.mmio[i].virt + off);

        vmm.stats.mmio_pages -= vmm.mmio[i].bytes / page_size;
        vmm.mmio[i] = vmm.mmio[vmm.mmio_count - 1];
        vmm.mmio_count--;
        return 0;
    }
    return -1;
}

/* Map a physical range at the *same* virtual address.
 *
 * Needed by the application-processor trampoline.  It builds a PDPTE out of the
 * kernel's first four PML4 entries and loads it as CR3 with PAE on, which maps
 * the low four gigabytes exactly as the kernel's own tables do; the trampoline
 * is executing from low memory at that moment and has to keep fetching from
 * there for a few more instructions before it can switch to the real PML4.  So
 * the page holding it has to be reachable both ways.
 *
 * Only the low region is usable for this: the point of the map is that the
 * virtual address equals the physical one, and nothing above 4 GiB can have
 * that. */
int vmm_map_low_identity(uint64_t phys, size_t bytes) {
    if (phys == 0 || bytes == 0 || !IS_ALIGNED(phys, page_size))
        return -1;

    if (phys + bytes > 0x100000000ull)
        return -1;

    if (!vmm.active) {
        /* Before the switch there are no kernel tables of our own to add to;
         * the caller has to be after vmm_init(). */
        return -1;
    }

    if (!pt_map_range(&vmm.tables, phys, phys, bytes,
                      PTE_PRESENT | PTE_WRITE | PTE_CACHE_WB)) {
        kprintf("[warn] vmm: could not identity-map %#lx\n",
                (unsigned long)phys);
        return -1;
    }

    vmm.stats.kernel_pages += bytes / page_size;
    kprintf("vmm: identity-mapped %lu KiB at %#lx for the AP trampoline\n",
            (unsigned long)(bytes / 1024), (unsigned long)phys);
    return 0;
}

/* Point the reset vector at `phys`.
 *
 * The reset vector at 0xfffffff0 is where a processor goes when it is released
 * from INIT but has not been told where to start.  Real firmware parks a
 * halt-and-wait loop there, and that is what the documented INIT/SIPI sequence
 * assumes.  It is not a safe assumption here: whatever the firmware was, it was
 * replaced by the bootloader's own image, and on this machine an application
 * processor released from INIT executes bootloader code, faults with no IDT
 * loaded, and takes the machine down before the startup IPI is ever sent.
 *
 * So the kernel maps its own wait-for-SIPI stub over the page.  Only this
 * kernel's view of that page changes; the physical memory behind it is untouched
 * and no firmware is stopped or rewritten.
 *
 * A page is mapped, so this rounds down. */
int vmm_map_reset_vector(uint64_t phys) {
    const uint64_t reset_page = 0xfffff000ull;

    if (phys == 0 || !IS_ALIGNED(phys, page_size))
        return -1;
    if (!vmm.active)
        return -1;

    if (!pt_map_range(&vmm.tables, reset_page, phys, page_size,
                      PTE_PRESENT | PTE_WRITE | PTE_CACHE_WB))
        return -1;

    kprintf("vmm: reset vector points at the AP wait stub at %#lx\n",
            (unsigned long)phys);
    return 0;
}

/* --- init --------------------------------------------------------------- */

/* Every page-table frame the kernel has allocated has to be reachable through
 * the direct map, because that is how the walk in pagetables.c reaches them
 * (entry_at() is phys_to_virt(table_phys) + index * 8) -- there is deliberately
 * no self-mapping.  A self-mapping at PML4[PML4_INDEX(HHDM_OFFSET)] is the
 * other common arrangement, and it cannot be used here: the direct map and the
 * kernel image share that PML4 entry, so overwriting it with a pointer to the
 * PML4 turns every kernel address into a walk that ends "not present" and the
 * machine triple-faults the instant CR3 is loaded.
 *
 * So instead of mapping the tables twice, this proves the assumption the walk
 * depends on: every table frame below the top of the direct map. */
static bool tables_under_direct_map(uint64_t table_phys, unsigned levels) {
    uint64_t limit = pmm_highest_usable_phys();
    pte_t *table = (pte_t *)phys_to_virt(table_phys);

    for (unsigned i = 0; i < PML4_ENTRY_COUNT; i++) {
        pte_t entry = table[i];
        if (!pte_present(entry))
            continue;

        uint64_t next = pte_phys(entry);
        if (next >= limit)
            return false;

        /* A huge page is a leaf: there is nothing below it to reach. */
        if (levels > 1 && !pte_huge(entry) &&
            !tables_under_direct_map(next, levels - 1))
            return false;
    }

    return true;
}

static bool map_own_tables(void) {
    if (!tables_under_direct_map(vmm.tables.pml4, 3)) {
        kprintf("[warn] vmm: a page table at or above %#lx is outside the "
                "direct map\n", (unsigned long)pmm_highest_usable_phys());
        return false;
    }

    kprintf("vmm: all page-table frames are inside the direct map; no "
            "self-mapping is needed\n");
    return true;
}

int vmm_init(void) {
    if (vmm.active)
        return 0;
    if (!pmm_ready()) {
        kprintf("[warn] vmm: the physical allocator is not ready\n");
        return -1;
    }

    /* NX has to be on in EFER before any entry can carry the bit, or setting it
     * raises #UD on the first write that also touches those pages. */
    pt_enable_nx();
    pt_write_cr4(pt_read_cr4() | CR4_PAE);

    /* Check the pointer before converting it: virt_to_phys(NULL) wraps around to
     * a plausible-looking high address, so converting first turns an allocation
     * failure into a wild physical address that faults much later. */
    uint8_t *pml4_virt = pmm_alloc_zeroed_page();
    if (pml4_virt == NULL) {
        pmm_stats_t stats;
        pmm_get_stats(&stats);
        kprintf("[warn] vmm: out of frames for the top level table "
                "(%lu of %lu frames free)\n",
                (unsigned long)stats.free_frames,
                (unsigned long)stats.total_frames);
        return -1;
    }

    uint64_t pml4_phys = virt_to_phys(pml4_virt);

    vmm.tables.pml4 = pml4_phys;
    vmm.tables.pml4_v = (uint64_t *)phys_to_virt(pml4_phys);
    vmm.tables.pdpt_v = NULL;

    kprintf("vmm: PML4 at %#lx, NX enabled\n", (unsigned long)pml4_phys);

    /* 1. The kernel image.  __kernel_end is the exclusive end of the mapped
     *    segment, so the last byte is __kernel_end - 1.
     *
     *    The physical base is *asked for*, not assumed.  `virt - 0xffffffff80000000`
     *    happens to be zero, but that is only true if the bootloader happened to
     *    load the image at physical zero, and it does not: the firmware memory
     *    map has no entry covering page 0 at all, because that page belongs to
     *    the BIOS.  Walking the bootloader's tables gives the address the image
     *    is actually at, which is the only one guaranteed to be right. */
    uint64_t image_virt = (uint64_t)__kernel_start;
    uint64_t image_phys = vmm_walk_translate(image_virt, NULL);
    if (image_phys == 0) {
        kprintf("[warn] vmm: the bootloader's tables do not map the kernel "
                "image; cannot build new tables\n");
        return -1;
    }
    uint64_t image_bytes = ALIGN_UP((uint64_t)__kernel_end - image_virt,
                                    page_size);

    kprintf("vmm: kernel image at physical %#lx\n", (unsigned long)image_phys);

    /* Kept for vmm_kernel_phys(): a kernel-image address is not a direct-map address, so
     * virt_to_phys() cannot convert one, and the difference matters. */
    vmm.image_virt = image_virt;
    vmm.image_phys = image_phys;

    if (!pt_map_range(&vmm.tables, image_virt, image_phys, image_bytes,
                      PTE_PRESENT | PTE_WRITE | PTE_CACHE_WB)) {
        kprintf("[warn] vmm: could not map the kernel image\n");
        return -1;
    }
    vmm.stats.kernel_pages += image_bytes / page_size;
    kprintf("vmm: kernel image %lu KiB at %#lx\n",
            (unsigned long)(image_bytes / 1024), (unsigned long)image_virt);

    /* 2. The direct map, covering every frame the firmware called usable.  1 GiB
     *    pages where the machine allows them, 2 MiB otherwise. */
    uint64_t direct_bytes = ALIGN_UP(pmm_highest_usable_phys(), page_size);
    bool ok;

    if (direct_bytes >= 0x40000000ull) {
        ok = pt_map_1gib(&vmm.tables, HHDM_OFFSET, 0, direct_bytes,
                         PTE_PRESENT | PTE_WRITE | PTE_CACHE_WB | PTE_NX);
        vmm.stats.huge_pages = direct_bytes / 0x40000000ull;
    } else {
        ok = pt_map_2mib(&vmm.tables, HHDM_OFFSET, 0, direct_bytes,
                         PTE_PRESENT | PTE_WRITE | PTE_CACHE_WB | PTE_NX);
        vmm.stats.huge_pages = direct_bytes / 0x200000ull;
    }

    if (!ok) {
        kprintf("[warn] vmm: could not map %lu MiB of direct map\n",
                (unsigned long)(direct_bytes / (1024 * 1024)));
        return -1;
    }

    /* The image was just overwritten by the direct map at the same physical
     * addresses; put the NX-free copy back so the kernel can execute. */
    if (!pt_map_range(&vmm.tables, image_virt, image_phys, image_bytes,
                      PTE_PRESENT | PTE_WRITE | PTE_CACHE_WB)) {
        kprintf("[warn] vmm: could not restore the kernel image mapping\n");
        return -1;
    }

    vmm.stats.kernel_pages += direct_bytes / page_size;
    kprintf("vmm: direct map %lu MiB at %#lx (%lu huge pages), NX\n",
            (unsigned long)(direct_bytes / (1024 * 1024)),
            (unsigned long)HHDM_OFFSET, (unsigned long)vmm.stats.huge_pages);

    /* 3. The heap.  It starts past the image and grows by mapping more frames as
     *    the allocator attaches regions. */
    vmm.heap_virt_base = ALIGN_UP(image_virt + image_bytes, 0x200000ull);
    vmm.heap_virt_end = vmm.heap_virt_base;
    kprintf("vmm: heap window starts at %#lx\n",
            (unsigned long)vmm.heap_virt_base);

    /* 4. Device windows, uncacheable and kernel-only. */
    for (unsigned i = 0; i < vmm.mmio_count; i++) {
        if (!pt_map_range(&vmm.tables, vmm.mmio[i].virt, vmm.mmio[i].phys,
                          vmm.mmio[i].bytes,
                          PTE_PRESENT | PTE_WRITE | PTE_CACHE_UC)) {
            kprintf("[warn] vmm: could not map the %s window at %#lx\n",
                    vmm.mmio[i].name, (unsigned long)vmm.mmio[i].phys);
            return -1;
        }
        vmm.stats.mmio_pages += vmm.mmio[i].bytes / page_size;
        kprintf("vmm: mapped %lu KiB of %s at %#lx (uncacheable)\n",
                (unsigned long)(vmm.mmio[i].bytes / 1024), vmm.mmio[i].name,
                (unsigned long)vmm.mmio[i].phys);
    }

    /* 5. The framebuffer.  This is the trap in switching to kernel-owned page
     *    tables: the framebuffer is not in RAM.  Limine places it above the top
     *    of usable memory (at 0xfd000000 here), so the direct map -- which
     *    covers exactly the frames the firmware called usable -- does not reach
     *    it.  Its address is an HHDM linear address that the *bootloader's*
     *    tables map, which is why drawing to the console worked right up to the
     *    moment CR3 was replaced and then stopped with a page fault on the next
     *    kprintf.  So map it explicitly, write-combining rather than
     *    uncacheable: it is written far more than it is read. */
    const fb_info_t *fb = fb_get_info();
    if (fb->ready && fb->base != NULL) {
        size_t fb_bytes = (size_t)fb->pitch * fb->height;
        uint64_t fb_phys = virt_to_phys(fb->base);

        if (pt_map_range(&vmm.tables, (uint64_t)fb->base, fb_phys, fb_bytes,
                         PTE_PRESENT | PTE_WRITE | PTE_CACHE_WC)) {
            vmm.stats.mmio_pages += fb_bytes / page_size;
            kprintf("vmm: framebuffer %ux%u at %#lx (%lu KiB, write-combining)\n",
                    fb->width, fb->height, (unsigned long)fb_phys,
                    (unsigned long)(fb_bytes / 1024));
        } else {
            kprintf("[warn] vmm: could not map the framebuffer at %#lx\n",
                    (unsigned long)fb_phys);
        }
    }

    if (!map_own_tables()) {
        kprintf("[warn] vmm: the page tables are not all reachable; staying "
                "on the bootloader's tables\n");
        return -1;
    }

    /* Keep the bootloader's CR3 so its tables can go back to the pool after the
     * switch. */
    boot_set_old_cr3(pt_read_cr3());

    /* Everything the kernel is currently using has to be reachable *before* the
     * switch, not just after: the next instruction fetch is from the image, the
     * stack is in the heap, the console's cells and the GDT/IDT are in .bss, and
     * the fault handler is no use at all if it cannot be fetched. */
    pt_write_cr3(vmm.tables.pml4);
    vmm.active = true;

    kprintf("vmm: switched to CR3 %#lx; the direct map and the APIC windows "
            "are live\n", (unsigned long)vmm.tables.pml4);

    /* 6. Supervisor mode execution prevention, now that the tables are in
     *    place.  Everything the kernel executes is mapped without NX, so this
     *    catches a kernel that jumps into the heap rather than stopping every
     *    data page from ever running code. */
    pt_write_cr4(pt_read_cr4() | CR4_SMEP);

    /* The bootloader's page tables are *not* released here.  The bootloader
     * starts the application processors itself and parks them on its own page
     * tables, so those tables stay live — and read — until every processor has
     * been handed its entry point and switched to the kernel's.  Releasing them
     * at the CR3 switch would hand the allocator frames three processors are still
     * walking.  vmm_release_boot_tables() is called after the hand-off. */
    kprintf("vmm: the bootloader's tables at %#lx stay live until every "
            "processor has been handed off\n",
            (unsigned long)boot_old_cr3());
    return 0;
}

/* Release the bootloader's memory now that nothing is running on its tables.
 *
 * Only safe once every processor the bootloader started is executing on the
 * kernel's own address space: the frames behind its tables were marked used, so
 * nothing else can have taken them, but a processor that is still walking them
 * would be reading frames the allocator now believes are free. */
int vmm_release_boot_tables(void) {
    if (!vmm.active)
        return -1;
    if (vmm.boot_tables_released)
        return 0;

    pmm_release_boot_tables();
    vmm.boot_tables_released = true;
    return 0;
}

/* --- mappings used after init ------------------------------------------- */

int vmm_map_kernel_virt(uint64_t virt, uint64_t phys, size_t bytes,
                        bool executable) {
    if (!vmm.active)
        return -1;
    size_t span = ALIGN_UP(bytes, page_size);

    uint64_t flags = PTE_PRESENT | PTE_WRITE | PTE_CACHE_WB;
    if (!executable)
        flags |= PTE_NX;

    if (!pt_map_range(vmm_tables(), virt, phys, span, flags))
        return -1;

    vmm.stats.kernel_pages += span / page_size;
    return 0;
}

int vmm_unmap_kernel_virt(uint64_t virt, size_t bytes) {
    if (!vmm.active)
        return -1;

    size_t span = ALIGN_UP(bytes, page_size);
    for (size_t off = 0; off < span; off += page_size)
        pt_unmap_page(vmm_tables(), virt + off);

    if (vmm.stats.kernel_pages >= span / page_size)
        vmm.stats.kernel_pages -= span / page_size;
    return 0;
}

int vmm_set_nx_range(uint64_t virt, size_t bytes, bool nx) {
    if (!vmm.active)
        return -1;

    for (size_t off = 0; off < ALIGN_UP(bytes, page_size); off += page_size) {
        if (!pt_set_nx(vmm_tables(), virt + off, nx))
            return -1;
    }
    return 0;
}

int vmm_map_user(uint64_t virt, uint64_t phys, size_t bytes, bool writable) {
    return vmm_map_into(vmm_tables(), virt, phys, bytes, writable);
}

/* The same thing with the address space named, for callers that are not running in it.
 *
 * The ELF loader is the case that makes this necessary: it fills in a task's address
 * space before that task has ever been scheduled, so `vmm_tables()` -- the running task's
 * -- is the wrong one, and mapping through it succeeds while the program ends up with no
 * code mapped at its entry point. */
int vmm_map_into(page_table_t *tables, uint64_t virt, uint64_t phys, size_t bytes,
                 bool writable) {
    if (!vmm.active || tables == NULL)
        return -1;

    size_t span = ALIGN_UP(bytes, page_size);
    uint64_t flags = PTE_PRESENT | PTE_USER | PTE_CACHE_WB;
    if (writable)
        flags |= PTE_WRITE;

    if (!pt_map_range(tables, virt, phys, span, flags))
        return -1;

    vmm.stats.user_pages += span / page_size;
    return 0;
}

int vmm_unmap_user(uint64_t virt, size_t bytes) {
    if (!vmm.active)
        return -1;

    size_t span = ALIGN_UP(bytes, page_size);
    for (size_t off = 0; off < span; off += page_size)
        pt_unmap_page(vmm_tables(), virt + off);

    if (vmm.stats.user_pages >= span / page_size)
        vmm.stats.user_pages -= span / page_size;
    return 0;
}

void vmm_get_stats(vmm_stats_t *out) {
    *out = vmm.stats;
}
