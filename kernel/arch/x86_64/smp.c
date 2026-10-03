/* minx - starting application processors.
 *
 * An application processor cannot be started by writing to anything the kernel
 * already controls: it is held in reset with nothing running, and the only way
 * out is a pair of inter-processor interrupts through the local APIC of the CPU
 * doing the starting.  This is that sequence, plus the per-CPU state each
 * processor needs before it is allowed to run anything.
 *
 * The order of operations matters at every step and each one is a place where a
 * mistake produces a machine that hangs rather than an error:
 *
 *   1. The trampoline is copied to a page below 1 MiB, because the startup IPI
 *      carries only eight bits and the CPU derives the entry point by
 *      multiplying them by sixteen.
 *   2. That page is identity-mapped in the kernel's own tables, because the
 *      trampoline carries a PDPTE built from the kernel's first four PML4
 *      entries and keeps fetching from itself after CR3 is loaded.
 *   3. INIT is sent, which resets the target processor.
 *   4. Two startup IPIs follow, with a wait in between.  One is the documented
 *      minimum; two is what the SDM recommends and what Limine sends, because a
 *      startup IPI delivered while the target is still leaving reset is lost.
 *   5. The boot processor spins on a flag the trampoline sets, with a bound, so
 *      a processor that never arrives is reported rather than hanging the boot.
 */
#include "smp.h"

#include "apic.h"
#include "cpu.h"
#include "delay.h"
#include "io.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "panic.h"
#include "percpu.h"
#include "pmm.h"
#include "string.h"
#include "types.h"
#include "vmm.h"

/* Layout of the trampoline's info block, repeated from
 * kernel/arch/x86_64/smp_trampoline.S.  Both files have to agree and there is no
 * way to share the constants between assembly and C, so the block is described
 * once here and the .S file documents the same list next to its own copy. */
#define TPL_CR3          0    /* u32  PDPTE page, physical               */
#define TPL_PML4         4    /* u32  the kernel's PML4, physical         */
#define TPL_BOOTED       8    /* u32  set to 1 once in long mode          */
#define TPL_INDEX        12   /* u32  index into the per-CPU array        */
#define TPL_ENTRY        16   /* u64  64-bit kernel entry point           */
#define TPL_STACK        24   /* u64  64-bit stack (an HHDM address)      */
#define TPL_GS           32   /* u64  IA32_GS_BASE for this processor     */
#define TPL_GDT32_LIMIT  40   /* u16  */
#define TPL_GDT32_BASE   48   /* u64  */
#define TPL_GDT64_LIMIT  56   /* u16  */
#define TPL_GDT64_BASE   64   /* u64  */
#define TPL_IDT_LIMIT    72   /* u16  */
#define TPL_IDT_BASE     80   /* u64  */
#define TPL_PROGRESS     88   /* u32  how far the stub got              */
#define TPL_BLOCK_SIZE   144

/* The startup IPI carries the entry point as an eight-bit vector, so the page
 * has to live below 1 MiB.  This is the most the firmware is expected to leave
 * usable down there; a machine with less gets a clear failure instead of a
 * trampoline that is never entered. */
#define TPL_MAX_PHYS     0x100000ull

/* Where the startup vector's selector sits inside the far jump.  The encoding is
 * EA, a 16-bit offset, then the 16-bit selector. */
#define FARJMP_SELECTOR_OFFSET 3

/* Symbols the trampoline exports.  Only their offsets within the trampoline
 * matter; the addresses themselves are link-time addresses of the kernel image
 * and mean nothing until the code is copied down to physical memory. */
extern const uint8_t  smp_trampoline_start[];
extern const uint8_t  smp_trampoline_farjmp[];
extern const uint8_t  smp_trampoline_block[];
extern const uint8_t  smp_trampoline_gdt16_base[];
extern const uint8_t  smp_trampoline_wait[];   /* where the reset vector lands */
extern const uint64_t smp_trampoline_size;

/* The 32-bit IDT pseudo-descriptor, for the trampoline to load.  Kept here
 * because only cpu.c knows the IDT's address and limit. */
extern void idt_descriptor(dt_ptr_t *out);

