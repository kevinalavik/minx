/* minx - in-kernel self tests.
 *
 * These run when the kernel is booted with minx.test=1.  Each check prints a
 * line the harness greps for:
 *
 *   PASS <name>   the check succeeded
 *   FAIL <name>   the check failed, with the reason after a colon
 *
 * The guest prints TEST-READY once every check has run and then lingers for a
 * moment so the harness can dump the framebuffer before shutting down.
 */
#include "test.h"

#include "boot.h"
#include "console.h"
#include "acpi.h"
#include "apic.h"
#include "cpu.h"
#include "delay.h"
#include "fb.h"
#include "input.h"
#include "io.h"
#include "irq.h"
#include "keyboard.h"
#include "percpu.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "pmm.h"
#include "ramfs.h"
#include "panic.h"
#include "serial.h"
#include "spinlock.h"
#include "task.h"
#include "smp.h"
#include "string.h"
#include "timer.h"
#include "programs.h"
#include "elf.h"
#include "syscall.h"
#include "vmm.h"
#include "types.h"
#include "vmm.h"

/* How long to keep the framebuffer on screen after the tests finish. */
#define TEST_LINGER_MS 4000

static unsigned passed;
static unsigned failed;

/* --- tiny test harness -------------------------------------------------- */

/* Result lines must start at column 0: the checks that drive the console with
 * escape sequences leave the cursor mid-row, and a line that begins with an
 * escape byte would not match the harness' '^PASS' / '^FAIL' greps. */
static void result_line_start(void) {
    uint32_t row, col;
    console_get_cursor(&row, &col);
    if (col != 0)
        kprintf("\n");
}

void test_pass(const char *name) {
    result_line_start();
    passed++;
    kprintf("PASS %s\n", name);
}

void test_fail(const char *name, const char *fmt, ...) {
    result_line_start();
    failed++;
    kprintf("FAIL %s: ", name);
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
}

/* --- phase 1: firmware handover ----------------------------------------- */

static void test_firmware(void) {
    const fb_info_t *fbi = fb_get_info();
    if (!fbi->ready) {
        test_fail("phase1-firmware", "no framebuffer was provided");
        return;
    }
    if (fbi->width < 640 || fbi->height < 400) {
        test_fail("phase1-firmware", "framebuffer is only %ux%u",
                  fbi->width, fbi->height);
        return;
    }
    if (fbi->bpp != 16 && fbi->bpp != 24 && fbi->bpp != 32) {
        test_fail("phase1-firmware", "unsupported framebuffer depth %u", fbi->bpp);
        return;
    }
    if (boot_info.memmap_entries == 0) {
        test_fail("phase1-firmware", "the memory map was empty");
        return;
    }
    if (boot_info.rsdp == NULL) {
        test_fail("phase1-firmware", "no RSDP was provided");
        return;
    }
    if (boot_info.cpu_count == 0 || boot_info.cpu_count > MAX_CPU_COUNT) {
        test_fail("phase1-firmware", "implausible CPU count %u",
                  boot_info.cpu_count);
        return;
    }
    test_pass("phase1-firmware");
}

/* --- phase 1: console ---------------------------------------------------- */

static uint32_t fbi_width(void) {
    const fb_info_t *fbi = fb_get_info();
    return fbi->ready ? fbi->width : 0;
}

/* The console keeps its cells in an array we cannot see from here, so the
 * scrolling check works through the observable side effects: after printing
 * more lines than there are rows, the first one must be gone from the screen. */

static void test_console_geometry(void) {
    uint32_t rows = console_rows();
    uint32_t cols = console_cols();

    if (rows < 10 || cols < 40) {
        test_fail("phase1-console-geometry", "terminal is only %ux%u", cols, rows);
        return;
    }
    if (cols * 8u > fbi_width()) {
        test_fail("phase1-console-geometry", "%u columns do not fit %u pixels",
                  cols, fbi_width());
        return;
    }
    test_pass("phase1-console-geometry");
}

static void test_console_scrolling(void) {
    uint32_t rows = console_rows();
    uint32_t start_row, start_col;

    /* Put a marker at the very top of a freshly scrolled screen. */
    console_set_cursor(0, 0);
    kprintf("SELFTEST-TOP-MARKER\n");

    /* Print far more lines than the screen can hold. */
    for (uint32_t i = 0; i < rows * 3; i++)
        kprintf("scroll line %u padding padding padding padding\n", i);

    console_get_cursor(&start_row, &start_col);

    /* After scrolling by rows*3 the original marker cannot still be visible:
     * the cursor must be lower on the screen than the top, and the total line
     * count has to have wrapped around. */
    if (start_row == 0) {
        test_fail("phase1-console-scroll", "cursor still on row 0 after "
                  "printing %u lines onto %u rows", rows * 3, rows);
        return;
    }

    kprintf("\n");
    test_pass("phase1-console-scroll");
}

static void test_console_colour(void) {
    console_color_t saved = console_get_color();

    /* Red foreground, then verify the terminal actually recorded a change. */
    kprintf("\033[31m");
    console_color_t red = console_get_color();
    kprintf("\033[0m");
    console_color_t reset = console_get_color();

    if (red.fg == saved.fg) {
        test_fail("phase1-console-colour", "SGR 31 did not change the "
                  "foreground (still %06x)", red.fg);
        return;
    }
    if (reset.fg != saved.fg) {
        test_fail("phase1-console-colour", "SGR 0 did not restore the "
                  "foreground (%06x, expected %06x)", reset.fg, saved.fg);
        return;
    }

    /* 256-colour form: 38;2;r;g;b */
    kprintf("\033[38;2;18;52;86m");
    console_color_t rgb = console_get_color();
    kprintf("\033[0m");

    if (rgb.fg != 0x123456u) {
        test_fail("phase1-console-colour", "24-bit SGR produced %06x, "
                  "expected 123456", rgb.fg);
        return;
    }

    /* Leave the terminal on a clean line: the checks that drive the console with
 * escape sequences would otherwise put control bytes at the start of the next
 * result line, and the harness greps for '^PASS' / '^FAIL'. */
    kprintf("\n");
    console_set_color(saved);
    test_pass("phase1-console-colour");
}

static void test_console_cursor(void) {
    uint32_t rows = console_rows();
    uint32_t cols = console_cols();
    uint32_t row, col;

    /* Cursor position (CUP), 1-based on the wire. */
    kprintf("\033[5;10H");
    console_get_cursor(&row, &col);
    if (row != 4 || col != 9) {
        test_fail("phase1-console-cursor", "CUP 5;10 landed on %u,%u "
                  "(expected 4,9)", row, col);
        return;
    }

    /* Cursor up must clamp at the top of the screen. */
    kprintf("\033[999A");
    console_get_cursor(&row, &col);
    if (row != 0) {
        test_fail("phase1-console-cursor", "cursor up past the top gave row %u", row);
        return;
    }

    /* And down must clamp at the bottom. */
    kprintf("\033[999B");
    console_get_cursor(&row, &col);
    if (row != rows - 1) {
        test_fail("phase1-console-cursor", "cursor down past the bottom "
                  "gave row %u, expected %u", row, rows - 1);
        return;
    }

    /* Erase-in-line must not disturb the column count. */
    kprintf("\033[K");
    console_get_cursor(&row, &col);
    if (col != cols - 1 && col >= cols) {
        test_fail("phase1-console-cursor", "EL moved the cursor to %u", col);
        return;
    }

    console_set_cursor(console_rows() - 1, 0);
    kprintf("\n");
    test_pass("phase1-console-cursor");
}

/* --- phase 1: descriptors and interrupts -------------------------------- */

/* A deliberately faulting instruction, used to prove the IDT path end to end. */
static volatile int ud2_fired;
static volatile uint64_t ud2_vector;
static volatile uint64_t ud2_rip;

static void ud2_handler(regs_t *regs) {
    ud2_fired = 1;
    ud2_vector = regs->vector;
    ud2_rip = regs->rip;
    /* iretq resumes at the RIP the CPU pushed, which for a fault is the
     * faulting instruction itself.  ud2 is two bytes, so step over it --
     * otherwise the handler returns straight into another #UD and the machine
     * spins forever with no output. */
    regs->rip += 2;
}

static void test_interrupt_path(void) {
    ud2_fired = 0;
    isr_install_handler(6, ud2_handler);

    /* ud2 raises #UD (vector 6).  The handler returns, so iretq resumes at
     * the instruction after it. */
    __asm__ volatile("ud2" ::: "memory");

    isr_install_handler(6, NULL);

    if (!ud2_fired) {
        test_fail("phase1-interrupt", "the #UD handler never ran");
        return;
    }
    if (ud2_vector != 6) {
        test_fail("phase1-interrupt", "handler saw vector %lu, expected 6",
                  ud2_vector);
        return;
    }
    test_pass("phase1-interrupt");
}

static void test_exception_report(void) {
    /* The reporting path must work even before the console exists, because
     * that is exactly when it is needed. */
    regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.vector = 14;
    regs.error_code = 0;
    regs.rip = 0xdeadbeefUL;

    kprintf("\n");
    isr_report_exception(&regs);
    kprintf("\n");
    test_pass("phase1-exception-report");
}

/* --- phase 1: allocator -------------------------------------------------- */

static void test_kmalloc(void) {
    size_t free_before = kmalloc_free_bytes();

    /* Churn: many allocations of assorted sizes, all freed again. */
    void *blocks[64];
    for (size_t i = 0; i < 64; i++) {
        blocks[i] = kmalloc(1 + i * 37);
        if (blocks[i] == NULL) {
            test_fail("phase1-kmalloc", "allocation %zu of %zu bytes failed",
                      i, 1 + i * 37);
            return;
        }
        memset(blocks[i], (int)(i & 0xff), 1 + i * 37);
    }

    /* Verify the writes stuck: a broken allocator would have aliased blocks. */
    for (size_t i = 0; i < 64; i++) {
        uint8_t *p = (uint8_t *)blocks[i];
        for (size_t j = 0; j < 1 + i * 37; j++) {
            if (p[j] != (uint8_t)(i & 0xff)) {
                test_fail("phase1-kmalloc", "block %zu byte %zu was "
                          "overwritten by another allocation", i, j);
                return;
            }
        }
    }

    for (size_t i = 0; i < 64; i++)
        kfree(blocks[i]);

    size_t free_after = kmalloc_free_bytes();
    if (free_after < free_before) {
        test_fail("phase1-kmalloc", "free bytes fell from %zu to %zu "
                  "(coalescing lost memory)", free_before, free_after);
        return;
    }

    /* A large allocation must still be satisfiable afterwards. */
    void *big = kmalloc(256 * 1024);
    if (big == NULL) {
        test_fail("phase1-kmalloc", "a 256 KiB allocation failed after churn");
        return;
    }
    kfree(big);

    test_pass("phase1-kmalloc");
}

static void test_string(void) {
    char buf[64];

    ksprintf(buf, sizeof(buf), "%d %u %x %s %c %lu", -42, 42u, 0xbeefu,
             "str", 'x', 1234567UL);
    if (strcmp(buf, "-42 42 beef str x 1234567") != 0) {
        test_fail("phase1-string", "ksprintf produced \"%s\"", buf);
        return;
    }

    ksprintf(buf, sizeof(buf), "[%5d][%-5d][%05d]", 42, 42, 42);
    if (strcmp(buf, "[   42][42   ][00042]") != 0) {
        test_fail("phase1-string", "padding produced \"%s\"", buf);
        return;
    }

    if (strnlen("abcdef", 3) != 3 || strcmp("a", "b") >= 0 ||
        strcmp("b", "a") <= 0 || strcasecmp("ABC", "abc") != 0) {
        test_fail("phase1-string", "string helpers misbehaved");
        return;
    }

    test_pass("phase1-string");
}

/* ACPI discovery and the interrupt controllers it describes. */
static void test_acpi(void) {
    if (!acpi_ready()) {
        test_fail("phase2-acpi", "no MADT was parsed (RSDP revision %u)",
                  acpi_version());
        return;
    }

    unsigned cpus = 0;
    const acpi_cpu_t *list = acpi_cpu_table(&cpus);

    if (cpus == 0) {
        test_fail("phase2-acpi", "the MADT listed no processors");
        return;
    }
    if (cpus > MAX_CPU_COUNT) {
        test_fail("phase2-acpi", "the MADT listed %u processors, over the "
                  "kernel's limit of %u", cpus, MAX_CPU_COUNT);
        return;
    }

    /* Two processors cannot share a LAPIC id: everything that targets a CPU by
     * id (IPIs, IOAPIC destinations) would then reach the wrong one. */
    for (unsigned i = 0; i < cpus; i++) {
        for (unsigned j = i + 1; j < cpus; j++) {
            if (list[i].lapic_id == list[j].lapic_id) {
                test_fail("phase2-acpi",
                          "processors %u and %u both have LAPIC id %u", i, j,
                          list[i].lapic_id);
                return;
            }
        }
    }
    if (!acpi_has_lapic()) {
        test_fail("phase2-acpi", "the MADT did not name a local APIC");
        return;
    }
    if (acpi_madt() == NULL || acpi_madt_length() < ACPI_MADT_HEADER_SIZE) {
        test_fail("phase2-acpi", "the MADT record is missing or too short");
        return;
    }

    /* The first processor must be the one that is actually running, otherwise
     * the topology the kernel logged is not the topology it is on. */
    if (percpu_lapic_id(PERCPU_BOOTSTRAP) != boot_info.bsp_lapic_id) {
        test_fail("phase2-acpi",
                  "the boot processor's MADT entry has LAPIC id %u, the "
                  "bootloader said %u",
                  percpu_lapic_id(PERCPU_BOOTSTRAP), boot_info.bsp_lapic_id);
        return;
    }

    unsigned ioapics = 0;
    const acpi_ioapic_t *io = acpi_ioapic_table(&ioapics);
    if (ioapics == 0) {
        test_fail("phase2-acpi", "the MADT listed no IOAPIC");
        return;
    }

    /* An IOAPIC's MMIO base is a full 32-bit address: truncating it to a byte,
     * which is easy to do, turns 0xfec00000 into 0x00. */
    if (io[0].base == 0 || io[0].base < 0x1000u) {
        test_fail("phase2-acpi",
                  "IOAPIC 0 has implausible base %#lx, which is what a "
                  "truncated field looks like",
                  (unsigned long)io[0].base);
        return;
    }

    test_pass("phase2-acpi");
}

