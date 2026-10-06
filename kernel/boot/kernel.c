/* minx - bootloader handover and kernel start-up.
 *
 * Limine fills in the `.response` pointers of the request structures below and
 * then calls kmain().  Everything the kernel needs from the firmware (memory
 * map, framebuffer, RSDP, SMP info, the initrd module) is read straight out of
 * those responses.
 */
#include "boot.h"

#include <limine.h>

#include "console.h"
#include "conio.h"
#include "cpu.h"
#include "delay.h"
#include "fb.h"
#include "hhdm.h"
#include "input.h"
#include "io.h"
#include "irq.h"
#include "keyboard.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "panic.h"
#include "percpu.h"
#include "pmm.h"
#include "power.h"
#include "serial.h"
#include "smp.h"
#include "syscall.h"
#include "task.h"
#include "string.h"
#include "test.h"
#include "timer.h"
#include "types.h"
#include "vmm.h"

#include "vectors.h"

#include "acpi.h"
#include "apic.h"

/* Size of the boot CPU's kernel stack.  Generous, because deep call chains in
 * the filesystem and the scheduler need the headroom. */
#define BOOT_STACK_SIZE (64u * 1024u)

/* --- Limine requests ---------------------------------------------------- */

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests_start_marker")))
static volatile uint64_t limine_requests_start_marker[] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_entry_point_request entry_point_request = {
    .id = LIMINE_ENTRY_POINT_REQUEST_ID,
    .revision = 0,
    .response = NULL,
    .entry = kmain,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

/* The higher-half direct map's offset.  Limine randomises it, so phys_to_virt()
 * cannot be a compile-time expression; every caller depends on this being
 * answered before anything touches physical memory. */
__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request limine_hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .response = NULL,
    .flags = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests_end_marker")))
static volatile uint64_t limine_requests_end_marker[] =
    LIMINE_REQUESTS_END_MARKER;

/* --- Handover record --------------------------------------------------- */

/* Filled in from the Limine responses and read by later phases; see boot.h. */
boot_info_t boot_info;

/* The memory map, kept reachable after the request block's response pointer is
 * no longer interesting.  The physical memory manager needs it to decide which
 * frames the firmware is willing to give up. */
struct limine_memmap_response *boot_memmap(void) {
    return memmap_request.response;
}

/* The per-processor table.  The kernel writes each processor's entry point into
 * it rather than starting the processors itself: the bootloader has already
 * started them and parked each one on its own slot's goto_address. */
const struct limine_mp_response *boot_mp_response(void) {
    return mp_request.response;
}
/* CR3 as it was handed over by the bootloader, so the tables behind it can be
 * released once the kernel's own are in place. */
static uint64_t old_cr3;

uint64_t boot_old_cr3(void) { return old_cr3; }
void boot_set_old_cr3(uint64_t value) { old_cr3 = value; }




/* The kernel command line, e.g. "minx.quiet=1 minx.test=1". */
static char kernel_cmdline[512];
boot_cmdline_t boot_cmdline = { kernel_cmdline, 0, false, false, { 0 }, 0 };

/* --- helpers ------------------------------------------------------------ */

int atoi_simple(const char *s) {
    char *end = NULL;
    long v = (long)strtoull(s, &end, 10);
    return (int)v;
}

static void framebuffer_start(void) {
    struct limine_framebuffer_response *resp = framebuffer_request.response;
    if (resp == NULL || resp->framebuffer_count < 1) {
        kprintf("[warn] no framebuffer: the graphical console is unavailable\n");
        return;
    }

    struct limine_framebuffer *fb = resp->framebuffers[0];
    if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB) {
        kprintf("[warn] framebuffer is not RGB (model %u); "
                "using the serial console only\n", fb->memory_model);
        return;
    }

    /* Through the direct map, not at the framebuffer's own address.
     *
     * Limine hands over the framebuffer at its *physical* address, which for a PCI BAR is
     * a hole low in the 32-bit range -- 0xf0000000 on this machine.  That is PML4 slot 0,
     * which is the half a user address space owns: vmm_address_space_create() gives a
     * process slots 0..255 and copies slots 256..511 from the kernel, so a user task has
     * *nothing* mapped there.
     *
     * The console therefore became unusable from any user address space, and the place
     * that matters is the ring-3 page-fault handler: it runs in the faulting task's
     * address space, so writing one character of its own report faulted, which is a double
     * fault, and the double-fault handler wrote its report the same way, which is a triple
     * fault.  A user program that faulted took the machine down before printing anything,
     * which is why every ring-3 failure in this kernel looked like a silent death rather
     * than a report.
     *
     * The direct map covers the whole of physical memory and lives in the kernel half,
     * which every address space shares, so the same bytes are reachable from all of them.
     * The identity mapping at the framebuffer's own address is left in place: it is what
     * vmm_init() creates it for, and it is harmless. */
    if (fb_init((uint8_t *)phys_to_virt(virt_to_phys((uint64_t)fb->address)),
                (uint32_t)fb->width,
                (uint32_t)fb->height, (uint32_t)fb->pitch, fb->bpp, true,
                fb->red_mask_size, fb->red_mask_shift, fb->green_mask_size,
                fb->green_mask_shift, fb->blue_mask_size,
                fb->blue_mask_shift) != 0) {
        kprintf("[warn] framebuffer %ux%u @ %u bpp is not supported\n",
                (uint32_t)fb->width, (uint32_t)fb->height, fb->bpp);
        return;
    }

    boot_info.framebuffer_addr = (uint64_t)fb->address;
    boot_info.framebuffer_width = (uint32_t)fb->width;
    boot_info.framebuffer_height = (uint32_t)fb->height;
    boot_info.framebuffer_pitch = (uint32_t)fb->pitch;
    boot_info.framebuffer_bpp = fb->bpp;
}