static uint64_t tpl_phys;
static uint8_t *smp_info_for(unsigned index, uint64_t gs_base,
                             uint64_t stack_top, uint64_t entry);
static bool smp_wait_for_boot(unsigned timeout_ms);

/* --- the trampoline page ------------------------------------------------ */

static void trampoline_patches(uint64_t base) {
    /* The far jump's selector is this page's number in real mode: CS becomes
     * base >> 4 and IP the link-time offset of the 32-bit stub, so CS:IP lands
     * on the page's first byte. */
    uint16_t selector = (uint16_t)((base >> 4) & 0xffff);
    *(volatile uint16_t *)(phys_to_virt(base) +
                           (smp_trampoline_farjmp - smp_trampoline_start) +
                           FARJMP_SELECTOR_OFFSET) = selector;

    /* The throwaway GDT lives inside the page, so its base is a physical
     * address, not the link-time address the assembler wrote. */
    *(volatile uint32_t *)(phys_to_virt(base) +
                           (smp_trampoline_gdt16_base - smp_trampoline_start)) =
        (uint32_t)(base + (smp_trampoline_gdt16_base - smp_trampoline_start));
}

static int trampoline_install(void) {
    if (tpl_phys != 0)
        return 0;

    if (!pmm_ready() || !vmm_active())
        return -1;

    /* One page below 1 MiB, for the reasons above.  pmm_alloc_phys_page()
     * searches forward from a cursor that may already be past the low memory,
     * so the frame is found by asking for the frame index directly. */
    uint64_t page = 0;
    for (uint64_t frame = 0x1000 / page_size;
         frame < (TPL_MAX_PHYS / page_size); frame++) {
        if (pmm_phys_page_is_free(frame * page_size)) {
            page = pmm_alloc_phys_page_at(frame * page_size);
            break;
        }
    }
    if (page == 0 || page >= TPL_MAX_PHYS) {
        kprintf("[warn] smp: no free page below 1 MiB for the AP trampoline\n");
        return -1;
    }

    uint64_t bytes = smp_trampoline_size;
    if (bytes > page_size) {
        kprintf("[warn] smp: the AP trampoline is %lu bytes, which does not "
                "fit in a page\n", (unsigned long)bytes);
        pmm_free_phys_page(page);
        return -1;
    }

    /* Identity-map the page so the trampoline can keep fetching from itself
     * after it loads the PDPTE built out of the kernel's low PML4 entries. */
    if (vmm_map_low_identity(page, page_size) != 0) {
        kprintf("[warn] smp: could not identity-map the trampoline page\n");
        pmm_free_phys_page(page);
        return -1;
    }

    memcpy((void *)phys_to_virt(page), smp_trampoline_start, (size_t)bytes);
    trampoline_patches(page);

    /* The wait-for-startup-IPI stub shares this page.  It is copied along with
     * everything else but nothing can be pointed at it yet -- see the comment on
     * the INIT sequence in smp_start_cpu(). */
    (void)smp_trampoline_wait;

    tpl_phys = page;
    kprintf("smp: AP trampoline %lu bytes at %#lx\n",
            (unsigned long)bytes, (unsigned long)page);
    return 0;
}

/* --- the PDPTE the trampoline needs ------------------------------------ */

/* With PAE enabled CR3 names four PDPTE entries, not 512 PML4 entries, so the
 * kernel's PML4 cannot be loaded directly.  Copying its first four entries gives
 * a CR3 that maps the low four gigabytes exactly as the kernel's own tables do,
 * which is what the trampoline needs: it is running from low memory at the
 * moment it loads this, and has to keep fetching from there for a few more
 * instructions before it can switch to the real PML4. */
static uint64_t pdpte_prepare(void) {
    uint8_t *page = pmm_alloc_zeroed_page();
    if (page == NULL)
        return 0;

    page_table_t *tables = vmm_tables();
    if (tables == NULL || tables->pml4 == 0) {
        pmm_free_page(page);
        return 0;
    }

    uint64_t *src = (uint64_t *)phys_to_virt(tables->pml4);
    uint64_t *dst = (uint64_t *)page;
    for (unsigned i = 0; i < 4; i++)
        dst[i] = src[i];

    /* A processor that walks into a user page from here would have a window it
     * should not, so the copies are marked supervisor-only. */
    for (unsigned i = 0; i < 4; i++)
        dst[i] &= ~(1ull << 2);

    return virt_to_phys(page);
}

