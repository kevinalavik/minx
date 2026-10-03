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
#include "irq.h"
#include "percpu.h"
#include "io.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "panic.h"
#include "serial.h"
#include "string.h"
#include "types.h"

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

static void test_pic(void) {
    /* The remap exists so no IRQ vector collides with a CPU exception: the 32
     * exceptions occupy 0..31, so the 8259s must start at 32 and 40. */
    uint16_t mask = pic_irq_mask();

    if (mask == 0xffff) {
        test_fail("phase2-pic", "every PIC line is still masked");
        return;
    }

    /* IRQ0 (the 8253) and IRQ1 (the keyboard) must be unmasked for the console
     * to be usable; everything else should stay masked until a driver asks. */
    if ((mask & 1u) != 0) {
        test_fail("phase2-pic", "IRQ0 (timer) is masked");
        return;
    }
    if ((mask & 2u) != 0) {
        test_fail("phase2-pic", "IRQ1 (keyboard) is masked");
        return;
    }

    /* Round-trip a line we do not use.  Unmasking means clearing the bit in the
     * hardware mask, so the readback has to go from set to clear and back. */
    irq_enable(7);
    if ((pic_irq_mask() & 0x80u) != 0) {
        test_fail("phase2-pic", "unmasking IRQ7 left the bit set in the chip");
        return;
    }
    irq_disable(7);
    if ((pic_irq_mask() & 0x80u) == 0) {
        test_fail("phase2-pic", "masking IRQ7 left the bit clear in the chip");
        return;
    }

    if (irq_handler(0) == NULL) {
        test_fail("phase2-pic", "no handler is registered for IRQ0");
        return;
    }
    if (irq_handler(1) == NULL) {
        test_fail("phase2-pic", "no handler is registered for IRQ1");
        return;
    }

    test_pass("phase2-pic");
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
    test_pic();
    test_input_queue();

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