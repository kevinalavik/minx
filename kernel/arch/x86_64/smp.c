/* minx - starting application processors.
 *
 * There are two ways to start an application processor and only one of them is
 * used here.
 *
 * The portable way is INIT/SIPI: reset the processor with an INIT interrupt,
 * release it, then send it a startup interrupt whose eight-bit vector names the
 * entry point in the first megabyte.  That needs a trampoline of our own, because
 * a released processor is in real mode with paging off and nothing above 1 MiB is
 * reachable.  `smp_start_cpu()` and `kernel/arch/x86_64/smp_trampoline.S` are that
 * path, and it is kept -- it is what a bootloader that does *not* start the
 * application processors would need -- but it is not the one used, because a
 * processor that has been released and not yet sent a startup interrupt runs
 * whatever is at the reset vector, and on this machine that is the bootloader's
 * code.  It faults with no IDT of its own and takes the machine down, and the
 * kernel cannot redirect that address from its own page tables because a released
 * processor never consults them.  `smp_start_cpu()` documents this at the point
 * the sequence is issued.
 *
 * The path actually used is the one the bootloader already provides.  Limine
 * starts every application processor itself -- each one runs Limine's own
 * trampoline out of the INIT/SIPI sequence -- and then parks it, spinning on one
 * field: `mp_info->goto_address`.  The kernel's job is to write its entry point
 * there and the processor jumps to it.  `smp_start_all()` does that.
 *
 * That matters for more than convenience.  A processor handed over this way is
 * running on the *bootloader's* page tables, so it also means the bootloader is
 * still in use by three processors after the kernel has switched CR3 -- and
 * `vmm_release_boot_tables()` therefore waits for the hand-off to finish before
 * it hands the bootloader's memory back to the allocator.
 */
#include "smp.h"

#include "apic.h"
#include "boot.h"
#include "cpu.h"
#include "delay.h"
#include "io.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "panic.h"
#include "pagemap.h"
#include "percpu.h"
#include "pmm.h"
#include "string.h"
#include "types.h"
#include "pagetables.h"
#include "vectors.h"
#include "vmm.h"

#include <limine.h>

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

static uint64_t tpl_phys;

/* --- the per-processor hand-off state ------------------------------------ */

/* What a parked processor needs before it can join the kernel: the address space
 * to switch to, the CR4 to adopt, which slot is its own, and where its IST
 * stacks are.
 *
 * The bootloader hands over three fields, and all three are live:
 *
 *   mp_info->reserved       the stack the processor switches to
 *   mp_info->goto_address   the entry point, polled until it becomes non-zero
 *   mp_info->extra_argument free for the kernel
 *
 * Only `extra_argument` is actually free, so it carries this slot's address --
 * as a *direct map* address, because the processor is still on the bootloader's
 * page tables and the only thing it has been shown to reach in this kernel is
 * the direct map.  `reserved` has to be a stack the processor can use before it
 * has changed anything, which the direct map is. */
typedef struct {
    uint64_t pml4;
    uint64_t cr4;
    uint64_t index;
    uint64_t ist1_top;
    uint64_t ist2_top;
} smp_ap_slot_t;

static smp_ap_slot_t ap_slot[MAX_CPU_COUNT];

/* The kernel entry point every processor ends up in: in long mode, on a stack the
 * bootloader already switched it to, with GS not yet pointing anywhere useful. */
static void smp_cpu_entry(unsigned index);

/* --- the INIT/SIPI path, not used on this bootloader ---------------------- */

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

