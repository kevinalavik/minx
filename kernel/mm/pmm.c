#include "pmm.h"

#include <limine.h>

#include "boot.h"
#include "kprintf.h"
#include "pagetables.h"
#include "pagemap.h"
#include "panic.h"
#include "spinlock.h"
#include "string.h"
#include "types.h"

/* One bit per frame: clear is free, set is used or reserved.
 *
 * The space covered is a compile-time choice, because the bitmap itself has to
 * exist before the allocator that could provide it.  64 GiB costs 2 MiB of
 * .bss; a machine with more reports the clamp rather than quietly miscounting. */
#define PMM_MAX_PHYS (64ull * 1024ull * 1024ull * 1024ull)
#define BITMAP_WORDS  ((size_t)(PMM_MAX_PHYS / page_size / 64))

/* The bitmap cannot come from the allocator it describes, so it starts here. */
static uint64_t bitmap_storage[BITMAP_WORDS] __attribute__((aligned(4096)));

static struct {
    bool     ready;
    uint64_t *bits;
    size_t    frames_per_word;
    uint64_t  total_frames;
    uint64_t  free_frames;
    uint64_t  reserved_frames;
    uint64_t  highest_usable;

    /* Allocation hint.  Starting where the last allocation ended makes the
     * common "allocate a page table, allocate a page table" pattern hit the same
     * cache-friendly region instead of walking the whole bitmap. */
    uint64_t  cursor;
} pmm;

bool pmm_ready(void) { return pmm.ready; }

/* The lock, and why this file needs one at all.
 *
 * The physical allocator is the only allocator every processor uses: a page table for a
 * fault, a frame for a copy-on-write page, a frame for a program image.  Every one of
 * those is a read-modify-write of one bit in one bitmap word, and `pmm.cursor` and
 * `pmm.free_frames` are read-modify-writes too -- so two processors asking for a frame at
 * the same time can both find the same one free, and both hand it out.
 *
 * That is not a rare race to shrug at.  Two address spaces then name the same physical
 * page, so a program writes into another process's data and neither of them faults; a
 * freed frame is still in a page table, so the next allocation writes into a mapping the
 * processor is using; and `free_frames` drifts away from the truth, so the machine either
 * claims memory it does not have or refuses memory it does.  Every one of those looks like
 * something else entirely: a filesystem that runs out of nodes, a test that fails once in
 * three, a machine that stops with no fault and no message.
 *
 * A spinlock, irq-saving.  It is held for a bitmap walk and a couple of counter updates,
 * which is short, and it must be irq-saving because a frame is also allocated from an
 * interrupt -- the timer tick waking a sleeper allocates nothing, but the page-fault path
 * does, and an interrupt that spins on a lock its own interrupted code holds never
 * returns. */
static ticket_lock_t pmm_lock;

/* --- bitmap primitives -------------------------------------------------- */

static inline void bit_set(uint64_t frame) {
    pmm.bits[frame / 64] |= 1ull << (frame % 64);
}

static inline void bit_clear(uint64_t frame) {
    pmm.bits[frame / 64] &= ~(1ull << (frame % 64));
}

static inline bool bit_test(uint64_t frame) {
    return (pmm.bits[frame / 64] & (1ull << (frame % 64))) != 0;
}

/* --- init --------------------------------------------------------------- */

/* Mark every frame as used.  The firmware's usable regions then clear the bits
 * back, so a frame nobody described can never be handed out. */
static void fill_unusable(void) {
    memset(pmm.bits, 0xff, (size_t)((pmm.total_frames + 63) / 64) *
                              sizeof(uint64_t));
}