static void parse_cmdline(void) {
    struct limine_executable_cmdline_response *resp = cmdline_request.response;
    const char *src = resp != NULL && resp->cmdline != NULL ? resp->cmdline : "";

    strlcpy(kernel_cmdline, src, sizeof(kernel_cmdline));
    boot_cmdline.len = (uint32_t)strlen(kernel_cmdline);

    /* Very small option lookup: "key" or "key=value", whitespace separated. */
    const char *p = kernel_cmdline;
    while (*p) {
        while (*p == ' ')
            p++;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        size_t len = (size_t)(p - start);

        static const char *const known[] = {
            "minx.quiet", "minx.test", "minx.init", "minx.loglevel",
        };
        for (size_t i = 0; i < array_len(known); i++) {
            size_t klen = strlen(known[i]);
            if (len >= klen && strncmp(start, known[i], klen) == 0 &&
                (len == klen || start[klen] == '=')) {
                switch (i) {
                case 0: boot_cmdline.quiet = (len == klen || start[klen + 1] == '1'); break;
                case 1: boot_cmdline.test = (len == klen || start[klen + 1] == '1'); break;
                case 2:
                    strlcpy(boot_cmdline.init, start + klen + 1,
                            sizeof(boot_cmdline.init));
                    break;
                case 3:
                    boot_cmdline.loglevel = atoi_simple(start + klen + 1);
                    break;
                default: break;
                }
            }
        }
    }
}

static void print_banner(const struct limine_bootloader_info_response *info) {
    kprintf("\n");
    kprintf("minx - a Unix-like OS for x86-64\n");
    kprintf("booted by %s %s\n",
            info != NULL && info->name ? info->name : "unknown",
            info != NULL && info->version ? info->version : "");
    kprintf("kernel image %016lx-%016lx\n", (uint64_t)__kernel_start,
            (uint64_t)__kernel_end);
}

static void report_memmap(const struct limine_memmap_response *resp) {
    static const char *const names[] = {
        "usable",     "reserved",        "ACPI reclaimable", "ACPI NVS",
        "bad memory", "bootloader",      "kernel + modules", "framebuffer",
        "reserved(mapped)",
    };

    boot_info.memmap_entries = (uint32_t)resp->entry_count;

    kprintf("memory map (%lu entries):\n", resp->entry_count);
    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        const char *name = e->type < array_len(names) ? names[e->type] : "?";
        kprintf("  %016lx-%016lx  %s\n", e->base, e->base + e->length, name);
    }
}