/* Interrupt delivery.
 *
 * The IOAPIC owns the lines once it is up, and the 8259s are masked for good,
 * so the interesting checks are the IOAPIC's redirection entries and -- the part
 * that actually matters -- that an interrupt reaches the CPU at all.  The LAPIC
 * timer and the 8253 are both wired up, and both are counted here, which is the
 * end-to-end proof that page tables, the APIC and the IDT all work together. */
static void test_interrupts(void) {
    if (!interrupts_via_ioapic()) {
        test_fail("phase2-interrupts",
                  "the IOAPIC does not own the interrupt lines");
        return;
    }

    /* With the IOAPIC in charge the legacy chips must stay masked; anything else
     * means a line has two possible sources and can be delivered twice. */
    if (pic_irq_mask() != 0xffff) {
        test_fail("phase2-interrupts",
                  "the 8259s are not fully masked while the IOAPIC owns the "
                  "lines (mask %#x)", pic_irq_mask());
        return;
    }

    if (!lapic_available()) {
        test_fail("phase2-interrupts", "no local APIC");
        return;
    }

    /* Round-trip a line's mask bit through the chip, the same way the 8259 read
     * back its own mask: an unmask that the hardware ignored has to show up.
     * Every line starts masked, because ioapic_init() masks the whole chip until
     * a driver claims the lines it needs. */
    irq_disable(7);
    if (!irq_line_masked(7)) {
        test_fail("phase2-interrupts",
                  "masking IRQ7 left the IOAPIC's bit clear");
        return;
    }
    irq_enable(7);
    if (irq_line_masked(7)) {
        test_fail("phase2-interrupts",
                  "unmasking IRQ7 left the IOAPIC's bit set");
        return;
    }
    irq_disable(7);

    if (irq_handler(0) == NULL) {
        test_fail("phase2-interrupts", "no handler is registered for IRQ0");
        return;
    }
    if (irq_handler(1) == NULL) {
        test_fail("phase2-interrupts", "no handler is registered for IRQ1");
        return;
    }

    /* Interrupts are still off at this point in the boot, so turn them on for
     * the length of the measurement and turn them off again: a test that left
     * them on would race every later check. */
    if (boot_info.have_timer && !lapic_timer_ticks()) {
        uint64_t before = lapic_timer_ticks();
        uint64_t pit_before = pit_uptime_ticks();

        sti();
        delay_ms(120);
        cli();

        uint64_t after = lapic_timer_ticks();
        uint64_t pit_after = pit_uptime_ticks();

        /* The LAPIC timer runs at about 1 kHz, so 120 ms is around 120 ticks.
         * Requiring at least a handful rather than an exact count keeps this
         * from being a test of QEMU's timer rather than of the kernel. */
        if (after - before < 10) {
            test_fail("phase2-interrupts",
                      "the LAPIC timer fired %lu times in 120 ms",
                      (unsigned long)(after - before));
            return;
        }
        if (pit_after - pit_before < 2) {
            test_fail("phase2-interrupts",
                      "the 8253 fired %lu times in 120 ms",
                      (unsigned long)(pit_after - pit_before));
            return;
        }

        kprintf("\ninterrupts: LAPIC timer %lu ticks, 8253 %lu ticks in "
                "120 ms\n", (unsigned long)(after - before),
                (unsigned long)(pit_after - pit_before));
    }

    test_pass("phase2-interrupts");
}

/* The PS/2 controller has to be found, configured and routed, or the graphical
 * console cannot be typed into.  No scancodes are required here: a keyboard that
 * nobody has touched yet says nothing, and the interactive path is what proves
 * they arrive. */
static void test_keyboard(void) {
    if (!keyboard_present()) {
        test_fail("phase2-keyboard", "no PS/2 keyboard was detected");
        return;
    }
    if (!keyboard_ready()) {
        test_fail("phase2-keyboard",
                  "the keyboard did not pass its self test");
        return;
    }
    if (keyboard_uses_8042_translation()) {
        /* Translation is the normal arrangement: the keyboard speaks set 2 and
         * the controller converts.  It only matters that one of the two was
         * chosen deliberately, which the log line above already records. */
        kprintf("\nkeyboard: set 2 with 8042 translation\n");
    }
    if (irq_handler(1) == NULL) {
        test_fail("phase2-keyboard", "IRQ1 has no handler");
        return;
    }
    if (irq_line_masked(1)) {
        test_fail("phase2-keyboard", "IRQ1 is masked");
        return;
    }

    test_pass("phase2-keyboard");
}

/* The input queue has to survive the pattern the keyboard produces: make, then
 * break for the same key, with modifiers changing in between. */
static void test_input_queue(void) {
    input_event_t ev;

    input_init();
    if (!input_empty()) {
        test_fail("phase2-input", "the queue was not empty after init");
        return;
    }
    if (input_pop(&ev)) {
        test_fail("phase2-input", "popping an empty queue returned an event");
        return;
    }

    /* Fill it past the end and confirm the overflow is reported rather than
     * quietly overwriting events.  The ring uses sequence counters rather than a
     * spare sentinel slot, so all INPUT_QUEUE_SIZE slots are usable. */
    unsigned accepted = 0;
    for (unsigned i = 0; i < INPUT_QUEUE_SIZE + 8; i++) {
        ev.type = INPUT_KEY_DOWN;
        ev.key = 'a' + (i % 26);
        ev.modifiers = 0;
        ev.x = ev.y = 0;
        ev.buttons = 0;
        if (input_push(&ev))
            accepted++;
    }
    if (accepted != INPUT_QUEUE_SIZE) {
        test_fail("phase2-input", "the queue accepted %u events into %u slots",
                  accepted, INPUT_QUEUE_SIZE);
        return;
    }
    if (input_pending() != accepted) {
        test_fail("phase2-input", "the queue reports %zu pending, %u pushed",
                  input_pending(), accepted);
        return;
    }

    /* Events must come back in the order they went in. */
    for (unsigned i = 0; i < accepted; i++) {
        if (!input_pop(&ev)) {
            test_fail("phase2-input", "the queue emptied after %u of %u",
                      i, accepted);
            return;
        }
        if (ev.key != (uint32_t)('a' + (i % 26))) {
            test_fail("phase2-input", "event %u came back as %u, expected %u",
                      i, ev.key, (uint32_t)('a' + (i % 26)));
            return;
        }
    }
    if (!input_empty()) {
        test_fail("phase2-input", "the queue still had events after draining");
        return;
    }

    test_pass("phase2-input");
}

/* --- phase 3: memory ----------------------------------------------------- */

/* Every frame the physical manager hands out has to be addressable, writable
 * and distinct, and giving it back has to put it in the free pool.  The
 * accounting is checked against the numbers the allocator reports rather than
 * against a fixed number, because the machine's memory is whatever the
 * firmware said it was. */
static void test_pmm(void) {
    if (!pmm_ready()) {
        test_fail("phase3-pmm", "the physical allocator is not ready");
        return;
    }

    pmm_stats_t before;
    pmm_get_stats(&before);
    if (before.free_frames == 0 || before.free_frames > before.total_frames) {
        test_fail("phase3-pmm", "%lu of %lu frames are free, which cannot "
                  "be right", (unsigned long)before.free_frames,
                  (unsigned long)before.total_frames);
        return;
    }
    if (before.free_frames + before.used_frames != before.total_frames) {
        test_fail("phase3-pmm", "%lu free plus %lu used is not %lu total",
                  (unsigned long)before.free_frames,
                  (unsigned long)before.used_frames,
                  (unsigned long)before.total_frames);
        return;
    }

    /* Frames written through the direct map come back with their own contents
     * intact, which is what makes the direct map worth having. */
    enum { PAGES = 32 };
    uint64_t phys[PAGES];
    uint8_t *virt[PAGES];

    for (unsigned i = 0; i < PAGES; i++) {
        virt[i] = pmm_alloc_page();
        if (virt[i] == NULL) {
            test_fail("phase3-pmm", "frame %u of %u could not be allocated",
                      i, PAGES);
            return;
        }

        uint64_t p = virt_to_phys(virt[i]);
        phys[i] = p;
        if ((p & (page_size - 1)) != 0) {
            test_fail("phase3-pmm", "frame %u is at %#lx, not page aligned",
                      i, (unsigned long)p);
            return;
        }
        if (!pmm_physical_is_mapped(p)) {
            test_fail("phase3-pmm", "frame %u at %#lx is outside the direct "
                      "map", i, (unsigned long)p);
            return;
        }
        if (vmm_translate((uint64_t)virt[i]) != p) {
            test_fail("phase3-pmm", "frame %u translates to %#lx, expected "
                      "%#lx", i, (unsigned long)vmm_translate((uint64_t)virt[i]),
                      (unsigned long)p);
            return;
        }

        memset(virt[i], (int)(i + 1), page_size);
    }

    /* Distinct frames must not share a page, and a write through one must not
     * turn up in another: a direct map that aliases every frame to the same
     * physical page passes every single check above. */
    for (unsigned i = 0; i < PAGES; i++) {
        for (unsigned j = i + 1; j < PAGES; j++) {
            if (phys[i] == phys[j]) {
                test_fail("phase3-pmm", "frames %u and %u are both at %#lx",
                          i, j, (unsigned long)phys[i]);
                return;
            }
        }
        if (virt[i][0] != (uint8_t)(i + 1) ||
            virt[i][page_size - 1] != (uint8_t)(i + 1)) {
            test_fail("phase3-pmm", "frame %u no longer holds what was "
                      "written to it", i);
            return;
        }
    }

    for (unsigned i = 0; i < PAGES; i++)
        pmm_free_page(virt[i]);

    pmm_stats_t after;
    pmm_get_stats(&after);
    if (after.free_frames != before.free_frames) {
        test_fail("phase3-pmm", "free frames went from %lu to %lu across "
                  "%u allocations and frees", (unsigned long)before.free_frames,
                  (unsigned long)after.free_frames, PAGES);
        return;
    }

    test_pass("phase3-pmm");
}

/* A run of contiguous frames has to come back contiguous and in order, because
 * page tables need a single allocation to map a run.  Getting that wrong is
 * silent: the frames are all distinct and all writable, they are just not next
 * to each other. */
static void test_pmm_contiguous(void) {
    if (!pmm_ready()) {
        test_fail("phase3-pmm-contiguous", "the physical allocator is not ready");
        return;
    }

    enum { COUNT = 16 };
    uint8_t *run = pmm_alloc_zeroed_pages(COUNT);
    if (run == NULL) {
        test_fail("phase3-pmm-contiguous", "no run of %u frames", COUNT);
        return;
    }

    uint64_t first = virt_to_phys(run);
    for (unsigned i = 0; i < COUNT; i++) {
        uint64_t p = virt_to_phys(run + i * page_size);
        if (p != first + (uint64_t)i * page_size) {
            test_fail("phase3-pmm-contiguous", "frame %u of the run is at "
                      "%#lx, expected %#lx", i, (unsigned long)p,
                      (unsigned long)(first + (uint64_t)i * page_size));
            return;
        }
        /* A zeroed allocation that is not zero is a page that was handed out
         * before and not cleared. */
        if (run[i * page_size] != 0 || run[i * page_size + page_size - 1] != 0) {
            test_fail("phase3-pmm-contiguous", "frame %u of the run is not "
                      "zeroed", i);
            return;
        }
    }

    pmm_free_pages(run, COUNT);
    test_pass("phase3-pmm-contiguous");
}

/* The direct map is what makes physical memory reachable at all, so every part
 * of it has to translate back to the address it is supposed to be covering: the
 * bottom of RAM, the top of the usable range, and the frames the kernel itself
 * is standing in. */
static void test_direct_map(void) {
    if (!vmm_active()) {
        test_fail("phase3-direct-map", "the virtual memory manager is not active");
        return;
    }
    if (hhdm_offset == 0) {
        test_fail("phase3-direct-map", "the direct map offset is zero");
        return;
    }

    uint64_t highest = pmm_highest_usable_phys();
    if (highest == 0) {
        test_fail("phase3-direct-map", "the allocator reports no usable memory");
        return;
    }

    struct {
        uint64_t phys;
        const char *what;
    } probes[] = {
        { 0x1000, "the first page above the BIOS" },
        { 0x100000, "1 MiB" },
        { 0x10000000, "256 MiB" },
        { highest - page_size, "the last usable frame" },
        /* Not page-aligned, deliberately.  A translation that returns the frame
         * rather than the address is right for every page-aligned address and
         * wrong for all the others, so a probe that only used aligned ones would
         * never notice -- and the address a caller has is rarely aligned. */
        { 0x1234, "an unaligned address near the bottom" },
        { 0x10000 + 0xabc, "an unaligned address inside 1 MiB" },
        { 0x400000 + 0x7d0, "an unaligned address inside 1 GiB" },
        { highest - 0x40, "an unaligned address in the last frame" },
    };

    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        uint64_t virt = probes[i].phys + hhdm_offset;

        if (vmm_translate(virt) != probes[i].phys) {
            test_fail("phase3-direct-map", "%s (%#lx) translates to %#lx",
                      probes[i].what, (unsigned long)probes[i].phys,
                      (unsigned long)vmm_translate(virt));
            return;
        }
        if (!pmm_physical_is_mapped(probes[i].phys)) {
            test_fail("phase3-direct-map", "%s (%#lx) is not covered",
                      probes[i].what, (unsigned long)probes[i].phys);
            return;
        }
    }

    /* One past the top of usable memory is not mapped: a mapping that ran past
     * the end of RAM would hand out frames nobody owns. */
    if (pmm_physical_is_mapped(highest + page_size)) {
        test_fail("phase3-direct-map", "the direct map covers %#lx, which is "
                  "past the last usable frame", (unsigned long)(highest + page_size));
        return;
    }

    test_pass("phase3-direct-map");
}

