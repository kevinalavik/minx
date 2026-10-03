/* minx - the interactive console loop.
 *
 * Until the TTY line discipline exists (phase 9) the boot CPU drains the input
 * queue itself and echoes what arrives, which is what makes "type into the
 * graphical console" true from phase 2 onwards.  Later this moves behind the
 * TTY without changing any of the code below.
 */
#include "conio.h"

#include "console.h"
#include "cpu.h"
#include "input.h"
#include "io.h"
#include "irq.h"
#include "keyboard.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "percpu.h"
#include "power.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "types.h"

/* The console talks about the PIT-based uptime rather than the TSC, because the
 * PIT is the time base that is known to be right. */
static inline uint64_t console_uptime_ms(void) { return timer_uptime_ms(); }
static inline unsigned cpu_online_count(void) { return percpu_online_count(); }
static inline uint32_t cpu_lapic_id(unsigned i) { return percpu_lapic_id(i); }
static inline uint64_t cpu_interrupt_count(unsigned i) { return percpu_interrupts(i); }

/* A short line of typed input, so the echo can be edited before it is acted on. */
#define CONIO_LINE_MAX 256

static char line[CONIO_LINE_MAX];
static size_t line_len;

/* Escape sequences written when a named key is pressed. */
static void show_key(uint32_t key) {
    switch (key) {
    case KEY_UP:      console_puts("\033[A"); break;
    case KEY_DOWN:    console_puts("\033[B"); break;
    case KEY_RIGHT:   console_puts("\033[C"); break;
    case KEY_LEFT:    console_puts("\033[D"); break;
    case KEY_HOME:    console_puts("\033[H"); break;
    case KEY_END:     console_puts("\033[F"); break;
    case KEY_PAGEUP:  console_puts("\033[5~"); break;
    case KEY_PAGEDOWN: console_puts("\033[6~"); break;
    case KEY_DELETE:  console_puts("\033[3~"); break;
    case KEY_INSERT:  console_puts("\033[2~"); break;
    case KEY_F1:      console_puts("\033OP"); break;
    case KEY_F2:      console_puts("\033OQ"); break;
    case KEY_F3:      console_puts("\033[OR"); break;
    case KEY_F4:      console_puts("\033[OS"); break;
    case KEY_F5:      console_puts("\033[15~"); break;
    case KEY_F6:      console_puts("\033[17~"); break;
    case KEY_F7:      console_puts("\033[18~"); break;
    case KEY_F8:      console_puts("\033[19~"); break;
    case KEY_F9:      console_puts("\033[20~"); break;
    case KEY_F10:     console_puts("\033[21~"); break;
    case KEY_F11:     console_puts("\033[23~"); break;
    case KEY_F12:     console_puts("\033[24~"); break;
    default:
        console_puts("\033[37m");
        kprintf("[key %x]", key);
        console_puts("\033[0m");
        break;
    }
}

static void line_backspace(void) {
    if (line_len == 0)
        return;
    line_len--;
    console_puts("\b \b");
}