static void report_initrd(const struct limine_module_response *resp) {
    if (resp == NULL || resp->module_count < 1) {
        kprintf("[warn] no initrd module was loaded\n");
        return;
    }
    struct limine_file *m = resp->modules[0];
    boot_info.initrd_base = (uint64_t)m->address;
    boot_info.initrd_size = m->size;
    kprintf("initrd: %s, %lu bytes at %p\n", m->path ? m->path : "?",
            m->size, m->address);
}

/* Build the physical allocator and then the kernel's own page tables.
 *
 * Order matters: ACPI has to be parsed first because it names the APIC windows,
 * those windows have to be *requested* before the tables exist, and only then
 * can the tables be built.  The 8259s are remapped before all of it so that a
 * stray line during bring-up lands on a vector that exists. */
static void cpu_interrupts_init(void) {
    /* ACPI first: it names the APIC windows and the processors, and everything
     * below -- the per-CPU table, the MMIO requests, the routing -- depends on
     * what it found. */
    if (acpi_init(boot_info.rsdp, boot_cmdline.loglevel >= 1) != 0)
        kwarn("ACPI discovery failed; falling back to the default APIC address");

    unsigned ncpus = 0;
    const acpi_cpu_t *cpus = acpi_cpu_table(&ncpus);
    unsigned nioapics = 0;
    const acpi_ioapic_t *ioapics = acpi_ioapic_table(&nioapics);

    /* Register every processor ACPI listed, so the online count and the
     * per-CPU table agree even before phase 4 starts the application CPUs. */
    if (ncpus == 0) {
        (void)percpu_register(boot_info.bsp_lapic_id);
    } else {
        for (unsigned i = 0; i < ncpus; i++) {
            (void)percpu_register(cpus[i].lapic_id);
            kprintf("cpu: %u: ACPI id %u, LAPIC id %u%s\n", i, cpus[i].id,
                    cpus[i].lapic_id, cpus[i].enabled ? "" : " (disabled)");
        }
    }
    boot_info.acpu_count = ncpus;
    percpu_by_index(PERCPU_BOOTSTRAP)->online = true;
    percpu_by_index(PERCPU_BOOTSTRAP)->started = true;

    kprintf("cpu: %u processor(s) known, %u online\n", percpu_count(),
            percpu_online_count());

    /* Remap the 8259s out of the exception vector range before they can fire. */
    pic_init();

    /* The local APIC is a single 4 KiB page at a firmware-declared address, and
     * the IOAPIC likewise.  Neither is in the direct map, because the direct map
     * is for RAM: device registers have to be mapped uncacheable on purpose. */
    if (acpi_has_lapic())
        vmm_map_mmio(acpi_lapic_address(), page_size, "local APIC");
    for (unsigned i = 0; i < nioapics; i++) {
        if (ioapics[i].base != 0)
            vmm_map_mmio(ioapics[i].base, page_size, "IOAPIC");
    }
    /* The well-known defaults, in case firmware was silent about them. */
    vmm_map_mmio(0xfee00000, page_size, "local APIC (default)");
    vmm_map_mmio(0xfec00000, page_size, "IOAPIC (default)");

    /* Now the physical allocator exists and the tables can be built with it. */
    if (pmm_init() != 0) {
        kwarn("the physical allocator failed to start; staying on the "
              "bootloader's page tables");
    } else if (vmm_init() != 0) {
        kwarn("could not build the kernel's page tables; staying on the "
              "bootloader's");
    } else {
        boot_info.have_vmm = true;

        /* Hand the heap real frames now that there is a physical allocator;
         * the 4 MiB static arena stays as the floor so a failed pmm_init() is
         * survivable rather than fatal. */
        kmalloc_attach_pmm(HEAP_INITIAL_BYTES);
    }

    bool have_apic = lapic_init() == 0;
    bool have_ioapic = have_apic && ioapic_init() == 0;

    if (have_ioapic) {
        /* Hand the sixteen legacy lines to the IOAPIC before masking the 8259s.
         * The order matters: while the lines are still delivered by the PICs the
         * kernel has to be able to un-mask and re-mask a line in irq_enable(),
         * and after pic_disable() that path goes through the IOAPIC instead. */
        /* The ISA lines are edge-triggered pulses; level-triggered entries
         * would make the chip re-assert anything that leaves its line high,
         * which on the PS/2 controller means a keystroke handled forever. */
        if (ioapic_route_legacy_lines(VECTOR_IRQ0, lapic_id(), false) == 0) {
            irq_set_ioapic_delivery(true);

            /* Every line now has a redirection entry, so the legacy PICs can
             * never be unmasked again. */
            pic_disable();
            boot_info.interrupts_via_ioapic = true;
        } else {
            kwarn("the IOAPIC could not take the legacy lines; keeping the "
                  "8259s in charge of them");
        }
    } else if (have_apic) {
        kwarn("no IOAPIC: falling back to the legacy PIC for IRQ routing");
    } else {
        kwarn("no local APIC: staying on the 8259s, and the LAPIC timer cannot "
              "be used");
    }

    /* Install the handler before the timer is armed: a periodic timer that
     * fires into an unhandled vector would be reported as an unexpected
     * interrupt on every tick. */
    lapic_timer_interrupt_init();

    if (timer_init() != 0)
        kwarn("no LAPIC timer: the kernel will not preempt");
    else
        boot_info.have_timer = true;

    /* The 8253 keeps a time base running regardless, which is what the uptime
     * reporting and the scheduler's timeouts are built on. */
    if (pit_irq_init() != 0)
        kwarn("the PIT timer could not be started either");

    rtc_init();
    boot_info.have_rtc = true;
}