static void add_usable_range(uint64_t base, uint64_t bytes) {
    uint64_t first = ALIGN_UP(base, page_size) / page_size;
    uint64_t last = ALIGN_DOWN(base + bytes, page_size) / page_size;

    for (uint64_t frame = first; frame < last && frame < pmm.total_frames;
         frame++) {
        /* Everything starts marked used, so a frame that is still set here is
         * one this range is giving back. */
        if (bit_test(frame)) {
            bit_clear(frame);
            pmm.free_frames++;
        }
    }

    if (last * page_size > pmm.highest_usable)
        pmm.highest_usable = last * page_size;
}

int pmm_init(void) {
    if (pmm.ready)
        return 0;

    struct limine_memmap_response *map = boot_memmap();
    if (map == NULL) {
        kprintf("[warn] pmm: no memory map; falling back to the static arena\n");
        return -1;
    }

    /* Work out the extent first: the bitmap must cover everything usable. */
    uint64_t highest = 0;
    uint64_t usable_bytes = 0;

    /* entries is an array of pointers, not an array of structs. */
    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        uint64_t end = e->base + e->length;
        if (end > highest)
            highest = end;
        usable_bytes += e->length;
    }

    if (highest == 0) {
        kprintf("[warn] pmm: the memory map contains no usable memory\n");
        return -1;
    }

    uint64_t frames = highest / page_size;
    if (frames > BITMAP_WORDS * 64) {
        kprintf("[warn] pmm: %lu frames exceeds the static bitmap's %lu; "
                "clamping\n", (unsigned long)frames,
                (unsigned long)(BITMAP_WORDS * 64));
        frames = BITMAP_WORDS * 64;
    }

    pmm.bits = bitmap_storage;
    pmm.total_frames = frames;
    pmm.free_frames = 0;
    pmm.reserved_frames = 0;
    pmm.highest_usable = 0;
    pmm.cursor = 0;

    memset(pmm.bits, 0, (size_t)((frames + 63) / 64) * sizeof(uint64_t));
    fill_unusable();

    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        add_usable_range(e->base, e->length);
    }

    /* Anything firmware called reclaimable is safe to reclaim, *but not before
     * the kernel replaces CR3*.  This is where the bootloader's own page tables
     * live: "reclaimable" means the bootloader promises not to use it again, and
     * the bootloader is still the CPU's active translation source until the
     * kernel's CR3 is loaded.  Marking these frames free at this point lets the
     * allocator hand out the very pages the running page tables are stored in,
     * and the first write into the kernel's own top-level table then silently
     * corrupts the table the CPU is walking -- which surfaces much later as a
     * page fault at an address that has nothing to do with the code running.
     *
     * So they are added to the pool and then reserved again by
     * pmm_reserve_boot_areas(); pmm_release_boot_tables() returns them once CR3
     * no longer points at Limine's tables. */
    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
            continue;
        kprintf("pmm: reclaimable (held until CR3 is replaced): %lu KiB "
                "at %#lx\n", (unsigned long)(e->length / 1024),
                (unsigned long)e->base);
        add_usable_range(e->base, e->length);
    }

    kprintf("pmm: %lu MiB usable in %lu frames (highest %#lx), "
            "bitmap %lu KiB\n",
            (unsigned long)(usable_bytes / (1024 * 1024)),
            (unsigned long)pmm.total_frames, (unsigned long)highest,
            (unsigned long)((frames + 63) / 64 * 8 / 1024));
    kprintf("pmm: %lu frames free after marking the map\n",
            (unsigned long)pmm.free_frames);

    pmm_reserve_boot_areas();

    pmm.ready = true;
    return 0;
}

/* --- reservations ------------------------------------------------------- */

/* Reserve every frame that overlaps [phys, phys+bytes).
 *
 * The overlap matters and is not a detail: a region that does not end on a page
 * boundary -- the kernel image, which ends wherever .bss ends -- still owns every
 * byte of the frame its last byte is in.  Rounding the end *down* to a page
 * boundary leaves that frame free, and the allocator then hands the kernel's own
 * tail out as a page table, which corrupts whatever lives there the moment the
 * first page-table entry is written into it. */