/* The PDPTE the trampoline needs.
 *
 * With PAE enabled CR3 names four PDPTE entries, not 512 PML4 entries, so the
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

/* Start one application processor through INIT/SIPI.
 *
 * Not called on this bootloader, which starts the application processors itself
 * and hands them over through `mp_info->goto_address` -- see smp_start_all() and
 * the note at the top of this file.  It stays because it is the path a bootloader
 * that leaves them to the kernel would need, and it is the only implementation
 * that does not depend on the bootloader's cooperation.
 *
 * It is not called automatically on purpose: the INIT sequence it issues is what
 * `smp_start_all()` would otherwise have to undo, and on this machine it takes the
 * machine down (see the comment on the sequence below). */
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
     * of it to fit. */
    uint8_t vector = (uint8_t)((tpl_phys >> 12) & 0xff);

    /* Reset, release, then wake.
     *
     * INIT is a level interrupt whose destination field is ignored, so a
     * *targeted* one resets this processor too and the machine restarts in the
     * middle of bring-up.  The shorthand 10, "all excluding self", is the one
     * encoding INIT honours, and that is what goes out.
     *
     * The release is a second write with the assert bit clear: while the line is
     * asserted the processors stay reset and accept no startup IPI.  What a
     * released processor runs next is the reset vector at 0xfffffff0, where
     * hardware holds the firmware's wait-for-startup loop and this machine holds
     * the bootloader's code -- so a processor that gets that far executes
     * bootloader code, faults with no IDT of its own and takes the machine down.
     * The kernel cannot redirect that address from its own page tables either: a
     * released processor is in real mode with paging off and never consults
     * them.  That is why this path is not the one used. */
    lapic_ipi_init_all_but_self();
    lapic_ipi_init_deassert_all_but_self();

    /* Two startup IPIs: one is the documented minimum and two is what the SDM
     * recommends, because a startup IPI that arrives while the processor is
     * still on its way out of reset is dropped. */
    lapic_ipi_startup((uint8_t)lapic_id, vector);
    pause_cpu();
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

/* --- inter-processor interrupts ------------------------------------------ */

/* Per processor, and only ever touched by its owner: a counter of the IPIs this
 * one has handled, which is how the self test knows the IPI path delivered
 * something rather than the sender just believing it did. */
static uint64_t ipi_received;

uint64_t smp_ipis_received(void) {
    return __atomic_load_n(&ipi_received, __ATOMIC_ACQUIRE);
}

int smp_send_ipi(uint8_t vector, uint32_t lapic_id) {
    if (!lapic_available()) {
        kprintf("[warn] smp: cannot send an IPI with no local APIC\n");
        return -1;
    }

    lapic_ipi((uint8_t)lapic_id, vector);
    return 0;
}

int smp_ipi_self(uint8_t vector) {
    if (!lapic_available())
        return -1;

    lapic_ipi((uint8_t)percpu_lapic_id(percpu_self_index()), vector);
    return 0;
}

int smp_ipi_all_but_self(uint8_t vector) {
    if (!lapic_available())
        return -1;

    lapic_ipi_all_but_self(vector);
    return 0;
}

int smp_ipi_all(uint8_t vector) {
    if (!lapic_available())
        return -1;

    lapic_ipi_all_cpus(vector);
    return 0;
}

int smp_ipi_cpu(unsigned index, uint8_t vector) {
    if (index >= percpu_count())
        return -1;
    return smp_send_ipi(vector, percpu_lapic_id(index));
}

int smp_kick_cpu(unsigned index) {
    /* Kicking this processor would deadlock if the caller had interrupts off, and
     * would not be an IPI at all if it had them on -- either way the work is
     * already ours, so running it is both simpler and correct. */
    if (index == percpu_self_index()) {
        percpu_run_pending();
        return 0;
    }

    return smp_ipi_cpu(index, VECTOR_IPI_RESCHED);
}

unsigned smp_online_indices(unsigned *out, unsigned max) {
    unsigned n = 0;

    for (unsigned i = 0; i < percpu_count() && n < max; i++)
        if (percpu_is_online(i))
            out[n++] = i;

    return n;
}

/* The handler every IPI vector below is installed on.
 *
 * A common entry on purpose: the point of an IPI is to make the *receiving*
 * processor do something, and the thing it does is read its own per-CPU state.  So
 * the handler counts the arrival and then runs the work for the vector, and the
 * work looks at `percpu_self()` -- which resolves through GS to the processor that
 * is actually running, not to the one that sent it. */