/* Start every processor the firmware described.
 *
 * After the interrupts, the allocator and the kernel's own page tables, and
 * before the console starts taking input, because an application processor that
 * fails to come up is a much better thing to hear about in the boot log than to
 * discover later from a machine that is mysteriously single-CPU.
 *
 * Each processor that comes up installs its own GDT, IDT and task register,
 * points GS at its own per-CPU area, arms its own local APIC timer -- the 8253's
 * line is routed to the boot processor -- and then enters the scheduler as the
 * idle task of its own run queue.
 *
 * The scheduler is initialised first, and on the boot processor, because its run
 * queues and idle tasks have to exist before any processor can be put on one.  The
 * boot processor's own context becomes a task at that point, which is what makes
 * everything above this a time slice rather than a straight line. */
static void smp_init(void) {
    /* Handlers before anything else, and before any of the early returns below.
     *
     * A vector with nothing installed on it is an unreported triple fault, and a
     * processor can take an IPI whether or not there are any other processors --
     * on a one-processor machine the only IPI there is to itself.  Putting this
     * after the single-processor check left exactly that case with an unhandled
     * vector 0x34, which the kernel reported as "unexpected interrupt vector
     * ignored" and then counted the IPI as never having arrived. */
    smp_ipi_handlers_install();

    sched_init();

    if (percpu_count() <= 1) {
        kprintf("smp: only one processor; nothing to start\n");
        return;
    }

    if (!boot_info.have_vmm) {
        kwarn("smp: no kernel page tables, so the AP trampoline cannot be "
              "mapped; staying on one processor");
        return;
    }

    int up = smp_start_all();

    /* Only now is nothing running on the bootloader's page tables, so only now
     * can its frames go back to the allocator. */
    vmm_release_boot_tables();

    if (up < 0)
        kwarn("smp: no application processor came up; the kernel is "
              "single-CPU\n");
}

/* The idle loop: pump the input queue and halt between events until something
 * wakes us.  Declared before kmain_stage2, which starts it. */
static void interactive_console(void);

static char heartbeat_buf[24];

/* Format an unsigned into a small rotating buffer and return it as a string.
 * Only used by the heartbeat, which cannot afford a kprintf on every tick. */
static const char *utoa_to_buf(uint64_t value) {
    static unsigned slot;

    slot = (slot + 1) % 2;
    char *buf = &heartbeat_buf[slot * 12];
    if (utoa(value, 10, false, buf, 12) < 0)
        buf[0] = '\0';
    return buf;
}