static void line_submit(void) {
    console_puts("\n");

    /* Commands the console understands on its own; the shell takes over in
     * phase 9, and this file then shrinks to nothing but the read loop. */
    if (strcmp(line, "help") == 0) {
        console_puts("console commands: help, clear, uptime, meminfo, "
                     "cpus, keys, irqs, reboot\n");
    } else if (strcmp(line, "clear") == 0) {
        console_clear();
    } else if (strcmp(line, "uptime") == 0) {
        kprintf("up %lu ms (%lu PIT ticks, %lu ms of TSC)\n",
                (unsigned long)console_uptime_ms(),
                (unsigned long)pit_uptime_ticks(),
                (unsigned long)timer_tsc_since_pit_ms());
    } else if (strcmp(line, "meminfo") == 0) {
        kprintf("heap: %lu KiB total, %lu KiB free, %lu KiB used, "
                "largest free %lu bytes\n",
                (unsigned long)(kmalloc_total_bytes() / 1024),
                (unsigned long)(kmalloc_free_bytes() / 1024),
                (unsigned long)(kmalloc_used_bytes() / 1024),
                (unsigned long)kmalloc_largest_free());
    } else if (strcmp(line, "cpus") == 0) {
        for (unsigned i = 0; i < cpu_online_count(); i++)
            kprintf("cpu %u: online, LAPIC id %u, %llu interrupts\n", i,
                    cpu_lapic_id(i), (unsigned long long)cpu_interrupt_count(i));
    } else if (strcmp(line, "keys") == 0) {
        kprintf("keyboard: present=%d ready=%d, %s, %llu scancodes, "
                "%llu events, %lu overruns (caps %d, num %d)\n",
                keyboard_present(), keyboard_ready(),
                keyboard_uses_8042_translation() ? "set 2 via 8042 translation"
                                                 : "set 1",
                (unsigned long long)keyboard_scancodes(),
                (unsigned long long)keyboard_events(),
                (unsigned long)keyboard_overruns(),
                keyboard_capslock(), keyboard_numlock());
    } else if (strcmp(line, "irqs") == 0) {
        for (unsigned i = 0; i < IRQ_MAX; i++)
            kprintf("irq %2u: %-14s %s %lu\n", i, irq_name(i),
                    irq_enabled(i) ? "enabled " : "masked  ",
                    (unsigned long)irq_count(i));
    } else if (strcmp(line, "uptime-pit") == 0) {
        kprintf("pit %lu ticks at %u Hz\n", (unsigned long)pit_uptime_ticks(),
                pit_irq_hz());
    } else if (strcmp(line, "reboot") == 0) {
        reboot();
    } else if (line_len > 0) {
        kprintf("minx: '%s' is not a command (try help)\n", line);
    }

    line_len = 0;
    console_puts("\n");
}

/* Handle one key event.  Returns false when the loop should stop. */
bool conio_handle_event(const input_event_t *ev) {
    if (ev->type != INPUT_KEY_DOWN)
        return true;

    switch (ev->key) {
    case KEY_ENTER:
        line_submit();
        return true;
    case KEY_BACKSPACE:
    case 0x7f:   /* ASCII delete, which is what many keyboards send for ^? */
    case 0x08:
        line_backspace();
        return true;
    case KEY_TAB:
        console_puts("\t");
        return true;
    case KEY_ESCAPE:
        console_puts("\033");
        return true;
    case KEY_CAPSLOCK:
    case KEY_NUMLOCK:
        return true;   /* already tracked by the driver */
    case KEY_UP:
    case KEY_DOWN:
    case KEY_LEFT:
    case KEY_RIGHT:
    case KEY_HOME:
    case KEY_END:
    case KEY_PAGEUP:
    case KEY_PAGEDOWN:
    case KEY_DELETE:
    case KEY_INSERT:
    case KEY_F1: case KEY_F2: case KEY_F3: case KEY_F4:
    case KEY_F5: case KEY_F6: case KEY_F7: case KEY_F8:
    case KEY_F9: case KEY_F10: case KEY_F11: case KEY_F12:
        show_key(ev->key);
        return true;
    default:
        break;
    }

    if (ev->key == KEY_NONE)
        return true;

    /* Control characters act immediately rather than entering the buffer. */
    if (ev->key < 0x20 && ev->key != '\t') {
        switch (ev->key) {
        case 0x03: kprintf("^C\n"); return true;                 /* SIGINT */
        case 0x04: console_clear(); return true;                /* ^D */
        case 0x0c: kprintf("^L\n"); console_clear(); return true;
        case 0x0e: kprintf("^N: back to the boot loop\n"); return false;
        case 0x10: kprintf("^P\n"); return true;
        case 0x12: kprintf("^R\n"); return true;
        default:   kprintf("^%c\n", (char)('A' + ev->key - 1)); return true;
        }
    }

    if (line_len + 1 < sizeof(line))
        line[line_len++] = (char)ev->key;

    char c = (char)ev->key;
    console_putc(c);
    if (c == '\n' || c == '\r')
        line_submit();
    return true;
}

/* Drain whatever is queued and act on it.  Returns the number of events
 * handled. */
unsigned conio_pump(void) {
    unsigned handled = 0;
    input_event_t ev;

    while (input_pop(&ev)) {
        if (!conio_handle_event(&ev))
            break;
        handled++;
    }
    return handled;
}

void conio_banner(void) {
    line_len = 0;
    console_puts("\n");
    kprintf("minx console commands: help clear uptime meminfo cpus keys irqs "
            "reboot\n");
    kprintf("type help and press enter.\n");
}