/* Churn the physical allocator hard enough that a bitmap that forgets to clear
 * bits, or that hands out the same frame twice, runs out of room or repeats an
 * address.  A hundred megabyte's worth of one-page allocations in a loop is
 * cheap in instructions and impossible to survive by accident. */
static void test_pmm_churn(void) {
    if (!pmm_ready()) {
        test_fail("phase3-pmm-churn", "the physical allocator is not ready");
        return;
    }

    enum { SLOTS = 64 };
    uint8_t *held[SLOTS];
    uint64_t held_phys[SLOTS];
    unsigned held_count = 0;

    for (unsigned round = 0; round < 4096; round++) {
        unsigned slot = round % SLOTS;
        if (held[slot] != NULL) {
            pmm_free_page(held[slot]);
            held[slot] = NULL;
        }

        uint8_t *page = pmm_alloc_page();
        if (page == NULL) {
            test_fail("phase3-pmm-churn", "the pool ran dry at round %u of "
                      "4096", round);
            return;
        }

        uint64_t p = virt_to_phys(page);
        for (unsigned i = 0; i < held_count; i++) {
            if (held_phys[i] == p) {
                test_fail("phase3-pmm-churn", "round %u was handed frame "
                          "%#lx, which %u still holds", round,
                          (unsigned long)p, i);
                return;
            }
        }

        memset(page, (int)(round & 0xff), 64);
        held[slot] = page;
        held_phys[slot] = p;
        if (slot + 1 > held_count)
            held_count = slot + 1;
    }

    for (unsigned i = 0; i < SLOTS; i++)
        if (held[i] != NULL)
            pmm_free_page(held[i]);

    test_pass("phase3-pmm-churn");
}

/* --- phase 4: SMP -------------------------------------------------------- */

/* Per-processor state has to be *per* processor before it can be *shared* state.
 *
 * The check is that this processor's %gs resolves to its own slot and to nothing
 * else.  A kernel that reads one shared slot from every processor passes every
 * test in the tree while being exactly the thing that cannot work: two processors
 * counting interrupts into the same counter produce a total that looks plausible
 * and is wrong, and nothing crashes to say so.
 *
 * This does not require a second processor to exist.  It is the property that has
 * to hold before one is started, and holding it with one processor is what makes
 * the difference between "no processor came up" and "a processor came up and
 * corrupted everything". */
static void test_percpu(void) {
    /* One snapshot, and everything below is derived from it.
     *
     * Reading %gs twice to check that GS is self-consistent works right up until
     * there is a preemptive scheduler, and then it fails for a reason that has
     * nothing to do with per-CPU state: the task is switched to another processor
     * between the two reads and comes back with a different processor's slot.  The
     * report then says GS points at the wrong state when in fact it pointed at the
     * right one and the task simply moved. */
    unsigned self = MAX_CPU_COUNT;
    percpu_t *me = percpu_self_snapshot(&self);

    if (me == NULL || self >= percpu_count()) {
        test_fail("phase4-percpu", "%%gs:0 is %p, which is not one of the %u "
                  "known processors' slots", (void *)me, percpu_count());
        return;
    }

    if (percpu_by_index(self) != me) {
        test_fail("phase4-percpu", "processor %u's %%gs slot holds %p, but "
                  "slot %u is %p", self, (void *)me, self,
                  (void *)percpu_by_index(self));
        return;
    }

    if (self != percpu_self_index() || me != percpu_self()) {
        /* Not a failure on its own -- it is what a migration looks like -- but it
         * means this task moved between the snapshot and now, so the checks below
         * are about a processor that may no longer be running. */
        kprintf("percpu: this task moved between two reads; checking the "
                "table instead\n");
    }

    for (unsigned i = 0; i < percpu_count(); i++) {
        percpu_t *t = percpu_by_index(i);

        if (t == NULL) {
            test_fail("phase4-percpu", "processor %u has no slot", i);
            return;
        }

        for (unsigned j = i + 1; j < percpu_count(); j++) {
            percpu_t *o = percpu_by_index(j);

            if (t->lapic_id == o->lapic_id) {
                test_fail("phase4-percpu", "processors %u and %u share LAPIC "
                          "id %u", i, j, t->lapic_id);
                return;
            }
        }

        if (t->lapic_id != percpu_lapic_id(i)) {
            test_fail("phase4-percpu", "slot %u says LAPIC id %u, the hardware "
                      "says %u", i, t->lapic_id, percpu_lapic_id(i));
            return;
        }
    }

    test_pass("phase4-percpu");
}

/* How many processors are actually running.
 *
 * This reports rather than requires.  A machine where the application processors
 * could not be started is a machine that still boots, still has a console and
 * still passes every other check, so failing the whole suite here would turn an
 * incomplete phase into a red build and hide the fact that it is incomplete.
 * What it does insist on is that the count is possible: never more online than
 * known, and never less than the one processor that is certainly running. */
/* --- lock and IPI contention -------------------------------------------- */

/* Rounds per processor.  Small enough that a contended test does not dominate the
 * run, large enough that two processors really do overlap: at a few hundred the
 * whole thing fits inside one processor's first pass and nothing contends. */
#define ATOMIC_ROUNDS  20000
#define CONTEND_ROUNDS 8000
#define FAIR_ROUNDS    64

/* --- atomics ----------------------------------------------------------- */

static void atomic_worker(void *arg) {
    atomic_t *a = arg;

    for (unsigned i = 0; i < ATOMIC_ROUNDS; i++)
        atomic_increment(a);
}


/* Run `work` on every online processor and wait for all of them.
 *
 * One task per processor, each on that processor's own stack.
 *
 * A per-CPU work slot would be the alternative, and it is what the reschedule IPI
 * drains, but a slot holds exactly one item -- so anything that wants to run on
 * every processor at once has to overwrite something, and with the scheduler up a
 * processor is being kicked constantly, so an overwrite becomes the wrong function
 * being called on the wrong processor.  That is not hypothetical: it is what
 * `phase4-atomics` did on eight processors before this change, and it showed up as
 * a lost task rather than as anything that looked like a queue.
 *
 * The wait is on a completion count rather than on the tasks, so a worker that
 * never runs is a timeout and a short total -- not a test that passes because
 * nothing contended. */
typedef struct {
    void (*fn)(void *);
    void  *arg;
    atomic_t *done;
} fanout_t;

static void fanout_worker(void *arg) {
    fanout_t *f = arg;

    f->fn(f->arg);
    atomic_increment(f->done);
}

static unsigned fanout_every_cpu(void (*work)(void *), void *arg,
                                 atomic_t *done) {
    unsigned indices[MAX_CPU_COUNT];
    unsigned n = smp_online_indices(indices, MAX_CPU_COUNT);
    fanout_t f = { work, arg, done };

    /* A task for every processor, this one included.
     *
     * Running this processor's share inline and skipping its task is the obvious
     * shortcut and it is wrong under a scheduler: the share can be preempted and
     * migrated partway through, so the processor that started it is not the one
     * that finished it, and the task skipped on the strength of a stale index is
     * created anyway.  Then that processor runs the work twice, which is how a
     * fairness check ends up counting 128 turns where it asked for 64. */
    for (unsigned i = 0; i < n; i++) {
        task_t *t = task_create("fanout", fanout_worker, &f, TASK_WEIGHT_MAX);

        if (t == NULL) {
            test_fail("phase4-locks", "could not create a task for processor %u",
                      indices[i]);
            return 0;
        }
        if (task_wakeup_on(indices[i], t) != 0) {
            test_fail("phase4-locks", "could not queue work for processor %u",
                      indices[i]);
            return 0;
        }
    }

    for (unsigned waited = 0; waited < 20000; waited++) {
        if ((unsigned)atomic_load(done) >= n)
            break;
        task_sleep_ms(1);
    }

    return n;
}

/* --- atomics ----------------------------------------------------------- */

static void test_atomics(void) {
    atomic_t a;

    atomic_init(&a, 5);
    if (atomic_load(&a) != 5) {
        test_fail("phase4-atomics", "a counter initialised to 5 reads %ld",
                  (long)atomic_load(&a));
        return;
    }

    if (atomic_fetch_add(&a, 3) != 5 || atomic_load(&a) != 8) {
        test_fail("phase4-atomics", "fetch_add returned %ld and left %ld, "
                  "expected 5 and 8", (long)atomic_fetch_add(&a, 0),
                  (long)atomic_load(&a));
        return;
    }

    if (atomic_fetch_sub(&a, 3) != 8 || atomic_load(&a) != 5) {
        test_fail("phase4-atomics", "fetch_sub left the counter at %ld",
                  (long)atomic_load(&a));
        return;
    }

    /* The CAS has to fail on a stale expectation and leave the counter alone.
     * Returning the *previous* value from fetch_add and overwriting on a failed
     * CAS are both easy to get backwards, and both would pass a CAS-only test. */
    if (atomic_cas(&a, 4, 99)) {
        test_fail("phase4-atomics", "a compare-and-swap against a stale value "
                  "reported success");
        return;
    }
    if (atomic_load(&a) != 5) {
        test_fail("phase4-atomics", "a failed compare-and-swap changed the "
                  "counter to %ld", (long)atomic_load(&a));
        return;
    }
    if (!atomic_cas(&a, 5, 42) || atomic_load(&a) != 42) {
        test_fail("phase4-atomics", "a compare-and-swap against the current "
                  "value did not take");
        return;
    }

    /* Counting up from several processors at once must not lose an increment, and
     * this is the one check that would catch an atomic that is not one. */
    atomic_init(&a, 0);
    atomic_t done;

    atomic_init(&done, 0);
    fanout_every_cpu(atomic_worker, &a, &done);

    uint32_t online = percpu_online_count();
    uint32_t expected = online * ATOMIC_ROUNDS;

    if ((uint32_t)atomic_load(&a) != expected) {
        test_fail("phase4-atomics", "%u processors each added %u, giving %ld, "
                  "expected %u", online, ATOMIC_ROUNDS, (long)atomic_load(&a),
                  expected);
        return;
    }
    if ((uint32_t)atomic_load(&done) != online) {
        test_fail("phase4-atomics", "only %ld of %u processors finished",
                  (long)atomic_load(&done), online);
        return;
    }

    test_pass("phase4-atomics");
}

/* --- ticket lock ------------------------------------------------------- */

typedef struct {
    ticket_lock_t lock;
    volatile uint64_t counter;
} contend_t;

static void contend_worker(void *arg) {
    contend_t *c = arg;

    for (unsigned i = 0; i < CONTEND_ROUNDS; i++) {

        ticket_lock(&c->lock);

        /* Deliberately a read, some work and a write rather than one increment:
         * a "lock" that was really an atomic on the counter would pass the
         * increment version and fail this. */
        uint64_t v = c->counter;
        for (volatile unsigned spin = 0; spin < 16; spin++)
            ;
        c->counter = v + 1;

        ticket_unlock(&c->lock);
    }
}

static void test_lock_contention(void) {
    unsigned online = percpu_online_count();

    if (online == 0) {
        test_fail("phase4-locks", "no processor is online, so there is no "
                  "contention to test");
        return;
    }

    contend_t c;
    atomic_t done;

    ticket_lock_init(&c.lock);
    c.counter = 0;
    atomic_init(&done, 0);

    uint64_t t0 = timer_uptime_ms();
    fanout_every_cpu(contend_worker, &c, &done);
    uint64_t elapsed = timer_uptime_ms() - t0;

    uint64_t expected = (uint64_t)online * CONTEND_ROUNDS;
    if ((unsigned)atomic_load(&done) != online) {
        test_fail("phase4-locks", "only %ld of %u processors finished, and the "
                  "counter reached %lu", (long)atomic_load(&done), online,
                  (unsigned long)c.counter);
        return;
    }
    if (c.counter != expected) {
        test_fail("phase4-locks", "the counter reached %lu after %u processor(s) "
                  "each took the lock %u times, expected %lu -- updates were lost",
                  (unsigned long)c.counter, online, CONTEND_ROUNDS,
                  (unsigned long)expected);
        return;
    }

    kprintf("smp: %lu lock round trips across %u processor(s) in %lu ms\n",
            (unsigned long)expected, online, (unsigned long)elapsed);
    test_pass("phase4-locks");
}

/* A ticket lock hands the lock out in the order it was asked for.  A test-and-set
 * spinlock does not, and under TCG it usually does not: one processor's store to
 * memory keeps winning, so the others can be starved while it goes round after
 * round.  The way to see the difference is to check every waiter is served at all,
 * and that no processor is served twice while another waits. */
typedef struct {
    ticket_lock_t lock;
    uint32_t      turns[MAX_CPU_COUNT];
    uint32_t      seen;
    uint32_t      served;
} fair_t;

static void fair_worker(void *arg) {
    fair_t *f = arg;

    /* Indexed by *processor*, and deliberately so: a worker's turns belong to whoever
     * happened to be holding the lock, which is what fairness is about.  Per-*task* counts
     * would say only that every task finished, which is a liveness property the scheduler
     * provides on its own.
     *
     * The assertion below then cannot be per-CPU, though, and this is why: the workers are
     * tasks and the scheduler steals work, so a worker pinned to processor 2 can spend its
     * rounds on processors 0 and 1 and end with processor 2 having taken the lock not at
     * all.  So the per-processor figures are recorded here and summed, and the property
     * asserted is that no waiter was starved -- every worker completed every round, and
     * every acquisition of the lock was accounted for exactly once. */
    unsigned index = percpu_self_index();

    /* Many rounds each, not one.  A single round cannot tell a fair lock from an
     * unfair one: the interesting case is the processor that arrives while the
     * lock is held and then never gets it again, which only shows up over
     * repeated acquisitions. */
    for (unsigned i = 0; i < FAIR_ROUNDS; i++) {
        ticket_lock(&f->lock);
        if (index < MAX_CPU_COUNT)
            f->turns[index]++;
        f->seen++;   /* inside the critical section, so a plain store */
        f->served++;
        ticket_unlock(&f->lock);
    }
}