void pmm_reserve_phys_range(uint64_t phys, size_t bytes) {
    if (!pmm.bits)
        return;

    uint64_t first = ALIGN_DOWN(phys, page_size) / page_size;
    uint64_t last = ALIGN_UP(phys + bytes, page_size) / page_size;
    uint64_t flags = spin_lock_irqsave(&pmm_lock);

    for (uint64_t frame = first; frame < last && frame < pmm.total_frames;
         frame++) {
        if (!bit_test(frame)) {
            bit_set(frame);
            pmm.free_frames--;
            pmm.reserved_frames++;
        }
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
}

/* How many reclaimable ranges the boot-time record holds.  A machine with more
 * than this keeps the extras reserved rather than risking the bootloader's
 * tables; the count is reported so it is visible rather than silent. */
#define MAX_BOOT_TABLE_RANGES 32

static void pmm_reserve_bootloader_areas(void);

/* Mark everything the kernel is already sitting on.  Missing any of this turns
 * a page-table allocation into a silent corruption of the running kernel, so it
 * is done by walking the linker script's own symbols rather than by guessing
 * sizes. */
void pmm_reserve_boot_areas(void) {
    uint64_t image_start = (uint64_t)__kernel_start;
    uint64_t image_end = (uint64_t)__kernel_end;

    /* Where the image really is physically comes from the bootloader's own page
     * tables, not from the higher-half base: assuming `virt - 0xffffffff80000000`
     * reserves the wrong frames, and the frames actually holding the running code
     * then get handed out. */
    uint64_t phys_start = vmm_walk_translate(image_start, NULL);
    if (phys_start == 0) {
        kprintf("[warn] pmm: the bootloader's tables do not map the kernel "
                "image; its frames cannot be reserved\n");
        return;
    }
    uint64_t phys_end = phys_start + (image_end - image_start);

    pmm_reserve_phys_range(phys_start, phys_end - phys_start);
    kprintf("pmm: reserved %lu KiB of kernel image at %#lx\n",
            (unsigned long)((phys_end - phys_start) / 1024),
            (unsigned long)phys_start);

    if (boot_info.framebuffer_addr != 0) {
        /* The address recorded is the HHDM linear address the console draws
         * through, so it has to be translated back before reserving: reserving
         * 0xffff8000fd000000 marks frames at the top of the address space and
         * protects nothing. */
        uint64_t fb_phys = virt_to_phys(boot_info.framebuffer_addr);
        size_t fb_bytes = (size_t)boot_info.framebuffer_pitch *
                          boot_info.framebuffer_height;

        pmm_reserve_phys_range(fb_phys, fb_bytes);
        kprintf("pmm: reserved the framebuffer at %#lx (%lu KiB)\n",
                (unsigned long)fb_phys, (unsigned long)(fb_bytes / 1024));
    }

    if (boot_info.initrd_base != 0) {
        /* The initrd is reported as an address in the bootloader's direct map, so
         * it has to be translated back before reserving. */
        uint64_t initrd_phys = virt_to_phys(boot_info.initrd_base);
        pmm_reserve_phys_range(initrd_phys, boot_info.initrd_size);
        kprintf("pmm: reserved %lu KiB of initrd at %#lx\n",
                (unsigned long)(boot_info.initrd_size / 1024),
                (unsigned long)initrd_phys);
    }

    /* Everything the bootloader gave back is reserved again: its page tables are
     * in there and the CPU is walking them until CR3 is replaced.  Taken out of
     * the pool for the duration of the switch, returned afterwards. */
    pmm_reserve_bootloader_areas();
}

/* Reserve every frame the firmware marked reclaimable.
 *
 * The record is kept so the range can be handed back exactly later, rather than
 * by remembering CR3 (one page is not the whole set: a page table walk needs a
 * PML4, a PDPT, a page directory per region and a page table per 2 MiB, and
 * Limine builds several of those). */
static struct {
    uint64_t base;
    uint64_t length;
} boot_tables[MAX_BOOT_TABLE_RANGES];
static unsigned boot_table_ranges;

static void pmm_reserve_bootloader_areas(void) {
    struct limine_memmap_response *map = boot_memmap();
    if (map == NULL)
        return;

    for (uint64_t i = 0; i < map->entry_count && boot_table_ranges < MAX_BOOT_TABLE_RANGES; i++) {
        struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
            continue;

        boot_tables[boot_table_ranges].base = e->base;
        boot_tables[boot_table_ranges].length = e->length;
        boot_table_ranges++;

        pmm_reserve_phys_range(e->base, e->length);
    }

    kprintf("pmm: holding %u reclaimable range(s) until CR3 is replaced\n",
            boot_table_ranges);
}

/* Once CR3 points at the kernel's own tables, Limine's can go back to the pool.
 * Called only after the switch, because freeing the tables the CPU is standing on
 * would take the machine down mid-instruction. */
void pmm_release_boot_tables(void) {
    if (!pmm.ready)
        return;

    uint64_t old_cr3 = boot_old_cr3();
    if (old_cr3 == 0)
        return;

    boot_set_old_cr3(0);

    /* The whole reclaimable set, not just the frame CR3 named. */
    for (unsigned i = 0; i < boot_table_ranges; i++)
        pmm_free_phys_range(boot_tables[i].base, boot_tables[i].length);
    boot_table_ranges = 0;

    kprintf("pmm: released the bootloader's memory (old CR3 was %#lx)\n",
            (unsigned long)old_cr3);
}

/* --- allocation --------------------------------------------------------- */

uint64_t pmm_alloc_phys_page(void) {
    if (!pmm.ready)
        return 0;

    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint64_t got   = 0;

    if (pmm.free_frames != 0) {
        uint64_t total = pmm.total_frames;

        for (uint64_t i = 0; i < total; i++) {
            uint64_t frame = (pmm.cursor + i) % total;

            if (bit_test(frame))
                continue;

            bit_set(frame);
            pmm.free_frames--;
            /* Start the next search just past this frame. */
            pmm.cursor = (frame + 1) % total;
            got = frame * page_size;
            break;
        }
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
    return got;
}

uint8_t *pmm_alloc_zeroed_page(void) {
    uint64_t phys = pmm_alloc_phys_page();
    if (phys == 0)
        return NULL;

    void *p = (void *)phys_to_virt(phys);
    memset(p, 0, page_size);
    return p;
}

uint8_t *pmm_alloc_page(void) {
    uint64_t phys = pmm_alloc_phys_page();
    return phys != 0 ? (uint8_t *)phys_to_virt(phys) : NULL;
}

uint8_t *pmm_alloc_pages(size_t count) {
    if (count == 0 || !pmm.ready)
        return NULL;

    uint64_t total = pmm.total_frames;
    for (uint64_t i = 0; i + count <= total; i++) {
        uint64_t start = (pmm.cursor + i) % total;

        /* Never wrap: a run that crosses the end of the bitmap would not be
         * physically contiguous, and returning it would corrupt whatever maps
         * it later. */
        if (start + count > total)
            continue;

        bool run_free = true;
        for (size_t j = 0; j < count; j++) {
            if (bit_test(start + j)) {
                run_free = false;
                break;
            }
        }
        if (!run_free)
            continue;

        for (size_t j = 0; j < count; j++) {
            bit_set(start + j);
            pmm.free_frames--;
        }
        pmm.cursor = (start + count) % total;

        return (uint8_t *)phys_to_virt(start * page_size);
    }

    return NULL;
}

uint8_t *pmm_alloc_zeroed_pages(size_t count) {
    uint8_t *p = pmm_alloc_pages(count);
    if (p == NULL)
        return NULL;
    memset(p, 0, count * page_size);
    return p;
}

void pmm_free_phys_page(uint64_t phys) {
    if (!pmm.ready)
        return;

    uint64_t frame = phys / page_size;
    if (frame >= pmm.total_frames)
        return;

    uint64_t flags = spin_lock_irqsave(&pmm_lock);

    /* Only a frame that was actually in use becomes free, so only that one is
     * counted.  Releasing a frame that is already free must not add to the
     * total, or a double free inflates the free count and the allocator starts
     * believing the machine has more memory than it does.  Testing for the
     * frame being *free* here gets it exactly backwards: a real free would go
     * uncounted and a double free counted, and neither shows up as an error --
     * the memory is simply never given back. */
    if (bit_test(frame)) {
        pmm.free_frames++;
        if (pmm.reserved_frames > 0)
            pmm.reserved_frames--;
    }
    bit_clear(frame);

    spin_unlock_irqrestore(&pmm_lock, flags);
}

/* Release a whole range.  Frames that were never taken are counted once, the
 * same way pmm_free_phys_page() does it. */
void pmm_free_phys_range(uint64_t phys, size_t bytes) {
    uint64_t first = ALIGN_DOWN(phys, page_size) / page_size;
    uint64_t last = ALIGN_UP(phys + bytes, page_size) / page_size;
    uint64_t flags = spin_lock_irqsave(&pmm_lock);

    for (uint64_t frame = first; frame < last && frame < pmm.total_frames;
         frame++) {
        if (bit_test(frame)) {
            pmm.free_frames++;
            if (pmm.reserved_frames > 0)
                pmm.reserved_frames--;
        }
        bit_clear(frame);
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
}

bool pmm_phys_page_is_free(uint64_t phys) {
    if (!pmm.ready || !IS_ALIGNED(phys, page_size))
        return false;

    uint64_t frame = phys / page_size;
    return frame < pmm.total_frames && !bit_test(frame);
}

uint64_t pmm_alloc_phys_page_at(uint64_t phys) {
    if (!pmm.ready || !IS_ALIGNED(phys, page_size))
        return 0;

    uint64_t frame = phys / page_size;
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint64_t got   = 0;

    if (frame < pmm.total_frames && !bit_test(frame)) {
        bit_set(frame);
        pmm.free_frames--;

        /* Start the next search just past this frame, so a caller taking a specific
         * low address does not leave the cursor parked at the bottom of memory. */
        pmm.cursor = (frame + 1) % pmm.total_frames;
        got = phys;
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
    return got;
}

void pmm_free_page(void *virt) {
    pmm_free_phys_page(virt_to_phys(virt));
}

void pmm_free_phys_pages(uint64_t phys, size_t count) {
    pmm_free_phys_range(phys, count * page_size);
}

void pmm_free_pages(void *virt, size_t count) {
    pmm_free_phys_pages(virt_to_phys(virt), count);
}

/* --- queries ------------------------------------------------------------ */

void pmm_get_stats(pmm_stats_t *out) {
    memset(out, 0, sizeof(*out));

    out->total_frames = pmm.total_frames;
    out->free_frames = pmm.free_frames;
    out->used_frames = pmm.total_frames - pmm.free_frames;
    out->reserved_frames = pmm.reserved_frames;
    out->total_bytes = pmm.total_frames * page_size;
    out->highest_allocated = pmm.highest_usable;

    /* The longest run of free frames, which is the largest contiguous mapping
     * still possible. */
    uint64_t run = 0, best = 0;
    for (uint64_t frame = 0; frame < pmm.total_frames; frame++) {
        if (!bit_test(frame)) {
            run++;
            if (run > best)
                best = run;
        } else {
            run = 0;
        }
    }
    out->largest_free_run = best;
}

bool pmm_physical_is_mapped(uint64_t phys) {
    return phys < pmm.highest_usable;
}

uint64_t pmm_highest_usable_phys(void) {
    return pmm.highest_usable;
}