static void smp_ipi_common(regs_t *frame) {
    (void)frame;

    __atomic_add_fetch(&ipi_received, 1, __ATOMIC_ACQ_REL);
    percpu_count_ipi();

    /* End of interrupt, before anything else can return.
     *
     * This is the one thing an xAPIC handler must not forget, and forgetting it
     * is invisible on the processor that sent the interrupt -- only the receiver
     * goes deaf.  The interrupt-acknowledge sequence set an in-service bit, and
     * nothing clears it but a write to the end-of-interrupt register; leave it set
     * and this processor refuses every further interrupt of the same priority,
     * which for the local APIC means all of the fixed vectors: its own timer, every
     * IPI, everything.  It does not crash and it does not report anything, it just
     * stops being interrupted, and its idle loop's `hlt` stops returning. */
    lapic_eoi();
}

/* Reschedule: the receiving processor has work waiting for it, or the sender has
 * decided to preempt it.  This is the vector that makes a processor useful: it is
 * how it finds out about work it was not told about by going round itself. */
static void smp_ipi_resched(regs_t *frame) {
    smp_ipi_common(frame);
    percpu_run_pending();
}


/* TLB shootdown: another processor changed a mapping, so this processor's cached
 * translation for it is stale.  The mapping is a kernel one and the kernel's pages
 * are shared, so the flush is a reloaded CR3 -- which is why it can be done from
 * an interrupt without a lock.  `vmm_tlb_flush_local()` is where a proper invalidation
 * goes once there is a page table to walk. */
static void smp_ipi_tlb(regs_t *frame) {
    smp_ipi_common(frame);
    pt_write_cr3(pt_read_cr3());
}

/* Panic: stop every other processor.  A panic has to stop the machine, not just
 * the processor that noticed, and a processor that keeps running will take
 * interrupts against structures a panicked kernel is no longer maintaining.  So it
 * parks with interrupts off, which also stops the preemption tick. */
static void smp_ipi_halt(regs_t *frame) {
    smp_ipi_common(frame);

    cli();
    for (;;)
        hlt();
}

/* Leave the idle loop.  The wake is the interesting part: the receiving processor
 * is in `hlt`, and `hlt` only wakes for an interrupt, so an IPI to a parked
 * processor is also how it is taken out of `hlt`.  Anything queued for it runs
 * here too, because a processor that is woken with nothing to do goes straight
 * back to sleep. */
static void smp_ipi_wake(regs_t *frame) {
    smp_ipi_common(frame);
    percpu_run_pending();
}

static isr_handler_t const ipi_handlers[] = {
    [VECTOR_IPI_TLB - VECTOR_IPI_FIRST]      = smp_ipi_tlb,
    [VECTOR_IPI_RESCHED - VECTOR_IPI_FIRST]  = smp_ipi_resched,
    [VECTOR_IPI_HALT - VECTOR_IPI_FIRST]     = smp_ipi_halt,
    [VECTOR_IPI_WAKE - VECTOR_IPI_FIRST]     = smp_ipi_wake,
};

void smp_ipi_handlers_install(void) {
    for (unsigned v = VECTOR_IPI_FIRST; v <= VECTOR_IPI_LAST; v++) {
        isr_handler_t handler = ipi_handlers[v - VECTOR_IPI_FIRST];

        if (handler != NULL)
            isr_install_handler((uint8_t)v, handler);
    }
}

/* --- what every processor runs once it has its own address space ---------- */