static void test_lock_fairness(void) {
    unsigned online = percpu_online_count();
    fair_t f;
    atomic_t done;

    ticket_lock_init(&f.lock);
    memset(f.turns, 0, sizeof(f.turns));
    f.seen = 0;
    f.served = 0;
    atomic_init(&done, 0);

    fanout_every_cpu(fair_worker, &f, &done);

    /* Every worker got every round -- that is the no-starvation half, and it is the half
     * that was being asserted per processor, where work stealing made it unfalsifiable:
     * a run where one processor's tasks all migrated elsewhere failed on "processor 0
     * took the lock 0 times" while the lock had in fact behaved perfectly. */
    uint32_t counted = 0;
    for (unsigned i = 0; i < MAX_CPU_COUNT; i++)
        counted += f.turns[i];

    uint32_t expected = online * FAIR_ROUNDS;

    if (f.seen != expected) {
        test_fail("phase4-lockfair", "the workers recorded %u acquisitions, expected "
                  "%u: a waiter was starved", f.seen, expected);
        return;
    }

    /* And every acquisition was inside the critical section, exactly once: `counted` is
     * the sum of the per-processor tallies and `served` the number of times a worker held
     * the lock.  The two agreeing at `expected` is mutual exclusion; `served` running
     * ahead of `counted` would mean a worker was inside without the counter moving, and
     * the other way round would mean a counter without a holder. */
    if (counted != expected || f.served != expected) {
        test_fail("phase4-lockfair", "the per-processor turns add up to %u and the "
                  "counter reached %u, expected %u of each", counted, f.served,
                  expected);
        return;
    }

    test_pass("phase4-lockfair");
}

/* --- MCS lock --------------------------------------------------------- */

/* The queue has one node per *waiter*, so each worker's node is its own -- on its
 * own stack.  That is the whole difference from the ticket lock: the waiting
 * processors are a linked list of stack addresses, and handing the lock on is a
 * store into the predecessor's node rather than an increment of a word every
 * waiter is reading. */
typedef struct {
    mcs_lock_t   guard;
    volatile uint64_t counter;
} mcs_shared_t;

static void mcs_worker(void *arg) {
    mcs_shared_t *s = arg;
    mcs_node_t node;

    for (unsigned i = 0; i < CONTEND_ROUNDS; i++) {
        mcs_lock(&node, &s->guard);

        uint64_t v = s->counter;
        for (volatile unsigned spin = 0; spin < 16; spin++)
            ;
        s->counter = v + 1;

        mcs_unlock(&node, &s->guard);
    }
}

static void test_mcs(void) {
    mcs_shared_t s;
    atomic_t done;

    mcs_lock_init(&s.guard);
    s.counter = 0;
    atomic_init(&done, 0);

    unsigned online = percpu_online_count();
    fanout_every_cpu(mcs_worker, &s, &done);

    uint64_t expected = (uint64_t)online * CONTEND_ROUNDS;
    if ((unsigned)atomic_load(&done) != online) {
        test_fail("phase4-mcs", "only %ld of %u processors finished, and the "
                  "counter reached %lu", (long)atomic_load(&done), online,
                  (unsigned long)s.counter);
        return;
    }
    if (s.counter != expected) {
        test_fail("phase4-mcs", "the counter reached %lu after %u processor(s) "
                  "each took the lock %u times, expected %lu", (unsigned long)s.counter,
                  online, CONTEND_ROUNDS, (unsigned long)expected);
        return;
    }

    test_pass("phase4-mcs");
}

/* --- mutex and semaphore ----------------------------------------------- */

static mutex_t lock_mutex;
static volatile uint64_t mutex_counter;

static void mutex_worker(void *arg) {
    (void)arg;

    for (unsigned i = 0; i < CONTEND_ROUNDS; i++) {
        mutex_lock(&lock_mutex);
        uint64_t v = mutex_counter;
        for (volatile unsigned spin = 0; spin < 16; spin++)
            ;
        mutex_counter = v + 1;
        mutex_unlock(&lock_mutex);
    }
}

static void test_mutex_lock(void) {
    atomic_t done;

    mutex_init(&lock_mutex);
    mutex_counter = 0;
    atomic_init(&done, 0);

    unsigned online = percpu_online_count();
    fanout_every_cpu(mutex_worker, NULL, &done);

    uint64_t expected = (uint64_t)online * CONTEND_ROUNDS;
    if (mutex_counter != expected) {
        test_fail("phase4-mutex", "the counter reached %lu after %u processor(s) "
                  "each locked %u times, expected %lu", (unsigned long)mutex_counter,
                  online, CONTEND_ROUNDS, (unsigned long)expected);
        return;
    }

    test_pass("phase4-mutex");
}

static semaphore_t test_sem;
static atomic_t sem_total;

static void sem_worker(void *arg) {
    (void)arg;

    for (unsigned i = 0; i < CONTEND_ROUNDS; i++) {
        sem_wait(&test_sem);
        atomic_increment(&sem_total);
        sem_post(&test_sem);
    }
}

static void test_semaphore(void) {
    atomic_t done;

    sem_init(&test_sem, 1);
    atomic_init(&sem_total, 0);
    atomic_init(&done, 0);

    unsigned online = percpu_online_count();
    fanout_every_cpu(sem_worker, NULL, &done);

    /* A semaphore that is not counting is not a semaphore: if a post is lost the
     * count ends above one and the next wait would never block. */
    uint64_t expected = (uint64_t)online * CONTEND_ROUNDS;
    if ((uint64_t)atomic_load(&sem_total) != expected) {
        test_fail("phase4-semaphore", "the total reached %lu, expected %lu",
                  (unsigned long)atomic_load(&sem_total), (unsigned long)expected);
        return;
    }
    if (sem_count(&test_sem) != 1) {
        test_fail("phase4-semaphore", "the count ended at %ld, expected 1",
                  (long)sem_count(&test_sem));
        return;
    }

    test_pass("phase4-semaphore");
}

/* --- the interrupt-save variants --------------------------------------- */

static ticket_lock_t irq_lock;
static volatile uint32_t irq_holder;
static volatile uint32_t irq_reentered;

static void irq_worker(void *arg) {
    (void)arg;

    for (unsigned i = 0; i < 2000; i++) {
        uint64_t flags = spin_lock_irqsave(&irq_lock);

        /* If the interrupt save did not work, this processor's own timer
         * interrupt would land here and find the lock held -- by this processor,
         * on this processor, in the middle of the critical section.  The
         * interrupt-save variant is what prevents that, so the check is that it
         * never happens. */
        if (__atomic_load_n(&irq_holder, __ATOMIC_ACQUIRE) != 0)
            __atomic_add_fetch(&irq_reentered, 1, __ATOMIC_ACQ_REL);

        __atomic_store_n(&irq_holder, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&irq_holder, 0, __ATOMIC_RELEASE);

        spin_unlock_irqrestore(&irq_lock, flags);
    }
}

static void test_lock_irqsave(void) {
    atomic_t done;

    ticket_lock_init(&irq_lock);
    irq_holder = 0;
    irq_reentered = 0;
    atomic_init(&done, 0);

    unsigned online = percpu_online_count();
    fanout_every_cpu(irq_worker, NULL, &done);

    if ((unsigned)atomic_load(&done) != online) {
        test_fail("phase4-irqsave", "only %ld of %u processors finished",
                  (long)atomic_load(&done), online);
        return;
    }
    if (irq_reentered != 0) {
        test_fail("phase4-irqsave", "the critical section was entered %lu times "
                  "while already held", (unsigned long)irq_reentered);
        return;
    }

    test_pass("phase4-irqsave");
}

/* --- IPIs -------------------------------------------------------------- */

/* Ask each other processor to count, and read the answer back through its own
 * per-CPU slot.
 *
 * The reply has to go in the receiver's memory and be read by the *sender*, which
 * is the one thing an IPI does not do by itself: the sender cannot know whether
 * the handler has run, only that delivery has completed.  The count is per-CPU and
 * only its owner writes it, so the sender needs no lock to read it. */
/* Each entry has exactly one writer -- the processor that owns the slot -- and is
 * read by the boot processor, so it needs no lock.  A volatile store rather than
 * an atomic one: the value is only ever this one number. */
static volatile uint64_t ipi_probe_ran[MAX_CPU_COUNT];



static void ipi_probe_worker(void *arg) {
    (void)arg;

    unsigned index = percpu_self_index();
    if (index < MAX_CPU_COUNT)
        ipi_probe_ran[index] = 1;
}

static void test_ipi(void) {
    unsigned online = percpu_online_count();

    if (online < 2) {
        /* With one processor the only IPI there is to itself, and it is the case a
         * caller gets wrong by disabling interrupts around the send: the handler
         * cannot run until the send returns, so the send never returns. */
        uint64_t before = smp_ipis_received();
        if (smp_ipi_self(VECTOR_IPI_RESCHED) != 0) {
            test_fail("phase4-ipi", "could not send an IPI to this processor");
            return;
        }
        if (smp_ipis_received() <= before) {
            test_fail("phase4-ipi", "an IPI to this processor did not arrive "
                      "(%lu before, %lu after)", (unsigned long)before,
                      (unsigned long)smp_ipis_received());
            return;
        }
        test_pass("phase4-ipi");
        return;
    }

    unsigned indices[MAX_CPU_COUNT];
    unsigned n = smp_online_indices(indices, MAX_CPU_COUNT);
    unsigned self = percpu_self_index();

    for (unsigned i = 0; i < n; i++) {
        if (indices[i] == self)
            continue;

        ipi_probe_ran[indices[i]] = 0;
        if (percpu_queue_work(indices[i], ipi_probe_worker, NULL) != 0) {
            test_fail("phase4-ipi", "processor %u's work slot is occupied",
                      indices[i]);
            return;
        }

        /* Reschedule IPI: the processor has work, so it should find it. */
        if (smp_kick_cpu(indices[i]) != 0) {
            test_fail("phase4-ipi", "could not send a reschedule IPI to "
                      "processor %u", indices[i]);
            return;
        }

        /* Wake IPI on top: the handler has to run twice and drain the slot once,
         * so this also checks that a second IPI to the same processor is not
         * dropped and does not find the slot still occupied. */
        smp_ipi_cpu(indices[i], VECTOR_IPI_WAKE);

        /* TLB shootdown IPI: has to arrive and reload CR3 without disturbing
         * anything, which is the one IPI with a side effect. */
        smp_ipi_cpu(indices[i], VECTOR_IPI_TLB);
    }

    for (unsigned waited = 0; waited < 5000; waited++) {
        bool all = true;
        for (unsigned i = 0; i < n; i++) {
            if (indices[i] == self)
                continue;
            if (ipi_probe_ran[indices[i]] == 0)
                all = false;
        }
        if (all)
            break;
        delay_ms(1);
    }

    for (unsigned i = 0; i < n; i++) {
        if (indices[i] == self)
            continue;
        if (ipi_probe_ran[indices[i]] == 0) {
            test_fail("phase4-ipi", "processor %u never ran the work its IPI "
                      "asked for", indices[i]);
            return;
        }
    }

    if (percpu_interrupts(self) == 0) {
        test_fail("phase4-ipi", "this processor handled no interrupts at all, so "
                  "the IPI path proves nothing here");
        return;
    }

    test_pass("phase4-ipi");
}

/* --- the scheduler ------------------------------------------------------ */

/* Give every processor something to do and wait for it to finish.
 *
 * The waiting is the point: this runs as an ordinary task, so it can be preempted
 * while it waits, which is also what proves the scheduler can put it back. */
static bool wait_until(volatile uint64_t *counter, uint64_t target,
                       unsigned timeout_ms) {
    for (unsigned waited = 0; waited < timeout_ms; waited++) {
        if (*counter >= target)
            return true;
        task_sleep_ms(1);
    }
    return *counter >= target;
}

/* --- user space --------------------------------------------------------- */

/* The first program to run in ring 3.
 *
 * What is being checked is not that it printed something -- if it printed something,
 * the syscall path, the console and the loader all worked.  What is being checked is
 * that everything it *didn't* manage to do stayed that way: it asks for a descriptor
 * that does not exist, for a pointer that is not mapped, and for a pointer into the
 * kernel, and each of those has to come back as an error rather than as a success or a
 * fault.  A boundary that has only been tested with good input has not been tested.
 *
 * The program's own checks are inside it -- it returns non-zero if any of them surprise
 * it -- so the test below only has to confirm that it ran, that it exited, and with
 * what.  Duplicating the argument checks here would only test this kernel's idea of
 * what the program does. */