/* A heartbeat on the serial port alone.  It goes straight to COM1 rather than
 * through the console so it does not disturb what is on screen, and it makes the
 * interrupt path observable even when the keyboard is not answering. */
#define HEARTBEAT_MS 5000

static void heartbeat(uint64_t now_ms, uint64_t *last_ms) {
    if (now_ms - *last_ms < HEARTBEAT_MS)
        return;
    *last_ms = now_ms;

    serial_puts("[hb] up ");
    serial_puts(utoa_to_buf(now_ms));
    serial_puts(" ms, pit ");
    serial_puts(utoa_to_buf(pit_uptime_ticks()));
    serial_puts(" ticks, irq0 ");
    serial_puts(utoa_to_buf((uint64_t)irq_count(0)));
    serial_puts(" irq1 ");
    serial_puts(utoa_to_buf((uint64_t)irq_count(1)));
    serial_puts(" scancodes ");
    serial_puts(utoa_to_buf(keyboard_scancodes()));
    serial_puts(" events ");
    serial_puts(utoa_to_buf(keyboard_events()));
    serial_puts(", input queued ");
    serial_puts(utoa_to_buf(input_pending()));
    serial_puts(", if=");
    serial_puts(interrupts_enabled() ? "1" : "0");
    serial_puts(" picmask=");
    serial_puts(utoa_to_buf(pic_irq_mask()));

    /* The scheduler's own view of this processor: how many preemption ticks it
     * has taken and how many times it has switched.  Without these a processor
     * that has stopped ticking looks identical to one that is merely idle. */
    sched_stats_t st;
    sched_get_stats(percpu_self_index(), &st);
    serial_puts(" sched ");
    serial_puts(utoa_to_buf(st.ticks));
    serial_puts("t/");
    serial_puts(utoa_to_buf((uint64_t)st.switches));
    serial_puts("s q");
    serial_puts(utoa_to_buf((uint64_t)st.queued));
    serial_puts("\n");
}

/* Runs on the kernel's own stack; kmain() switches to it and never returns. */
static void kmain_stage2(void) {
    print_banner(bootloader_info_request.response);

    /* The bootloader zeroes limine_base_revision[2] when it understands the
     * revision the kernel asked for, so "supported" means it reads zero. */
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        panic("the bootloader speaks a newer Limine base revision (%lu) "
              "than this kernel supports",
              limine_base_revision[2]);
    }

    if (entry_point_request.response == NULL)
        panic("the bootloader did not report entry point support");

    if (memmap_request.response != NULL)
        report_memmap(memmap_request.response);
    else
        kprintf("[warn] no memory map response\n");

    if (rsdp_request.response != NULL) {
        boot_info.rsdp = rsdp_request.response->address;
        boot_info.rsdp_phys = virt_to_phys(boot_info.rsdp);
        kprintf("RSDP at %p (physical %#lx)\n", boot_info.rsdp, boot_info.rsdp_phys);
    } else {
        kprintf("[warn] no RSDP: ACPI will not be available\n");
    }

    if (mp_request.response != NULL) {
        struct limine_mp_response *mp = mp_request.response;
        boot_info.cpu_count = (uint32_t)mp->cpu_count;
        boot_info.bsp_lapic_id = mp->bsp_lapic_id;
        kprintf("SMP: %lu CPUs, BSP LAPIC id %u\n", mp->cpu_count,
                mp->bsp_lapic_id);
    } else {
        boot_info.cpu_count = 1;
        kprintf("SMP: single CPU (no MP response)\n");
    }

    report_initrd(module_request.response);

    cpu_interrupts_init();

    const fb_info_t *fbi = fb_get_info();
    if (fbi->ready) {
        kprintf("console: framebuffer %ux%u, %u bpp; text %ux%u cells\n",
                fbi->width, fbi->height, fbi->bpp, console_cols(),
                console_rows());
    } else {
        kprintf("console: serial only (no framebuffer)\n");
    }

    kprintf("kernel command line: \"%s\"\n", boot_cmdline.str);
    kprintf("heap: %lu KiB in %u region(s), %lu KiB free\n",
            (unsigned long)(kmalloc_total_bytes() / 1024),
            kmalloc_region_count(),
            (unsigned long)(kmalloc_free_bytes() / 1024));
    kprintf("tsc: %lu MHz, %s\n",
            (unsigned long)(tsc_frequency() / 1000000),
            tsc_is_invariant() ? "invariant" : "not invariant");
    delay_report();

    kprintf("boot complete: graphical console live, serial mirrored\n");

    /* Bring the other processors up before the console starts answering, so a
     * machine that comes up single-CPU says so while the log is still being
     * read rather than being discovered much later. */
    smp_init();

    /* Bring up input before either path continues, so the self test can check
     * that a keyboard was actually found and configured rather than assuming
     * it. */
    if (keyboard_init() != 0)
        kwarn("no keyboard: the graphical console will not accept typing");

    /* The user/kernel boundary, before anything can enter it: the gate has to be
     * installed with its DPL before a program's first `int $0x80`, and a gate that is
     * not there yet turns into a #GP that looks like a broken program. */
    if (syscall_init() != 0)
        kwarn("syscalls: the syscall gate could not be installed; no program "
              "will be able to enter the kernel");

    /* Interrupts on, before anything that can wait for one.
     *
     * The application processors already have theirs on -- each enables its own
     * as it comes up -- and until this point the boot processor does not, so a
     * tick that would preempt it never arrives and its half of the scheduler
     * never ticks.  It is here rather than in interactive_console() because the
     * self tests need it too: they sleep, and a sleep with interrupts off never
     * ends. */
    sti();

    if (boot_cmdline.test) {
        test_run_all();
        test_linger_and_exit();
    }

    interactive_console();
}