/* --- the per-CPU block -------------------------------------------------- */

static uint8_t *smp_info_for(unsigned index, uint64_t gs_base,
                             uint64_t stack_top, uint64_t entry) {
    dt_ptr_t gdt = gdt_descriptor();
    dt_ptr_t idtd;
    idt_descriptor(&idtd);

    page_table_t *tables = vmm_tables();
    uint64_t pdpte = pdpte_prepare();
    if (pdpte == 0)
        return NULL;

    uint8_t *info = (uint8_t *)phys_to_virt(tpl_phys) +
                    (smp_trampoline_block - smp_trampoline_start);
    memset(info, 0, TPL_BLOCK_SIZE);

    *(volatile uint32_t *)(info + TPL_CR3) = (uint32_t)pdpte;
    *(volatile uint32_t *)(info + TPL_PML4) = (uint32_t)tables->pml4;
    *(volatile uint32_t *)(info + TPL_BOOTED) = 0;
    *(volatile uint32_t *)(info + TPL_INDEX) = index;
    *(volatile uint64_t *)(info + TPL_ENTRY) = entry;
    *(volatile uint64_t *)(info + TPL_STACK) = stack_top;
    *(volatile uint64_t *)(info + TPL_GS) = gs_base;
    *(volatile uint16_t *)(info + TPL_GDT32_LIMIT) = gdt.limit;
    *(volatile uint64_t *)(info + TPL_GDT32_BASE) = gdt.base;
    *(volatile uint16_t *)(info + TPL_GDT64_LIMIT) = gdt.limit;
    *(volatile uint64_t *)(info + TPL_GDT64_BASE) = gdt.base;
    *(volatile uint16_t *)(info + TPL_IDT_LIMIT) = idtd.limit;
    *(volatile uint64_t *)(info + TPL_IDT_BASE) = idtd.base;

    return info;
}

static volatile uint32_t *smp_booted_flag(void) {
    return (volatile uint32_t *)((uint8_t *)phys_to_virt(tpl_phys) +
                                 (smp_trampoline_block - smp_trampoline_start) +
                                 TPL_BOOTED);
}

static volatile uint32_t *smp_progress_flag(void) {
    return (volatile uint32_t *)((uint8_t *)phys_to_virt(tpl_phys) +
                                 (smp_trampoline_block - smp_trampoline_start) +
                                 TPL_PROGRESS);
}

static bool smp_wait_for_boot(unsigned timeout_ms) {
    unsigned elapsed = 0;
    while (elapsed < timeout_ms) {
        if (*smp_booted_flag() == 1)
            return true;
        delay_us(200);
        elapsed += 1;
    }
    return *smp_booted_flag() == 1;
}

/* --- starting one processor -------------------------------------------- */

int smp_start_cpu(uint32_t lapic_id, unsigned index, void (*entry)(void),
                  void *arg) {
    if (!lapic_available())
        return -1;
    if (trampoline_install() != 0)
        return -1;

    /* A stack of its own.  Every processor that shares the boot processor's
     * stack will corrupt it the first time two of them take an interrupt.  The
     * heap's payloads are 16-byte aligned, which is what the ABI wants and all
     * this needs: there are no guard pages yet, so a page-aligned stack would
     * buy nothing. */
    uint8_t *stack = kmalloc(SMP_STACK_SIZE);
    if (stack == NULL)
        return -1;

    uint64_t stack_top = (uint64_t)stack + SMP_STACK_SIZE;

    if (smp_info_for(index, percpu_gs_base(index), stack_top,
                     (uint64_t)entry) == NULL) {
        kfree(stack);
        return -1;
    }

    /* Where a released processor starts executing.
     *
     * The startup IPI carries an eight-bit vector and the CPU turns it into a
     * real-mode entry address, so only the low bits of the trampoline's physical
     * address survive the trip and the page has to be low enough for the whole
     * of it to fit.  That is what limits the search below to the first megabyte,
     * and it is also why the search starts at frame 1 rather than frame 0: a
     * physical address of zero is indistinguishable from "no frame". */
    uint8_t vector = (uint8_t)((tpl_phys >> 12) & 0xff);

    /* Two startup IPIs, with a wait in between: one is the documented minimum
     * and two is what the SDM recommends, because a startup IPI that arrives
     * while the processor is still on its way out of reset is dropped.
     *
     * There is no INIT here, and that is a deliberate gap rather than an
     * oversight -- see smp_start_all(). */
    lapic_ipi_startup((uint8_t)lapic_id, vector);
    delay_ms(1);
    lapic_ipi_startup((uint8_t)lapic_id, vector);

    if (!smp_wait_for_boot(2000)) {
        kprintf("[warn] smp: LAPIC %u did not come up (the trampoline got to "
                "step %lu of 9)\n", lapic_id,
                (unsigned long)*smp_progress_flag());
        kfree(stack);
        return -1;
    }

    *smp_booted_flag() = 0;
    kprintf("smp: LAPIC %u is up as processor %u\n", lapic_id, index);

    (void)arg;
    return 0;
}

