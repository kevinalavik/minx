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
#include "panic.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
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
    unsigned self = percpu_self_index();
    percpu_t *me = percpu_self();

    if (self >= percpu_count()) {
        test_fail("phase4-percpu", "%%gs resolves to processor %u, but only "
                  "%u are known", self, percpu_count());
        return;
    }

    if (me != percpu_by_index(self)) {
        test_fail("phase4-percpu", "processor %u's %%gs slot holds %p, but "
                  "slot %u is %p", self, (void *)me, self,
                  (void *)percpu_by_index(self));
        return;
    }

    /* %gs:0 has to be the slot's own state pointer, which is what makes the
     * accessor one instruction instead of an indexed load. */
    percpu_t *from_gs;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(from_gs));
    if (from_gs != me) {
        test_fail("phase4-percpu", "%%gs:0 is %p, not this processor's own "
                  "state at %p", (void *)from_gs, (void *)me);
        return;
    }

    /* Every known processor has a distinct slot, and no two of them claim the
     * same LAPIC id -- ACPI listed them, so a duplicate would mean two
     * processors are being started from one entry. */
    for (unsigned i = 0; i < percpu_count(); i++) {
        if (percpu_by_index(i) == NULL) {
            test_fail("phase4-percpu", "processor %u has no slot", i);
            return;
        }
        for (unsigned j = i + 1; j < percpu_count(); j++) {
            if (percpu_lapic_id(i) == percpu_lapic_id(j)) {
                test_fail("phase4-percpu", "processors %u and %u share LAPIC "
                          "id %u", i, j, percpu_lapic_id(i));
                return;
            }
        }
    }

    if (percpu_by_index(self)->lapic_id != lapic_id()) {
        test_fail("phase4-percpu", "slot %u says LAPIC id %u, the hardware "
                  "says %u", self, percpu_by_index(self)->lapic_id,
                  lapic_id());
        return;
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

        if (base != (uint64_t)&kernel_tss[i]) {
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

        /* The limit has to reach the I/O map base at 0x6a, or `ltr` is refused
         * with #TS. */
        if (limit < 0x6a) {
            test_fail("phase4-tss", "processor %u's TSS limit is %#x, which does "
                      "not cover the I/O map base at 0x6a", i, limit);
            return;
        }

        if (kernel_tss[i].iomap_base != sizeof(tss_t)) {
            test_fail("phase4-tss", "processor %u's TSS says its I/O map starts "
                      "at %#x, expected %#x", i, kernel_tss[i].iomap_base,
                      (unsigned)sizeof(tss_t));
            return;
        }

        /* Two more field offsets the CPU reads and nothing checks.  RSP0 lives
         * at 0x08, not 0x04 -- the 32-bit TSS's SS:0 slot is there -- and IST 1
         * at 0x28, not 0x24.  Both wrong just means a fault finds no stack. */
        if (kernel_tss[i].reserved1 != 0 || kernel_tss[i].reserved2 != 0) {
            test_fail("phase4-tss", "processor %u's TSS reserved words are "
                      "not zero (%#lx, %#lx)", i,
                      (unsigned long)kernel_tss[i].reserved1,
                      (unsigned long)kernel_tss[i].reserved2);
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
    test_smp_tss();
    test_smp_online();

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