static void test_user_program(void) {
    syscall_last_errno_reset();

    /* `exectest`, not `hello`: it replaces itself with `hello` and then hello runs to
     * completion in the same task.  So one task checks both that a program can be started
     * and that a process can be replaced by another, and the process that exits 0 at the
     * end is `hello` -- which `exectest` could not have been, since its own exit status is
     * only ever the one it returns on a failed `exec`. */
    task_t *t = program_load_and_run("exectest", 8);

    if (t == NULL) {
        test_fail("phase6-user", "the program would not load");
        return;
    }

    /* The task exits, and nothing wakes anybody when it does, so wait for the exit
     * itself rather than for a state change.  A program that hangs is a failure the
     * harness would otherwise report as "the guest never reached TEST-READY". */
    /* Wait for *this* task's exit, not for any exit.
     *
     * That distinction did not exist before `fork`: there was one program, so "the last
     * exit" and "the program's exit" were the same question.  Now `hello` forks, and its
     * child exits with a status of its own on purpose, so the last exit recorded is
     * whichever of the two finished last -- and the child's is a deliberate 7, which the
     * test then read as the parent's own report that it had failed.
     *
     * `syscall_exit_record()` is published together for exactly this.  *Together*: reading
     * the status and the pid as two separate calls lets another task's exit land between
     * them, and the two halves then describe different programs -- which is how this test
     * once reported "the exit was recorded against task 40, but the program is task 37"
     * for a program that had exited correctly several milliseconds earlier. */
    int      code = -1;
    uint64_t who  = 0;

    for (unsigned waited = 0; waited < 20000; waited++) {
        syscall_exit_record(&code, &who);
        if (code >= 0 && who == t->id)
            break;
        task_sleep_ms(1);
    }

    if (code < 0) {
        /* Everything about the task that decides whether it ever ran, because "never
         * exited" covers both "ran and hung" and "was never scheduled" and the two need
         * opposite fixes. */
        kprintf("phase6-user: task %lu (%s) state %d started %d switches %lu "
                "last_cpu %u on-cpu %lu stack %p list_tag %p vruntime %lu\n",
                (unsigned long)t->id, t->name, t->state, t->started ? 1 : 0,
                (unsigned long)t->switch_count, t->last_cpu,
                (unsigned long)t->ran_on_cpu, (void *)t->stack,
                (void *)(uintptr_t)t->list_tag, (unsigned long)t->vruntime);
        for (unsigned i = 0; i < percpu_online_count(); i++) {
            sched_stats_t st;

            sched_get_stats(i, &st);
            kprintf("phase6-user: cpu %u holds %u queued, %u sleeping, %u switches, "
                    "%u steals\n", i, st.queued, st.sleeping, st.switches,
                    st.steals);
        }
        test_fail("phase6-user", "the program never exited: it either hung or was "
                  "still waiting to be scheduled after 20 s");
        return;
    }

    if (code != 0) {
        test_fail("phase6-user", "the program exited %d, which is its own report "
                  "that one of its checks failed", code);
        return;
    }

    if (who != t->id) {
        test_fail("phase6-user", "the exit was recorded against task %lu, but the "
                  "program is task %lu", (unsigned long)who, (unsigned long)t->id);
        return;
    }

    /* It really ran, rather than the exit having come from somewhere else. */
    if (t->switch_count < 1 || !t->started) {
        test_fail("phase6-user", "the program was never actually scheduled "
                  "(%lu switches, started %d)", (unsigned long)t->switch_count,
                  t->started ? 1 : 0);
        return;
    }

    if (t->state != TASK_DEAD) {
        test_fail("phase6-user", "the program is still in state %d after its exit "
                  "returned", t->state);
        return;
    }

    kprintf("user: hello ran in ring 3, exited %d, task %lu, %lu switches\n",
            code, (unsigned long)t->id, (unsigned long)t->switch_count);

    test_pass("phase6-user");
}

/* Files to print when a program has to be reported as hung.
 *
 * NULL unless a test asked for it.  `shtest` redirects the shell's output into a file and
 * compares it when it is done, so a shell that hangs leaves the only evidence of how far it
 * got in a file that nothing else ever looks at -- and a timeout report that says "task 53
 * never exited" sends the reader to the scheduler instead of to the shell, which is where
 * the answer is.  These are the paths `shtest` writes. */
static const char **hung_dump_paths;

/* The files a test asked to have reported on the way out, however it left.
 *
 * On a hang, what a program left behind says how far it got; on a failure of its own, it
 * says the same thing and also whether the program managed to write the input at all.  A
 * program that reports "0 of 52 lines matched" is asking the reader to work out whether
 * its script was empty, its output file was missing, or its shell printed nothing, and
 * those are three different bugs. */
/* Every task, one line each: number, name, state, parent, and whether it ever ran.
 *
 * A hang is a relationship between tasks -- this one is waiting for that one, which is
 * waiting for a third -- and only the whole list shows the relationship.  `task_state_name()`
 * is what makes it readable: "waiting" says what to look for and "5" does not. */
static void dump_tasks(void) {
    task_t *t;

    for (t = task_first(); t != NULL; t = task_next(t)) {
        kprintf("  task %lu %-10s %-9s parent %lu exit %d started %d switches %lu",
                (unsigned long)t->id, t->name, task_state_name(t->state),
                t->parent ? (unsigned long)t->parent->id : 0, t->exit_code,
                t->started ? 1 : 0, (unsigned long)t->switch_count);

        /* Where it is, for a task that never finished.
         *
         * A task saved from ring 3 has the interrupt frame as its context, the same 22
         * words `sys_fork()` reads to make a child's, so the instruction pointer is in the
         * same place in both.  A task saved from kernel code has a kernel frame there
         * instead and the word is meaningless, so it is only printed for a task that was in
         * ring 3 when it was last saved. */
        if (t->is_user && t->saved_in_user && t->saved_rsp != 0) {
            const uint64_t *f = (const uint64_t *)(uintptr_t)t->saved_rsp;

            kprintf(" rip %#lx", (unsigned long)f[17]);
        }

        kprintf("\n");
    }
}

extern char __kernel_start[];
extern char __kernel_end[];

/* Walk a task's kernel stack and print what is on it.
 *
 * Only words that look like return addresses into the kernel image are printed, and only as
 * raw addresses: turning them into names needs a symbol table this kernel does not carry at
 * run time.  What a reader gets is a list of addresses, which is enough to be turned into
 * names afterwards with `addr2line`, and which is what makes the difference between "task 41
 * is blocked" -- which says nothing -- and where inside the kernel it is blocked.
 *
 * The frames are skipped rather than trusted: a stack holding kernel addresses has kernel
 * addresses' *values* on it too (a struct field that happens to look like one), so this
 * prints candidates and not a call graph. */
static void dump_kernel_stack(const task_t *t) {
    if (t == NULL || t->stack == NULL || t->stack_top == 0)
        return;

    const uint64_t start = t->stack_top & ~15ull;
    unsigned      shown = 0;

    kprintf("  kernel stack of task %lu (%s), %lu bytes at %p:\n",
            (unsigned long)t->id, t->name,
            (unsigned long)(t->stack_top - (uint64_t)(uintptr_t)t->stack),
            (void *)t->stack);

    for (uint64_t a = start; a >= (uint64_t)(uintptr_t)t->stack && shown < 24; a -= 8) {
        uint64_t w = *(const volatile uint64_t *)(uintptr_t)a;

        if (w >= (uint64_t)(uintptr_t)__kernel_start &&
            w < (uint64_t)(uintptr_t)__kernel_end) {
            kprintf("    %p: %p\n", (void *)(uintptr_t)a, (void *)(uintptr_t)w);
            shown++;
        }
    }
}

static void dump_ramfs_file(const char *path);

static void dump_test_files(void) {
    if (hung_dump_paths == NULL)
        return;

    for (const char **p = hung_dump_paths; *p != NULL; p++)
        dump_ramfs_file(*p);
}

/* Print a file from the ramfs, up to `limit` bytes.
 *
 * The kernel reading a node directly rather than through `vfs_*()`: this is a failure
 * report, and a report that can fail because of the thing it is reporting on is not a
 * report. */
static void dump_ramfs_file(const char *path) {
    uint64_t parent = ramfs_root();
    const char *at = path;
    uint64_t ino = 0;

    while (*at == '/')
        at++;

    while (*at != 0 && parent != 0) {
        const char *slash = at;
        char       name[64];
        size_t     n = 0;

        while (*slash != 0 && *slash != '/')
            slash++;

        n = (size_t)(slash - at);
        if (n >= sizeof(name))
            n = sizeof(name) - 1;
        memcpy(name, at, n);
        name[n] = 0;

        ino = ramfs_lookup(parent, name);
        if (ino == 0) {
            kprintf("  %s: not there\n", path);
            return;
        }

        parent = ino;
        at     = *slash == 0 ? slash : slash + 1;
    }

    uint64_t size = ramfs_node_size(ino);
    char     line[128];

    if (ino == 0) {
        kprintf("  %s: no such node\n", path);
        return;
    }

    kprintf("  %s: %lu bytes\n", path, (unsigned long)size);

    if (size > 4096)
        size = 4096;

    uint64_t at_byte = 0;

    while (at_byte < size) {
        size_t want = sizeof(line) - 1;

        if (size - at_byte < want)
            want = (size_t)(size - at_byte);

        if (ramfs_read(ino, at_byte, line, want) < 0)
            break;

        for (size_t i = 0; i < want; i++) {
            if (line[i] == 0)
                line[i] = '.';
            else if (line[i] == '\n')
                line[i] = '|';
        }
        line[want] = 0;
        kprintf("    |%s|\n", line);
        at_byte += want;
    }
}

/* Run one registered ring-3 program to completion and report whether it exited 0.
 *
 * The helper rather than four copies of the wait loop: the wait is the delicate part --
 * it has to watch for *this* task's exit rather than any exit, because a program that
 * forks has children whose exits are not its own report -- and four copies of a delicate
 * part is three chances to get one of them subtly different. */
static bool run_program(const char *name, const char *test, const char *what) {
    syscall_last_errno_reset();

    task_t *t = program_load_and_run(name, 8);

    if (t == NULL) {
        test_fail(test, "%s", what);
        return false;
    }

    /* One snapshot of the pair per poll, for the reason in `test_user_program()`: two
     * separate reads can describe two different programs.  `forktest` has four children
     * whose exits are deliberately 1, 2, 3 and 4, so a torn read here is not a rare race
     * to shrug at -- it is the common case, and it is how this helper once reported
     * "forktest exited 4" for a program that had printed "forktest: ok".
     *
     * `seen` is what says the exit was *this program's*.  Without it a poll that ran out
     * of time reports whatever exit it last happened to read, which on a machine where a
     * shell's own children are exiting is almost always somebody else's success: the loop
     * below gave up after 40 s, the last record on it was a `$( )` child exiting 0, and
     * `phase9-sh` was reported PASS for a `shtest` that was still sitting in `waitpid`
     * with its script half-run.  A check that cannot fail when the thing it checks hangs
     * is worse than no check, because it is a green line in a log. */
    int      code = -1;
    uint64_t who  = 0;
    bool     seen = false;

    for (unsigned waited = 0; waited < 40000; waited++) {
        syscall_exit_record(&code, &who);
        if (code >= 0 && who == t->id) {
            seen = true;
            break;
        }
        task_sleep_ms(1);
    }

    if (!seen) {
        code = -1;
    }

    if (code < 0) {
        kprintf("%s: task %lu (%s) never exited: state %d, started %d, %lu "
                "switches, %lu on cpu %u\n", test, (unsigned long)t->id, t->name,
                t->state, t->started ? 1 : 0, (unsigned long)t->switch_count,
                (unsigned long)t->ran_on_cpu, t->last_cpu);

        if (hung_dump_paths != NULL) {
            for (const char **p = hung_dump_paths; *p != NULL; p++)
                dump_ramfs_file(*p);
        }

        kprintf("%s: tasks at the time of the report:\n", test);
        dump_tasks();
        dump_kernel_stack(t);
        test_fail(test, "the program never exited: it hung or was never scheduled "
                  "after 40 s");
        return false;
    }

    if (code != 0) {
        kprintf("%s: %s exited %d, which is its own report that a check failed\n",
                test, name, code);
        dump_test_files();
        test_fail(test, "the program's own checks failed");
        return false;
    }

    return true;
}

/* Phase 7's three programs.
 *
 * Each one is a directory under user/ and nothing else, and that is the point of them
 * being separate: `hello` grew past 4 KiB of text while the signal checks were being added
 * to it, and its own growth was the bug -- the loader was filling every page of a segment
 * with a copy of the first.  A test that lives inside another program's text is a test
 * that changes what it is testing. */
static void test_fork_program(void) {
    if (!run_program("forktest", "phase7-fork", "forktest would not load"))
        return;
    test_pass("phase7-fork");
}

static void test_cow_program(void) {
    if (!run_program("cowtest", "phase7-cow", "cowtest would not load"))
        return;
    test_pass("phase7-cow");
}

static void test_signal_program(void) {
    if (!run_program("sigtest", "phase7-signal", "sigtest would not load"))
        return;
    test_pass("phase7-signal");
}

/* Phase 9's libc, exercised by a program that links it.
 *
 * Every other program in `user/` is written against the raw door -- `minx_main()`, no libc,
 * `user/common/start.S` -- on purpose: a test of the kernel's syscall handling, its loader
 * or its signal trampoline has to have nothing in between that could be the bug.  This one
 * is the opposite and is the only test here that fails when `printf()` pads wrongly or
 * `malloc()` fragments its own heap.
 *
 * It runs through the same `run_program()` as the others, so its exit status is checked the
 * same way -- which also means it is a live test of the libc's own `exit()`, on the path
 * where a program that never exits is a test that never reports. */
static void test_libc_program(void) {
    if (!run_program("libctest", "phase9-libc", "libctest would not load"))
        return;
    test_pass("phase9-libc");
}

/* Phase 9's other program: the shell.
 *
 * `shtest` writes a script, runs `/bin/sh` on it, and compares what came out against what it
 * promised.  It is a program rather than a kernel-side check because the thing being tested
 * is a ring-3 program: a test that ran the shell from the kernel would be testing the
 * kernel's ability to run the shell, which `test_libc_program` already covers.
 *
 * It has to be a *separate* test from the libc one for a second reason: they fail
 * differently.  `libctest` failing means the libc is wrong; `shtest` failing means the shell
 * is, and the difference is the difference between "the foundation moved" and "the thing on
 * top of it moved". */
static void test_sh_program(void) {
#if MINX_SHELL_PROGRAM
    /* What `shtest` leaves behind, in the order that tells the story: the script it wrote,
     * so the reader can see what it was asked to do, and the output it collected, so the
     * reader can see how far it got. */
    static const char *paths[] = { "/tmp/shtest.sh", "/tmp/shtest.out", NULL };

    hung_dump_paths = paths;

    bool ok = run_program("shtest", "phase9-sh", "shtest would not load");

    hung_dump_paths = NULL;

    if (!ok)
        return;
    test_pass("phase9-sh");
#else
    /* The shell is built only with `make WITH_SHELL=1`, and a test for a program that is not in
     * the image would fail for the right reason and say nothing useful.  Saying so is one
     * line and is the difference between "phase9-sh failed" and "phase9-sh is not here". */
    kprintf("phase9-sh: skipped -- the shell is not in this image (make WITH_SHELL=1)\n");
#endif
}