/* The idle loop: pump the input queue and halt between events until something
 * wakes us. */
static void interactive_console(void) {
    /* Report which lines are actually unmasked.  The 8259s are fully masked by
     * design once the IOAPIC owns delivery, so counting clear bits in *their*
     * mask always prints zero and says nothing about the machine. */
    unsigned open = 0;
    for (unsigned line = 0; line < IRQ_MAX; line++)
        if (!irq_line_masked(line))
            open++;

    kprintf("interrupts enabled; %u interrupt line(s) deliver (via the %s), "
            "LAPIC timer %s\n", open,
            boot_info.interrupts_via_ioapic ? "IOAPIC" : "8259",
            boot_info.have_timer ? "armed" : "absent");

    conio_banner();

    /* Halve between events rather than halt.
     *
     * The scheduler's idle path is where a halt belongs, but this is the boot
     * task: it is never descheduled while it is in this loop unless something is
     * queued, and a halt here would park the processor in the first interrupt gap
     * if the console were ever the only thing left.  The cost of the pause is a
     * few hundred cycles; the cost of a halt that never wakes is a machine. */
    uint64_t last = timer_uptime_ms();
    for (;;) {
        if (conio_pump() == 0) {
            for (unsigned spin = 0; spin < 64; spin++)
                pause_cpu();
        }
        heartbeat(timer_uptime_ms(), &last);
    }
}

/* --- entry -------------------------------------------------------------- */

void kmain(void) {
    /* Descriptor tables first: from here on a fault is reportable with a full
     * register dump instead of silently triple-faulting the machine. */
    gdt_init();
    idt_init();

    serial_init();

    /* The direct map offset has to be known before anything calls
     * phys_to_virt(): the framebuffer address, the ACPI tables and every frame
     * the allocator hands out all go through it. */
    hhdm_init();

    kmalloc_init();
    parse_cmdline();

    /* The framebuffer console has to come up before anything is printed so
     * that the graphical console and the serial mirror never disagree. */
    framebuffer_start();
    console_init();

    /* Hand the boot CPU a stack that lives in the kernel's own heap. */
    void *stack = kmalloc(BOOT_STACK_SIZE);
    if (stack == NULL)
        panic("out of memory allocating the boot stack");

    kstack_enter((uint8_t *)stack + BOOT_STACK_SIZE, kmain_stage2);
}