/* --- the idle loop every processor runs once it is up ------------------- */

static void smp_cpu_entry(void) {
    unsigned index = percpu_boot_index();
    percpu_t *self = percpu_self();

    self->online = true;
    self->started = true;
    percpu_set_online(index, true);

    /* Every CPU loads its own copies of the tables.  LGDT, LIDT and LTR are
     * per-processor state: the boot processor's copies are not inherited, so
     * without this the first interrupt on this CPU finds no IDT at all. */
    gdt_reload();
    idt_reload();

    kprintf("cpu: %u online (LAPIC %u)\n", index, self->lapic_id);

    /* Timers and the 8253 are routed to the boot processor's local APIC, so
     * this processor's LAPIC timer is armed separately rather than inherited.
     * Until the scheduler exists there is nothing to reschedule, so the loop is
     * halt-and-check: it has to exist and be per-CPU so phase 5 can replace it,
     * and halting keeps a machine with four processors from burning four cores
     * spinning. */
    percpu_arm_local_timer();
    sti();

    for (;;) {
        self->idle_ticks++;
        hlt();
    }
}

/* --- bringing the machine's processors up ------------------------------- */

int smp_start_all(void) {
    if (!lapic_available()) {
        kprintf("[warn] smp: no local APIC, staying on one processor\n");
        return -1;
    }

    /* The reset half of the INIT/SIPI sequence is deliberately not sent.
     *
     * INIT is a level interrupt, and its destination field is ignored: it resets
     * every processor including the one that sends it, which is why even the
     * "all but self" shorthand exists.  A released processor then fetches from
     * the reset vector at 0xfffffff0.  On hardware that address holds the
     * firmware's own wait-for-startup loop, which is exactly what the sequence
     * assumes.  On this machine it holds the *bootloader's* code instead, because
     * the firmware that would have been there was never loaded in the first
     * place, so a released application processor executes bootloader code, faults
     * with no IDT of its own, and takes the machine down before the startup IPI
     * can be sent.
     *
     * The kernel cannot fix that from its own page tables: a released processor
     * is in real mode with paging off, so the reset vector is a *physical* address
     * and page tables are not consulted at all.  Pointing it at this kernel's own
     * wait stub would mean writing to a page of BIOS memory that the firmware
     * still owns.
     *
     * So the startup IPIs go out without the reset.  A processor that was already
     * parked waiting for a startup IPI comes up; one that is not stays where it
     * is, and the boot processor says so instead of guessing. */
    unsigned started = 0;

    for (unsigned i = 1; i < percpu_count(); i++) {
        uint32_t lapic_id = percpu_lapic_id(i);

        if (smp_start_cpu(lapic_id, i, smp_cpu_entry, NULL) == 0)
            started++;
    }

    /* Point the boot processor's per-CPU area at itself through GS as well, so
     * that %gs:0 means the same thing here as it does on every other CPU. */
    percpu_gs_install_self();

    if (started == 0) {
        kprintf("[warn] smp: no application processor came up; "
                "%u online\n", percpu_online_count());
        return -1;
    }

    kprintf("smp: %u of %u processor(s) online\n", percpu_online_count(),
            percpu_count());
    return (int)started;
}