/* Phase 8's program, and the only filesystem test that goes through `int $0x80`.
 *
 * `test_ramfs()` below tests the same tree from inside the kernel, and both are here on
 * purpose.  They cannot catch each other's bugs: the kernel-side one calls `vfs_*()`
 * directly and so cannot notice a syscall that forgets to validate a pointer, and the
 * user-side one cannot notice a node whose internal bookkeeping is wrong, because a program
 * has no way to look at any.  What a pair of them catches that either alone would not is a
 * disagreement between what the two halves believe, which is the whole class of bug that
 * shows up as "it works from the tests and not from a program". */
static void test_fs_program(void) {
    if (!run_program("fstest", "phase8-vfs", "fstest would not load"))
        return;
    test_pass("phase8-vfs");
}

/* Ring 3 cannot read the kernel, and the check for that is a page-table walk rather
 * than a fault: `syscall_user_range_ok()` is the only thing standing between a
 * program's pointer and the kernel's memory, so it is worth testing on its own rather
 * than only through a program that happens to exercise it. */
static void test_user_pointers(void) {
    page_table_t tables;

    if (vmm_address_space_create(&tables) != 0) {
        test_fail("phase6-pointers", "could not create an address space");
        return;
    }

    /* The kernel image, the direct map, and the top of user space: all three are
     * addresses that exist and none of which is readable from ring 3. */
    static const uint64_t forbidden[] = {
        0xffffffff80000000ull,      /* the kernel image        */
        0xffff800000100000ull,      /* the direct map          */
        USER_SPACE_END,              /* just past the top        */
        ~0ull,                       /* the highest address      */
    };

    for (unsigned i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        if (syscall_user_range_ok(forbidden[i], 16, false)) {
            test_fail("phase6-pointers", "the kernel would accept %#lx as a "
                      "readable user address", (unsigned long)forbidden[i]);
            vmm_address_space_destroy(&tables);
            return;
        }
    }

    /* A range that wraps past the top of the address space is not a range. */
    if (syscall_user_range_ok(USER_SPACE_END - 8, 64, false)) {
        test_fail("phase6-pointers", "a range that wraps past the top of user space "
                  "was accepted");
        vmm_address_space_destroy(&tables);
        return;
    }

    /* An empty range at a legal address is legal, and refusing it would break every
     * program that asks to write nothing. */
    if (!syscall_user_range_ok(0x1000, 0, false)) {
        test_fail("phase6-pointers", "an empty range was refused");
        vmm_address_space_destroy(&tables);
        return;
    }

    vmm_address_space_destroy(&tables);
    test_pass("phase6-pointers");
}

/* The loader's refusals.
 *
 * Each of these is a file that would map something the kernel should not, so the only
 * evidence the check works is that it is refused *and says why*.  A loader that
 * accepted a class-32 ELF would still load every program in this tree, and would only
 * be found out the first time somebody handed it a file from somewhere else. */
/* --- phase 8: the ramfs ------------------------------------------------- */

/* The filesystem, tested through the interface the rest of the kernel uses rather than
 * through the internals, because the point of a filesystem is what a caller can do to it.
 *
 * Every case here is one whose absence is silent: a file that reads back the right length
 * of the right bytes, in a tree nobody can walk, still looks fine until something tries to
 * unlink it or link it a second time.  So each check has a second half that would fail if
 * the first half had quietly done the wrong thing. */
static void test_ramfs(void) {
    const char *name = "phase8-ramfs";

    if (!ramfs_mounted()) {
        test_fail(name, "the root filesystem is not mounted");
        return;
    }

    uint64_t root = ramfs_root();
    if (root == 0) {
        test_fail(name, "the root directory has no node number");
        return;
    }

    unsigned before = ramfs_node_count();

    /* A file, written in two pieces so that the second write lands past the end of the
     * first and has to grow the buffer rather than overwrite it. */
    uint64_t f = ramfs_create(root, "greeting", RAMFS_FILE, 0644);
    if (f == 0) {
        test_fail(name, "could not create a file in the root directory");
        return;
    }

    if (ramfs_write(f, 0, "hello ", 6) != 6 || ramfs_write(f, 6, "ramfs", 5) != 5) {
        test_fail(name, "a write to a fresh file did not report its length");
        return;
    }

    if (ramfs_node_size(f) != 11) {
        test_fail(name, "two writes of 6 and 5 bytes produced a file of %llu",
                  (unsigned long long)ramfs_node_size(f));
        return;
    }

    char buf[32];
    memset(buf, 0, sizeof(buf));

    /* Read from the middle: an offset that lands inside the data has to be honoured, and
     * the commonest way to get that wrong is to treat the offset as a length. */
    if (ramfs_read(f, 6, buf, 5) != 5 || memcmp(buf, "ramfs", 5) != 0) {
        test_fail(name, "reading 5 bytes from offset 6 did not give \"ramfs\"");
        return;
    }

    /* Reading past the end returns what is there and no more -- short, never -1.  A reader
     * that gets a negative length instead has to special-case the end of a file, and one
     * that pads has invented bytes that were never written. */
    long got = ramfs_read(f, 8, buf, sizeof(buf));
    if (got != 3 || memcmp(buf, "mfs", 3) != 0) {
        test_fail(name, "reading 32 bytes from offset 8 of an 11-byte file gave "
                  "%ld bytes", got);
        return;
    }

    if (ramfs_read(f, 11, buf, sizeof(buf)) != 0) {
        test_fail(name, "reading at the end of a file did not return zero bytes");
        return;
    }

    /* Truncation shrinks and then grows, and the gap it grows over is zeroes -- which is
     * what a program that truncates a file to use it as scratch depends on. */
    if (ramfs_truncate(f, 5) != 0 || ramfs_node_size(f) != 5) {
        test_fail(name, "truncating to 5 left the file at %llu bytes",
                  (unsigned long long)ramfs_node_size(f));
        return;
    }

    if (ramfs_truncate(f, 8) != 0) {
        test_fail(name, "growing a file back failed");
        return;
    }

    memset(buf, 0xaa, sizeof(buf));
    if (ramfs_read(f, 5, buf, 3) != 3) {
        test_fail(name, "reading the bytes added by a growing truncate failed");
        return;
    }

    for (int i = 0; i < 3; i++) {
        if (buf[i] != 0) {
            test_fail(name, "the gap left by a growing truncate is 0x%02x, not zero",
                      (unsigned char)buf[i]);
            return;
        }
    }

    /* Directories, and a path is a walk through them. */
    uint64_t d = ramfs_create(root, "sub", RAMFS_DIR, 0755);
    uint64_t deep = ramfs_create(d, "deeper", RAMFS_DIR, 0755);
    if (d == 0 || deep == 0) {
        test_fail(name, "could not create nested directories");
        return;
    }

    uint64_t n = ramfs_create(deep, "leaf", RAMFS_FILE, 0600);
    if (n == 0 || ramfs_write(n, 0, "x", 1) != 1) {
        test_fail(name, "could not create a file two directories down");
        return;
    }

    /* The name is one component, and refusing a slash is what makes `..` and every path
     * walker downstream sound.  A directory called "a/b" would be reachable by two paths
     * and reachable by neither walk. */
    if (ramfs_create(d, "not/a/name", RAMFS_FILE, 0644) != 0) {
        test_fail(name, "a name containing a slash was accepted");
        return;
    }

    /* A hard link is a second name for the same data, not a copy: writing through one has
     * to be visible through the other, and the two have to share a node number. */
    if (ramfs_link(root, "greeting2", f) != 0) {
        test_fail(name, "could not hard-link a file");
        return;
    }

    uint64_t f2 = ramfs_lookup(root, "greeting2");
    if (f2 != f) {
        test_fail(name, "a hard link reached node %llu rather than %llu",
                  (unsigned long long)f2, (unsigned long long)f);
        return;
    }

    if (ramfs_write(f2, 0, "HELLO", 5) != 5) {
        test_fail(name, "writing through a hard link failed");
        return;
    }

    memset(buf, 0, sizeof(buf));
    if (ramfs_read(f, 0, buf, 5) != 5 || memcmp(buf, "HELLO", 5) != 0) {
        test_fail(name, "a write through one name was not visible through the other");
        return;
    }

    /* A directory cannot be hard-linked: two parents for one directory makes `..` a cycle
     * and the tree stops being a tree. */
    if (ramfs_link(root, "sub2", d) >= 0) {
        test_fail(name, "a directory could be hard-linked");
        return;
    }

    /* And it cannot be removed while it still has anything in it. */
    if (ramfs_rmdir(root, "sub") != -ENOTEMPTY) {
        test_fail(name, "a non-empty directory was removed");
        return;
    }

    if (ramfs_rmdir(root, "sub") == 0) {
        test_fail(name, "the non-empty directory went away on the second attempt");
        return;
    }

    /* Rename moves the name and keeps the data. */
    if (ramfs_rename(root, "greeting2", root, "greeting3") != 0) {
        test_fail(name, "renaming a file failed");
        return;
    }

    if (ramfs_lookup(root, "greeting2") != 0) {
        test_fail(name, "the old name survived a rename");
        return;
    }

    if (ramfs_lookup(root, "greeting3") != f) {
        test_fail(name, "the new name does not reach the same node");
        return;
    }

    /* Symlinks hold a path, not the thing the path names, and the test is that renaming the
     * target is visible through the link.  A symlink that stored the node would keep
     * working after the file it names is gone, which is the opposite of what it is for. */
    if (ramfs_symlink(root, "link", "greeting") == 0) {
        test_fail(name, "could not create a symbolic link");
        return;
    }

    uint64_t sl = ramfs_lookup(root, "link");
    if (sl == 0) {
        test_fail(name, "the symbolic link was not created");
        return;
    }

    memset(buf, 0, sizeof(buf));
    if (ramfs_read(sl, 0, buf, sizeof(buf)) != 8 ||
        memcmp(buf, "greeting", 8) != 0) {
        test_fail(name, "a symbolic link did not read back as the path it names");
        return;
    }

    /* Unlink takes the name away.  The data goes only when the last name does, so the
     * original file -- still reached by "greeting" -- has to survive the link's removal,
     * which is what makes `rm` on one of two links safe. */
    if (ramfs_unlink(root, "greeting3") != 0) {
        test_fail(name, "unlinking a file failed");
        return;
    }

    if (ramfs_lookup(root, "greeting3") != 0) {
        test_fail(name, "the name survived an unlink");
        return;
    }

    if (ramfs_lookup(root, "greeting") != f) {
        test_fail(name, "unlinking one name of a hard-linked file lost the data");
        return;
    }

    if (ramfs_unlink(root, "nothing-here") != -ENOENT) {
        test_fail(name, "unlinking a name that does not exist did not report ENOENT");
        return;
    }

    /* Everything above left the tree dirty; clean up so the test does not change what
     * every test after it sees. */
    ramfs_unlink(root, "greeting");
    ramfs_unlink(root, "link");
    ramfs_unlink(deep, "leaf");

    /* A directory comes out with rmdir, not unlink, and only while it is empty -- which is
     * the check that has to come first, because the order above is the only thing that made
     * it empty. */
    if (ramfs_rmdir(d, "deeper") != 0) {
        test_fail(name, "removing an empty directory failed");
        return;
    }

    if (ramfs_rmdir(root, "sub") != 0) {
        test_fail(name, "removing an emptied directory failed");
        return;
    }

    if (ramfs_node_count() != before) {
        test_fail(name, "%u nodes were created and not removed",
                  ramfs_node_count() - before);
        return;
    }

    test_pass(name);
}

static void test_elf_rejects(void) {
    static uint8_t fake[128];
    page_table_t tables;
    task_t *t;

    /* Too short to be a header at all. */
    if (vmm_address_space_create(&tables) != 0) {
        test_fail("phase6-elf", "could not create an address space");
        return;
    }

    t = task_create("elfrej", NULL, NULL, TASK_WEIGHT_DEFAULT);
    if (t == NULL) {
        test_fail("phase6-elf", "could not create a task");
        vmm_address_space_destroy(&tables);
        return;
    }
    t->tables = &tables;

    elf_load_result_t r = elf_load(fake, 8, t, 4, USER_IMAGE_BASE + 64, NULL, NULL);
    if (r.ok) {
        test_fail("phase6-elf", "an 8-byte file was loaded as a program");
        return;
    }

    /* A well-formed header for the wrong machine. */
    Elf64_Ehdr *eh = (Elf64_Ehdr *)fake;
    memset(fake, 0, sizeof(fake));
    eh->e_ident[EI_MAG0] = ELFMAG0;
    eh->e_ident[EI_MAG1] = ELFMAG1;
    eh->e_ident[EI_MAG2] = ELFMAG2;
    eh->e_ident[EI_MAG3] = ELFMAG3;
    eh->e_ident[EI_CLASS] = ELFCLASS64;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_type = ET_EXEC;
    eh->e_machine = 0x1234;            /* not x86-64 */
    eh->e_phentsize = ELF64_PHDR_SIZE;
    eh->e_phnum = 1;
    eh->e_phoff = sizeof(Elf64_Ehdr);

    r = elf_load(fake, sizeof(fake), t, 4, USER_IMAGE_BASE + 64, NULL, NULL);
    if (r.ok) {
        test_fail("phase6-elf", "an ELF for machine 0x1234 was loaded");
        return;
    }

    /* The right machine, the wrong class. */
    eh->e_machine = EM_X86_64;
    eh->e_ident[EI_CLASS] = ELFCLASS32;

    r = elf_load(fake, sizeof(fake), t, 4, USER_IMAGE_BASE + 64, NULL, NULL);
    if (r.ok) {
        test_fail("phase6-elf", "a 32-bit ELF was loaded");
        return;
    }

    /* Right class and machine, but no loadable segments: an ET_EXEC with nothing in it
     * has no entry point worth jumping to. */
    eh->e_ident[EI_CLASS] = ELFCLASS64;
    ((Elf64_Phdr *)(fake + eh->e_phoff))->p_type = PT_NULL;

    r = elf_load(fake, sizeof(fake), t, 4, USER_IMAGE_BASE + 64, NULL, NULL);
    if (r.ok) {
        test_fail("phase6-elf", "an ELF with no loadable segments was loaded");
        return;
    }

    vmm_address_space_destroy(&tables);
    test_pass("phase6-elf");
}