static void smp_cpu_entry(unsigned index) {
    percpu_t *self = percpu_by_index(index);

    /* The IDT first, before anything that can fault.  This processor has just
     * changed address space out from under itself and is still running on the
     * bootloader's descriptors, so the next few instructions are the most likely
     * place for something to go wrong -- and with no IDT at all, any fault is a
     * triple fault, which takes the machine down instead of reporting itself. */
    idt_reload();
    /* This processor's own TSS, with its own IST stacks and its own descriptor.
     * A shared descriptor would already be busy: loading a task register sets
     * that bit in the table, so the boot processor's `ltr` leaves the entry
     * marked and this one is refused with #GP. */
    tss_init_for(index, ap_slot[index].ist1_top, ap_slot[index].ist2_top);
    gdt_write_tss_descriptor(index, (uint64_t)&kernel_tss[index],
                             sizeof(kernel_tss[0]) - 1);
    gdt_reload_for(index);

    /* After the segment reload, not before: loading a segment selector in 64-bit
     * mode zeroes that segment's hidden base, so the `mov` that installs our
     * data selector also clears whatever GS was pointing at. */
    percpu_gs_install_index(index);

    if (self != NULL) {
        self->online = true;
        self->started = true;
    }
    percpu_set_online(index, true);

    kprintf("cpu: %u online (LAPIC %u)\n", index,
            self != NULL ? self->lapic_id : 0);

    /* Timers and the 8253 are routed to the boot processor's local APIC, so this
     * processor has to arm its own or nothing on it ever ticks. */
    percpu_arm_local_timer();
    sti();

    for (;;) {
        if (self != NULL)
            self->idle_ticks++;
        hlt();
    }
}

/* --- the hand-off a parked processor takes -------------------------------- */

/* Adopt the kernel's CR4 and CR3, then call smp_cpu_entry().
 *
 * This cannot be ordinary C.  Between replacing CR3 and reaching the kernel's
 * entry there must be no compiler-generated access to memory the new tables do
 * not map, and nothing may be touched that assumes the descriptors are ours --
 * the bootloader's table is still loaded.  So the whole sequence is written out,
 * and the call is made from the assembly for the same reason.
 *
 * The stack and the descriptors are not dealt with here: the bootloader has
 * already switched this processor onto a stack, and that stack lives in the direct
 * map, which both sets of page tables map.  The entry point installs the IDT, the
 * table and the task register before it touches anything that needs them.
 *
 * GS is deliberately left alone.  Setting it from here would be undone two
 * instructions later by the `mov` that loads our data selector, because loading a
 * segment selector in 64-bit mode zeroes that segment's hidden base. */
__attribute__((noreturn))
static void smp_ap_switch(uint64_t pml4_phys, uint64_t cr4, unsigned index) {
    uint64_t entry = (uint64_t)(uintptr_t)smp_cpu_entry;
    uint32_t idx = (uint32_t)index;

    __asm__ volatile(
        "movq %[cr4], %%rcx\n\t"
        "movq %%rcx, %%cr4\n\t"
        "movq %[pml4], %%rcx\n\t"
        "movq %%rcx, %%cr3\n\t"
        "movl %[idx], %%edi\n\t"
        "movq %[entry], %%r11\n\t"
        "call *%%r11\n\t"
        "ud2\n\t"
        :
        : [pml4] "r"(pml4_phys), [cr4] "r"(cr4), [idx] "r"(idx),
          [entry] "r"(entry)
        : "rax", "rcx", "rdx", "rdi", "r11", "memory");

    /* The assembly never comes back: it jumps straight into the kernel's entry
     * and that never returns either.  This is here only so the compiler does not
     * treat the fall-through as reachable. */
    __builtin_unreachable();
}

/* Where the bootloader parks an application processor, and where it arrives once
 * the kernel writes its entry point.
 *
 * It is running on the bootloader's page tables with the bootloader's GDT, on a
 * stack the bootloader chose, so kernel data is reachable only through the direct
 * map and the descriptors have to be replaced before anything compiled against
 * them runs. */
static void smp_ap_entry(struct limine_mp_info *info) {
    smp_ap_slot_t *slot = (smp_ap_slot_t *)info->extra_argument;

    smp_ap_switch(slot->pml4, slot->cr4, (unsigned)slot->index);
}

/* --- bringing the machine's processors up ------------------------------- */