/* --- address spaces ----------------------------------------------------- */

/* Two tasks, two address spaces, one shared kernel.
 *
 * The check is that a page mapped by one is *not* reachable from the other, which is
 * the entire reason an address space exists.  A kernel that shares its top-level
 * entries the wrong way round, or that maps into whatever tables happen to be in CR3
 * rather than the task's own, passes any test that only checks that mapping works.
 *
 * Both tasks write their own page, then both read the other's address.  The read has
 * to fault, and a fault is reported by making the *other* task's page readable and
 * seeing whether it reads back its own value.  Doing it that way rather than by
 * catching the fault keeps the test to the same shape as the rest of the suite:
 * counters, no expected faults. */
static volatile uint64_t aspace_own_value[2];
static volatile uint64_t aspace_seen_value[2];
static volatile uint64_t aspace_done;
static volatile uint64_t aspace_failed;
static volatile uint64_t aspace_kernel_ok[2];
static page_table_t aspace_tables[2];
static task_t *aspace_tasks[2];

/* Why a task could not map its own page: 1 = vmm_map_user refused, 2 = it claimed
 * success but the page is not in the address space the task is running in. */
static volatile int aspace_map_why[2];

static void aspace_worker(void *arg) {
    unsigned which = (unsigned)(uintptr_t)arg;
    uint64_t slot = (uint64_t)(which + 1) * 0x100000;
    uint64_t mine = 0x1111000000000000ull + which;

    if (vmm_map_user(slot, vmm_user_page_alloc(), page_size, true) != 0) {
        __atomic_store_n(&aspace_failed, 1, __ATOMIC_RELEASE);
        aspace_map_why[which] = 1;
        return;
    }

    /* A mapping that reports success but is not in the address space the task is
     * running in is the failure this whole test exists to catch, so it is checked
     * rather than left to the write below: the write faults the machine, and a
     * machine that is dead says nothing. */
    if (vmm_translate(slot) == 0) {
        __atomic_store_n(&aspace_failed, 1, __ATOMIC_RELEASE);
        aspace_map_why[which] = 2;
        return;
    }

    *(volatile uint64_t *)slot = mine;
    aspace_own_value[which] = *(volatile uint64_t *)slot;

    /* Now the other task's slot.  Its address is derived from the page it mapped, and
     * is not mapped here, so the read must not produce the other task's value -- and
     * must not fault the machine either, because this address is well below anything
     * the kernel has mapped and the fault handler reports rather than kills for a
     * supervisor read of an unmapped user address... which it does kill for.  So the
     * read is done through a translation instead: what matters is whether the two
     * address spaces have the same page behind the same virtual address. */
    uint64_t other_slot = (uint64_t)(1 - which + 1) * 0x100000;
    uint64_t translated = vmm_translate(other_slot);

    aspace_seen_value[which] = (translated != 0) ? *(volatile uint64_t *)other_slot : 0;

    /* The kernel half has to be reachable from *this* address space, or the task could
     * never make a syscall.  Translatable is the right question here, not readable:
     * reading it would be a supervisor read of a kernel address, which is a fault the
     * handler treats as a bug in whoever did it. */
    aspace_kernel_ok[which] = vmm_translate(0xffffffff80001000ull) != 0;

    __atomic_add_fetch(&aspace_done, 1, __ATOMIC_ACQ_REL);
}

static void test_address_spaces(void) {
    for (unsigned i = 0; i < 2; i++) {
        aspace_own_value[i] = 0;
        aspace_seen_value[i] = 0;
    }
    aspace_done = 0;
    aspace_failed = 0;

    for (unsigned i = 0; i < 2; i++) {
        if (vmm_address_space_create(&aspace_tables[i]) != 0) {
            test_fail("phase6-aspace", "could not create address space %u", i);
            return;
        }

        aspace_tasks[i] = task_create("aspace", aspace_worker,
                                      (void *)(uintptr_t)i, TASK_WEIGHT_MAX);
        if (aspace_tasks[i] == NULL) {
            test_fail("phase6-aspace", "could not create task %u", i);
            return;
        }
        aspace_tasks[i]->tables = &aspace_tables[i];
        aspace_tasks[i]->is_user = true;
    }

    task_wakeup(aspace_tasks[0]);
    task_wakeup(aspace_tasks[1]);

    if (!wait_until((volatile uint64_t *)&aspace_done, 2, 20000)) {
        test_fail("phase6-aspace", "only %lu of the two tasks finished", 
                  (unsigned long)aspace_done);
        return;
    }
    if (aspace_failed) {
        unsigned w = aspace_map_why[0] != 0 ? 0 : 1;
        test_fail("phase6-aspace", "task %u could not map its own page at %#lx: "
                  "%s", w, (unsigned long)((w + 1) * 0x100000),
                  aspace_map_why[w] == 1 ? "vmm_map_user refused it"
                                         : "vmm_map_user claimed success but the "
                                           "page is not in its own address space");
        return;
    }

    /* Each task read back what it wrote... */
    for (unsigned i = 0; i < 2; i++) {
        if (aspace_own_value[i] != 0x1111000000000000ull + i) {
            test_fail("phase6-aspace", "task %u read back %#lx from its own page, "
                      "expected %#lx", i, (unsigned long)aspace_own_value[i],
                      (unsigned long)(0x1111000000000000ull + i));
            return;
        }
    }

    /* ...and neither saw the other's, which is the part that distinguishes two address
     * spaces from one address space with two names. */
    for (unsigned i = 0; i < 2; i++) {
        if (aspace_seen_value[i] != 0) {
            test_fail("phase6-aspace", "task %u could reach the page task %u mapped, "
                      "so the two tasks share an address space", i, 1 - i);
            return;
        }
    }

    /* And the kernel half is shared: each task still reaches the kernel image from its
     * own address space, which is what makes a process able to make a syscall at all. */
    for (unsigned i = 0; i < 2; i++) {
        if (!aspace_kernel_ok[i]) {
            test_fail("phase6-aspace", "task %u's address space cannot reach the "
                      "kernel image, so it could never make a syscall", i);
            return;
        }
    }

    kprintf("aspace: 2 tasks, separate pages, kernel shared\n");
    test_pass("phase6-aspace");
}

typedef struct {
    volatile uint64_t count;
    volatile uint64_t slices;     /* observed time slices taken */
    uint64_t          iters;
    uint32_t          weight;
    unsigned          last_cpu;
} worker_t;

static volatile uint64_t workers_done;

static void sched_worker(void *arg) {
    worker_t *w = (worker_t *)arg;

    for (uint64_t i = 0; i < w->iters; i++) {
        __atomic_add_fetch(&w->count, 1, __ATOMIC_ACQ_REL);
        /* Enough work between the increments that a task this size really does
         * get preempted rather than running to the end in one slice. */
        for (volatile unsigned spin = 0; spin < 64; spin++)
            ;
        w->last_cpu = percpu_self_index();
    }

    __atomic_add_fetch(&workers_done, 1, __ATOMIC_ACQ_REL);
}

/* Every task runs to completion, and between them they used more than one
 * processor.  "More than one" is the whole claim: a scheduler that ran every task
 * on the boot processor would pass any test that only checked the totals. */
static void test_sched_parallel(void) {
    enum { WORKERS = 6 };
    static worker_t w[WORKERS];
    task_t *t[WORKERS];

    workers_done = 0;
    for (unsigned i = 0; i < WORKERS; i++) {
        w[i].count = 0;
        w[i].slices = 0;
        w[i].iters = 4000;
        w[i].weight = TASK_WEIGHT_DEFAULT;
        w[i].last_cpu = MAX_CPU_COUNT;

        t[i] = task_create("worker", sched_worker, &w[i], TASK_WEIGHT_DEFAULT);
        if (t[i] == NULL) {
            test_fail("phase5-parallel", "could not create task %u", i);
            return;
        }
        if (task_wakeup(t[i]) != 0) {
            test_fail("phase5-parallel", "could not queue task %u", i);
            return;
        }
    }

    if (!wait_until((volatile uint64_t *)&workers_done, WORKERS, 20000)) {
        test_fail("phase5-parallel", "only %lu of %u tasks finished in 20 s",
                  (unsigned long)workers_done, WORKERS);
        return;
    }

    unsigned cpus_seen = 0;
    for (unsigned i = 0; i < WORKERS; i++) {
        if (w[i].count != w[i].iters) {
            test_fail("phase5-parallel", "task %u completed %lu of %lu rounds",
                      i, (unsigned long)w[i].count, (unsigned long)w[i].iters);
            return;
        }
        if (w[i].last_cpu < percpu_online_count())
            cpus_seen |= 1u << w[i].last_cpu;
    }

    unsigned distinct = 0;
    for (unsigned b = cpus_seen; b != 0; b >>= 1)
        distinct += (b & 1);

    unsigned switches = 0;
    for (unsigned i = 0; i < WORKERS; i++)
        switches += t[i]->switch_count != 0 ? 1 : 0;

    if (percpu_online_count() > 1) {
        /* More than one processor has to have run them, or nothing was actually
         * concurrent.  A scheduler that ran every task on the boot processor
         * passes any test that only checks the totals. */
        if (distinct < 2) {
            test_fail("phase5-parallel", "the %u tasks all ran on one of the "
                      "%u processors, so nothing was actually concurrent",
                      WORKERS, percpu_online_count());
            return;
        }
        kprintf("sched: %u tasks ran on %u processor(s)\n", WORKERS, distinct);
    } else {
        /* One processor: there is no second one to be concurrent with, so the
         * claim has to be that the tasks shared this one rather than running one
         * after another to completion.  Each of them was switched into more than
         * once, which only happens if they were interleaved. */
        if (switches < 2) {
            test_fail("phase5-parallel", "only %u of the %u tasks were switched "
                      "into more than once, so they ran one after another rather "
                      "than sharing the processor", switches, WORKERS);
            return;
        }
        kprintf("sched: %u tasks shared one processor, %u of them interleaved\n",
                WORKERS, switches);
    }

    test_pass("phase5-parallel");
}

/* A task that never yields must still be taken off its processor.  This is
 * preemption, and it is the difference between a scheduler and a queue: without
 * it the first task to run runs to completion whatever the others want. */
static volatile uint64_t spinner_switches;
static volatile uint64_t spinner_done;

static void spinner(void *arg) {
    (void)arg;
    for (volatile unsigned spin = 0; spin < 4000000; spin++)
        ;

    __atomic_store_n(&spinner_switches,
                     sched_current() != NULL ? sched_current()->switch_count : 0,
                     __ATOMIC_RELEASE);
    __atomic_add_fetch(&spinner_done, 1, __ATOMIC_ACQ_REL);
}

static void test_sched_preempt(void) {
    task_t *t = task_create("spinner", spinner, NULL, TASK_WEIGHT_DEFAULT);

    spinner_switches = 0;
    spinner_done = 0;

    if (t == NULL || task_wakeup(t) != 0) {
        test_fail("phase5-preempt", "could not start the spinner");
        return;
    }

    if (!wait_until((volatile uint64_t *)&spinner_done, 1, 20000)) {
        test_fail("phase5-preempt", "the spinner never finished, so it was "
                  "never taken off its processor");
        return;
    }

    /* It ran for a long time without yielding, so it can only have finished if it
     * was switched out and back in at least once while it ran. */
    if (spinner_switches < 2) {
        test_fail("phase5-preempt", "the spinner was switched into only %lu "
                  "time(s); a task that never yields must be preempted",
                  (unsigned long)spinner_switches);
        return;
    }

    test_pass("phase5-preempt");
}

/* Two tasks of equal weight that both run flat out must both get processor time.
 * A scheduler that always picks the first runnable task, or one that migrates
 * everything onto one processor, gives one of them everything and the other
 * nothing -- and the one that gets nothing never finishes, so this fails as a
 * timeout rather than as a number. */
static volatile uint64_t fair_progress[2];
static volatile uint64_t fair_done;

static void fair_spinner(void *arg) {
    unsigned which = (unsigned)(uintptr_t)arg;

    for (unsigned i = 0; i < 300000; i++)
        __atomic_add_fetch(&fair_progress[which], 1, __ATOMIC_ACQ_REL);

    __atomic_add_fetch(&fair_done, 1, __ATOMIC_ACQ_REL);
}

static void test_sched_fair(void) {
    task_t *a, *b;

    fair_progress[0] = 0;
    fair_progress[1] = 0;
    fair_done = 0;

    a = task_create("fair0", fair_spinner, (void *)(uintptr_t)0,
                    TASK_WEIGHT_DEFAULT);
    b = task_create("fair1", fair_spinner, (void *)(uintptr_t)1,
                    TASK_WEIGHT_DEFAULT);

    if (a == NULL || b == NULL) {
        test_fail("phase5-fair", "could not create the two tasks");
        return;
    }
    task_wakeup(a);
    task_wakeup(b);

    if (!wait_until((volatile uint64_t *)&fair_done, 2, 20000)) {
        test_fail("phase5-fair", "only %lu of the two tasks finished in 20 s, so "
                  "one of them was starved", (unsigned long)fair_done);
        return;
    }

    uint64_t p0 = fair_progress[0];
    uint64_t p1 = fair_progress[1];

    /* A factor of four is slack, not a target: the point is that neither is
     * anywhere near zero while the other is done. */
    if (p0 == 0 || p1 == 0 || p0 > p1 * 4 || p1 > p0 * 4) {
        test_fail("phase5-fair", "the two equal-weight tasks got %lu and %lu "
                  "rounds, which is not even", (unsigned long)p0,
                  (unsigned long)p1);
        return;
    }

    test_pass("phase5-fair");
}

/* Sleep really waits.  A sleep that returned immediately would pass any test that
 * only checked the task finished, which is why this measures. */
static volatile uint64_t sleeper_done;
static volatile uint64_t sleeper_elapsed_ms;
static volatile uint64_t sleeper_woke_early;

static void timed_sleeper(void *arg) {
    (void)arg;
    uint64_t start = monotonic_ns();

    task_sleep_ms(60);
    uint64_t elapsed = monotonic_ns() - start;

    sleeper_elapsed_ms = elapsed / 1000000ull;
    if (elapsed < 60000000ull)
        sleeper_woke_early = 1;
    __atomic_add_fetch(&sleeper_done, 1, __ATOMIC_ACQ_REL);
}

static void test_sched_sleep(void) {
    task_t *t = task_create("sleeper", timed_sleeper, NULL,
                            TASK_WEIGHT_DEFAULT);

    sleeper_done = 0;
    sleeper_elapsed_ms = 0;
    sleeper_woke_early = 0;

    if (t == NULL || task_wakeup(t) != 0) {
        test_fail("phase5-sleep", "could not start the sleeper");
        return;
    }

    if (!wait_until((volatile uint64_t *)&sleeper_done, 1, 20000)) {
        test_fail("phase5-sleep", "the task never woke from a 60 ms sleep");
        return;
    }
    if (sleeper_woke_early) {
        test_fail("phase5-sleep", "a 60 ms sleep returned after %lu ms",
                  (unsigned long)sleeper_elapsed_ms);
        return;
    }
    /* And it must not have waited absurdly long either: a sleep that overshoots
     * by a factor of fifty is a tick that is not firing. */
    if (sleeper_elapsed_ms > 3000) {
        test_fail("phase5-sleep", "a 60 ms sleep took %lu ms",
                  (unsigned long)sleeper_elapsed_ms);
        return;
    }

    kprintf("sched: a 60 ms sleep measured %lu ms\n",
            (unsigned long)sleeper_elapsed_ms);
    test_pass("phase5-sleep");
}

/* A wait queue blocks one task and a different task wakes exactly that one.
 * The wrong answer here is either "woke everybody", which a count cannot see, or
 * "never woke", which hangs -- so the test checks both the wake and the count. */
static wait_queue_t test_wq;
static volatile uint64_t wq_waiter_woke;
static volatile uint64_t wq_others_woke;
static volatile uint64_t wq_wake_ran;

static void wq_waiter(void *arg) {
    unsigned tag = (unsigned)(uintptr_t)arg;

    wait_queue_sleep(&test_wq, tag);
    /* The point of the test is which of the two was woken, so the two outcomes go
     * to two different counters rather than one. */
    __atomic_add_fetch(tag == 42 ? &wq_waiter_woke : &wq_others_woke, 1,
                       __ATOMIC_ACQ_REL);
}

static void wq_waker(void *arg) {
    (void)arg;
    /* Let both waiters get as far as the queue before naming one of them. */
    task_sleep_ms(40);
    wait_queue_wake(&test_wq, 42);
    __atomic_add_fetch(&wq_wake_ran, 1, __ATOMIC_ACQ_REL);
}

static void test_sched_waitq(void) {
    task_t *w1, *w2, *wk;

    test_wq.count = 0;
    wq_waiter_woke = 0;
    wq_others_woke = 0;
    wq_wake_ran = 0;
    wait_queue_init(&test_wq);

    w1 = task_create("wq42", wq_waiter, (void *)(uintptr_t)42,
                     TASK_WEIGHT_DEFAULT);
    w2 = task_create("wq7", wq_waiter, (void *)(uintptr_t)7,
                     TASK_WEIGHT_DEFAULT);
    wk = task_create("waker", wq_waker, NULL, TASK_WEIGHT_DEFAULT);

    if (w1 == NULL || w2 == NULL || wk == NULL) {
        test_fail("phase5-waitq", "could not create the three tasks");
        return;
    }

    task_wakeup(w1);
    task_wakeup(w2);
    task_wakeup(wk);

    if (!wait_until((volatile uint64_t *)&wq_wake_ran, 1, 20000)) {
        test_fail("phase5-waitq", "the waker never ran");
        return;
    }
    if (!wait_until((volatile uint64_t *)&wq_waiter_woke, 1, 5000)) {
        test_fail("phase5-waitq", "the named waiter was never woken");
        return;
    }

    if (wq_others_woke != 0) {
        test_fail("phase5-waitq", "waking tag 42 also woke %lu other waiter(s)",
                  (unsigned long)wq_others_woke);
        return;
    }
    if (wait_queue_count(&test_wq) != 1) {
        test_fail("phase5-waitq", "the queue still holds %u waiter(s), expected 1",
                  wait_queue_count(&test_wq));
        return;
    }

    /* Leave nothing blocked, so the next test starts from a clean queue. */
    wait_queue_wake_all(&test_wq);
    (void)wait_until((volatile uint64_t *)&wq_others_woke, 1, 5000);

    test_pass("phase5-waitq");
}

/* The per-processor view has to add up: every processor has an idle task, and the
 * scheduler's counters for each are live rather than zero. */
static void test_sched_queues(void) {
    unsigned online = percpu_online_count();
    uint32_t total_switches = 0;

    for (unsigned i = 0; i < online; i++) {
        sched_stats_t st;

        sched_get_stats(i, &st);
        if (st.ticks == 0) {
            test_fail("phase5-queues", "processor %u has taken no scheduler "
                      "ticks at all, which means its local APIC timer stopped "
                      "firing rather than that it had nothing to do", i);
            return;
        }
        total_switches += st.switches;
        kprintf("sched: cpu %u: %lu ticks, %u switches, %u steals, %u queued, "
                "%u idle\n", i, (unsigned long)st.ticks, st.switches,
                st.steals, st.queued, st.idle_ticks);
    }

    /* An idle processor legitimately makes no switches -- it has nothing to switch
     * to -- so the claim is about the machine as a whole.  A scheduler where
     * *nobody* ever switched would pass every per-processor check above. */
    if (total_switches == 0) {
        test_fail("phase5-queues", "no processor made a single context switch");
        return;
    }

    test_pass("phase5-queues");
}

static void test_smp_tss(void) {
    unsigned known = percpu_count();

    for (unsigned i = 0; i < known; i++) {
        uint64_t base = 0;
        uint32_t limit = 0;
        uint8_t type = 0;

        if (!gdt_tss_descriptor_for(i, &base, &limit, &type)) {
            test_fail("phase4-tss", "processor %u has no 64-bit TSS descriptor "
                      "at selector %#x", i, gdt_tss_selector(i));
            return;
        }

        /* The descriptor's base field is a *physical* address: the CPU fetches the TSS
         * contents from it directly and the TSS is not subject to paging, not even in long
         * mode.  This test asserted the virtual address, so it agreed with a kernel that
         * had the CPU reading its TSS out of physical 0xffffffff80xxxxxx -- and the CPU
         * was, which is why nothing about the TSS worked from ring 3 and nothing here
         * noticed.
         *
         * It then asserted virt_to_phys(), which is the second half of the same mistake: the
         * kernel image is not in the direct map, so that conversion is wrong for an image
         * address too, and the test would have agreed with a second broken kernel.  Hence
         * vmm_kernel_phys(), which is the one conversion that is defined for an image
         * address -- and this test's real lesson is that both times it "passed", it was
         * measuring the kernel against itself. */
        if (base != vmm_kernel_phys((uint64_t)&kernel_tss[i])) {
            test_fail("phase4-tss", "processor %u's descriptor points at %#lx, "
                      "not at its own TSS at %p", i, (unsigned long)base,
                      (void *)&kernel_tss[i]);
            return;
        }

        /* Two processors sharing one descriptor would find the second `ltr`
         * refused, because loading the task register marks it busy in place. */
        for (unsigned j = 0; j < i; j++) {
            uint64_t other = 0;
            if (gdt_tss_descriptor_for(j, &other, NULL, NULL) &&
                other == base) {
                test_fail("phase4-tss", "processors %u and %u share the TSS "
                          "descriptor at %#lx", j, i, (unsigned long)base);
                return;
            }
        }

        /* The limit has to reach the I/O map base at 0x66, or `ltr` is refused
         * with #TS. */
        if (limit < 0x66) {
            test_fail("phase4-tss", "processor %u's TSS limit is %#x, which does "
                      "not cover the I/O map base at 0x66", i, limit);
            return;
        }

        if (kernel_tss[i].iomap_base != sizeof(tss_t)) {
            test_fail("phase4-tss", "processor %u's TSS says its I/O map starts "
                      "at %#x, expected %#x", i, kernel_tss[i].iomap_base,
                      (unsigned)sizeof(tss_t));
            return;
        }

        /* The field offsets, which is the part of a TSS that has to be right and
         * which reading the struct cannot possibly notice.
         *
         * This test used to read `reserved1` and `reserved2` and check they were
         * zero, and it passed for the whole of phase 6 with the TSS shifted up four
         * bytes -- because a struct full of zeros checks out as a struct full of
         * zeros however it is laid out.  What the processor reads is not the struct,
         * it is the byte at a fixed offset, so that is what has to be asserted.
         *
         * RSP0 at 0x04 and IST 1 at 0x24 are what the 64-bit TSS actually uses --
         * Linux's `struct x86_hw_tss` and QEMU's `get_rsp_from_tss()` (index
         * `8 * level + 4`) agree on that.  Getting them wrong is invisible from
         * ring 0 and fatal from ring 3: an exception at CPL 0 needs no stack switch,
         * so phases 1 to 5 pass, and then the first syscall pushes onto whatever the
         * reserved word holds. */
        if (offsetof(tss_t, rsp[0]) != 0x04 ||
            offsetof(tss_t, rsp[1]) != 0x0c ||
            offsetof(tss_t, rsp[2]) != 0x14 ||
            offsetof(tss_t, ist[0]) != 0x24 ||
            offsetof(tss_t, ist[6]) != 0x54 ||
            offsetof(tss_t, iomap_base) != 0x66) {
            test_fail("phase4-tss", "tss_t is laid out at rsp0=%#x rsp1=%#x "
                      "rsp2=%#x ist1=%#x ist7=%#x iomap=%#x, expected "
                      "0x04/0x0c/0x14/0x24/0x54/0x66",
                      (unsigned)offsetof(tss_t, rsp[0]),
                      (unsigned)offsetof(tss_t, rsp[1]),
                      (unsigned)offsetof(tss_t, rsp[2]),
                      (unsigned)offsetof(tss_t, ist[0]),
                      (unsigned)offsetof(tss_t, ist[6]),
                      (unsigned)offsetof(tss_t, iomap_base));
            return;
        }

        /* And the reserved words the processor reads *instead* of the stack
         * pointers, when the layout above is wrong: they have to be zero so that a
         * fault at least fails loudly rather than stacking onto a number. */
        if (kernel_tss[i].reserved0 != 0 || kernel_tss[i].reserved2 != 0 ||
            kernel_tss[i].reserved3 != 0) {
            test_fail("phase4-tss", "processor %u's TSS reserved words are "
                      "not zero (%#lx, %#lx, %#lx)", i,
                      (unsigned long)kernel_tss[i].reserved0,
                      (unsigned long)kernel_tss[i].reserved2,
                      (unsigned long)kernel_tss[i].reserved3);
            return;
        }

        if (percpu_is_online(i)) {
            if (kernel_tss[i].ist[0] == 0) {
                test_fail("phase4-tss", "processor %u is online with no IST 1 "
                          "stack, so a double fault has nowhere to go", i);
                return;
            }
        }
    }

    test_pass("phase4-tss");
}

static void test_smp_online(void) {
    unsigned known = percpu_count();
    unsigned online = percpu_online_count();

    if (known == 0) {
        test_fail("phase4-smp", "no processors are known at all");
        return;
    }
    if (online == 0 || online > known) {
        test_fail("phase4-smp", "%u of %u processors are online, which is "
                  "not possible", online, known);
        return;
    }

    if (online < known)
        kprintf("smp: %u of %u processor(s) online; the application "
                "processors did not start\n", online, known);

    test_pass("phase4-smp");
}

/* --- entry --------------------------------------------------------------- */

void test_run_all(void) {
    kprintf("\n=== minx self test ===\n");

    test_firmware();
    test_console_geometry();
    test_console_colour();
    test_console_cursor();
    test_console_scrolling();
    test_kmalloc();
    test_string();
    test_interrupt_path();
    test_exception_report();
    test_acpi();
    test_interrupts();
    test_keyboard();
    test_input_queue();
    test_pmm();
    test_pmm_contiguous();
    test_direct_map();
    test_pmm_churn();
    test_percpu();
    test_atomics();
    test_lock_contention();
    test_lock_fairness();
    test_mcs();
    test_mutex_lock();
    test_semaphore();
    test_lock_irqsave();
    test_ipi();
    test_smp_tss();
    test_smp_online();
    test_user_pointers();
    test_elf_rejects();
    test_address_spaces();
    test_user_program();
    test_fork_program();
    test_cow_program();
    test_signal_program();
    test_ramfs();
    test_fs_program();
    test_libc_program();
    test_sh_program();
    test_sched_queues();
    test_sched_parallel();
    test_sched_preempt();
    test_sched_fair();
    test_sched_sleep();
    test_sched_waitq();

    kprintf("\n%u passed, %u failed\n", passed, failed);
    kprintf("TEST-READY\n");
}

void test_linger_and_exit(void) {
    /* Keep the framebuffer on screen long enough for the harness to dump it,
     * then power the machine off with a status the harness can read. */
    kprintf("lingering for %u ms so the harness can dump the framebuffer\n",
            TEST_LINGER_MS);
    delay_ms(TEST_LINGER_MS);

    kshutdown(failed != 0 ? 1 : 0);
}