int smp_start_all(void) {
    if (!lapic_available()) {
        kprintf("[warn] smp: no local APIC, staying on one processor\n");
        return -1;
    }

    if (percpu_count() <= 1) {
        kprintf("smp: only one processor; nothing to start\n");
        return 0;
    }

    const struct limine_mp_response *mp = boot_mp_response();
    if (mp == NULL || mp->cpus == NULL) {
        kwarn("smp: the bootloader reported no per-processor table, so there is "
              "nothing to hand off to");
        return -1;
    }

    page_table_t *tables = vmm_tables();
    if (tables == NULL || tables->pml4 == 0) {
        kwarn("smp: no kernel page tables, so a processor cannot be switched "
              "onto the kernel's address space");
        return -1;
    }

    unsigned started = 0;

    for (unsigned i = 1; i < percpu_count(); i++) {
        struct limine_mp_info *info = NULL;

        for (uint64_t n = 0; n < mp->cpu_count; n++) {
            if (mp->cpus[n] != NULL &&
                mp->cpus[n]->lapic_id == percpu_lapic_id(i)) {
                info = mp->cpus[n];
                break;
            }
        }

        if (info == NULL) {
            kprintf("[warn] smp: the bootloader has no slot for LAPIC %u, so "
                    "processor %u is not started\n", percpu_lapic_id(i), i);
            continue;
        }

        /* Its own stack, and its own per-CPU area, before it is told to arrive.
         * Two processors sharing either would corrupt the other the first time
         * both took an interrupt.
         *
         * The stack is handed over as a *direct map* address: the processor is
         * switched onto it before it has changed address space, so it has to be
         * valid on the bootloader's tables as well as ours.  The direct map is
         * mapped by both. */
        uint8_t *stack = kmalloc(SMP_STACK_SIZE);
        if (stack == NULL) {
            kprintf("[warn] smp: no stack for processor %u\n", i);
            continue;
        }

        /* Its own IST stacks as well: the IST pointer in a TSS is an absolute
         * address the CPU puts on the stack without going through the fault
         * handler, so a shared stack would mean one processor's double fault
         * landing in the middle of another's.  These come from the heap, which
         * hands out direct map addresses -- valid on the bootloader's tables
         * before the switch and on the kernel's after it. */
        uint8_t *ist1 = kmalloc(IST_STACK_SIZE);
        uint8_t *ist2 = kmalloc(IST_STACK_SIZE);
        if (ist1 == NULL || ist2 == NULL) {
            kprintf("[warn] smp: no IST stacks for processor %u\n", i);
            kfree(ist1);
            kfree(ist2);
            continue;
        }

        ap_slot[i].pml4 = tables->pml4;
        ap_slot[i].cr4 = pt_read_cr4();
        ap_slot[i].index = i;
        ap_slot[i].ist1_top = (uint64_t)ist1 + IST_STACK_SIZE;
        ap_slot[i].ist2_top = (uint64_t)ist2 + IST_STACK_SIZE;

        uint64_t slot_addr = phys_to_virt(
            vmm_walk_translate((uint64_t)(void *)&ap_slot[i], NULL));

        /* `goto_address` has to be written last and on its own.  The processor
         * polls that one field and jumps the instant it sees a non-zero address,
         * so everything its entry point reads has to be visible before it -- and
         * the compiler is free to reorder stores across a normal assignment. */
        __asm__ volatile("" ::: "memory");
        info->reserved = phys_to_virt(virt_to_phys(stack)) + SMP_STACK_SIZE;
        info->extra_argument = slot_addr;
        __asm__ volatile("" ::: "memory");
        info->goto_address = (limine_goto_address)smp_ap_entry;

        started++;
        kprintf("smp: handed LAPIC %u its entry point (processor %u)\n",
                percpu_lapic_id(i), i);
    }

    if (started == 0)
        return -1;

    /* Wait for them, with a bound: a processor that never arrives has to be
     * reported rather than waited for. */
    for (unsigned waited = 0; waited < 2000; waited++) {
        if (percpu_online_count() >= percpu_count())
            break;
        delay_ms(1);
    }

    kprintf("smp: %u of %u processor(s) online\n", percpu_online_count(),
            percpu_count());

    return (int)(percpu_online_count() - 1);
}
