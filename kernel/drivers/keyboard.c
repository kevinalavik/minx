#include "keyboard.h"

#include "cpu.h"
#include "delay.h"
#include "input.h"
#include "io.h"
#include "irq.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "types.h"

/* PS/2 keyboard on IRQ1, scancode set 1.
 *
 * Set 1 is the IBM PC/AT encoding: every PS/2 keyboard has produced it since
 * 1981, the tables below are the ones the hardware actually emits, and the
 * extended (E0-prefixed) codes carry the navigation and function keys.  The
 * driver asks the keyboard to use set 1 rather than assuming it, so a keyboard
 * that offers only set 2 produces a clear complaint instead of silently
 * decoding every other key.
 */

/* 8042 controller ports. */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

/* Status register (port 0x64) bits.  Getting these wrong is subtle: bit 1 means
 * the controller's input buffer is *full*, so a write must wait for it to clear,
 * and bit 0 means the output buffer *has* data, so a read must wait for it to
 * be set. */
#define PS2_STATUS_OUTPUT_FULL   0x01   /* bit 0: device data ready to read   */
#define PS2_STATUS_INPUT_FULL    0x02   /* bit 1: input buffer full           */
#define PS2_STATUS_SYSTEM_FLAG   0x04   /* bit 2: passed the self test       */
#define PS2_STATUS_INPUT_CLOCK   0x02   /* writes are blocked while this is set */
#define PS2_STATUS_OVERRUN       0x20   /* bit 5: reading the config byte clears an overrun */

/* Commands sent to the controller. */
#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_DISABLE_PORT2 0xa7
#define PS2_CMD_ENABLE_PORT2  0xa8
#define PS2_CMD_SELF_TEST     0xaa
#define PS2_CMD_TEST_PORT1    0xab
#define PS2_CMD_DISABLE_PORT1 0xad
#define PS2_CMD_ENABLE_PORT1  0xae
#define PS2_CMD_WRITE_PORT2   0xd4
#define PS2_CMD_SCAN_CODE_SET 0xf0

/* Configuration byte bits. */
#define CFG_PORT1_IRQ    (1u << 0)
#define CFG_PORT2_IRQ    (1u << 1)
#define CFG_PORT1_CLOCK  (1u << 4)
#define CFG_PORT2_CLOCK  (1u << 5)
#define CFG_TRANSLATION  (1u << 6)

/* Commands sent to the keyboard. */
#define KBD_CMD_RESET          0xff
#define KBD_CMD_SCAN_CODE_SET  0xf0
#define KBD_CMD_ENABLE_SCAN    0xf4
#define KBD_CMD_DISABLE_SCAN   0xf5
#define KBD_CCB_ENABLE_SCAN    0x20
#define KBD_CCB_WRITE_DEFAULTS 0x20

/* Replies.  Note that the *controller* and the *keyboard* use different success
 * codes for their self tests: 0x55 from the 8042, 0xAA from the keyboard after
 * a reset. */
#define KBD_ACK         0xfa
#define KBD_SELFTEST_OK 0x55   /* 8042 controller self test */
#define KBD_ECHO        0x0e
#define KBD_KEYSTUCK    0xfe

#define PS2_KEYBOARD_IRQ 1

#define SCAN_EXTENDED  0xe0   /* prefix for the second half of a key */
#define SCAN_E1        0xe1   /* prefix for the pause key */

/* Unshifted US layout, indexed by set-1 make code. */
static const char keymap_lower[128] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*',  ' ', 0,   ' ', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6',
    '+', '1', '2', '3', '0', '.',
};

static const char keymap_upper[128] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*',  ' ', 0,   ' ', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6',
    '+', '1', '2', '3', '0', '.',
};

/* Set-1 make codes for keys that have no printable character. */
static uint32_t special_key(uint8_t code, bool extended) {
    if (extended) {
        switch (code) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4b: return KEY_LEFT;
        case 0x4d: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4f: return KEY_END;
        case 0x49: return KEY_PAGEUP;
        case 0x51: return KEY_PAGEDOWN;
        case 0x53: return KEY_DELETE;
        case 0x52: return KEY_INSERT;
        /* Function keys arrive as E0-prefixed keys with their own codes, which
         * are the set-1 F1..F10 codes followed by the two that use 0x57/0x58. */
        case 0x3b: return KEY_F1;
        case 0x3c: return KEY_F2;
        case 0x3d: return KEY_F3;
        case 0x3e: return KEY_F4;
        case 0x3f: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        case 0x1d: return KEY_RIGHTCTRL;
        case 0x5c: return KEY_RIGHTMETA;
        case 0x35: return KEY_DIVIDE;      /* keypad slash */
        case 0x1c: return KEY_ENTER;       /* keypad enter */
        default:  return KEY_NONE;
        }
    }

    switch (code) {
    case 0x01: return KEY_ESCAPE;
    case 0x0e: return KEY_BACKSPACE;
    case 0x0f: return KEY_TAB;
    case 0x1c: return KEY_ENTER;
    case 0x39: return ' ';
    case 0x1d: return KEY_LEFTCTRL;
    case 0x2a: return KEY_LEFTSHIFT;
    case 0x36: return KEY_RIGHTSHIFT;
    case 0x38: return KEY_LEFTALT;
    case 0x5b: return KEY_LEFTMETA;
    case 0x3a: return KEY_CAPSLOCK;
    case 0x45: return KEY_NUMLOCK;
    case 0x46: return KEY_SCROLLLOCK;
    case 0x3b: return KEY_F1;
    case 0x3c: return KEY_F2;
    case 0x3d: return KEY_F3;
    case 0x3e: return KEY_F4;
    case 0x3f: return KEY_F5;
    case 0x40: return KEY_F6;
    case 0x41: return KEY_F7;
    case 0x42: return KEY_F8;
    case 0x43: return KEY_F9;
    case 0x44: return KEY_F10;
    case 0x57: return KEY_F11;
    case 0x58: return KEY_F12;
    default: return KEY_NONE;
    }
}

static struct {
    bool     present;
    bool     ready;
    bool     extended_prefix;   /* the next byte belongs to an E0 sequence */
    bool     caps_on;
    bool     num_on;
    uint32_t modifiers;
    uint8_t  config;
    uint64_t scancodes;
    uint64_t events;
    uint32_t overruns;
} kbd;

bool keyboard_present(void) { return kbd.present; }
bool keyboard_ready(void) { return kbd.ready; }
uint64_t keyboard_scancodes(void) { return kbd.scancodes; }
uint64_t keyboard_events(void) { return kbd.events; }
uint8_t keyboard_saved_byte(void) { return kbd.config; }
bool keyboard_numlock(void) { return kbd.num_on; }
bool keyboard_capslock(void) { return kbd.caps_on; }
uint32_t keyboard_overruns(void) { return kbd.overruns; }

static inline bool status_has(uint8_t bit) {
    return (inb(PS2_STATUS) & bit) != 0;
}

static bool wait_write(void) {
    for (unsigned i = 0; i < 100000u; i++) {
        if (!status_has(PS2_STATUS_INPUT_CLOCK))
            return true;
        io_wait();
    }
    return false;
}

static bool wait_read(void) {
    for (unsigned i = 0; i < 100000u; i++) {
        if (status_has(PS2_STATUS_OUTPUT_FULL))
            return true;
        io_wait();
    }
    return false;
}

bool ps2_write_command(uint8_t cmd) {
    if (!wait_write())
        return false;
    outb(PS2_COMMAND, cmd);
    return true;
}

bool ps2_write_data(uint8_t value) {
    if (!wait_write())
        return false;
    outb(PS2_DATA, value);
    return true;
}

uint8_t ps2_read_data(void) {
    if (!wait_read())
        return 0xff;
    return inb(PS2_DATA);
}

/* Drain anything the controller has queued, so a stale byte is not mistaken for
 * the answer to the next command. */
static void ps2_flush(void) {
    for (unsigned i = 0; i < 64; i++) {
        if (!status_has(PS2_STATUS_OUTPUT_FULL))
            return;
        (void)inb(PS2_DATA);
        io_wait();
    }
}

/* Send a command to the keyboard and check for its acknowledge. */
static bool kbd_command(uint8_t cmd) {
    if (!ps2_write_data(cmd))
        return false;
    if (!wait_read())
        return false;

    uint8_t reply = inb(PS2_DATA);
    if (reply == KBD_ACK || reply == KBD_ECHO)
        return true;
    kprintf("[warn] keyboard: command %#x replied with %#x\n", cmd, reply);
    return false;
}

static void emit(uint32_t key, input_type_t type) {
    input_event_t ev;
    ev.type = type;
    ev.key = key;
    ev.modifiers = kbd.modifiers;
    ev.x = ev.y = 0;
    ev.buttons = 0;
    input_push(&ev);
    kbd.events++;
}

static uint32_t apply_modifier(uint8_t code, bool make) {
    uint32_t bit;
    switch (code) {
    case 0x2a:
    case 0x36: bit = MOD_SHIFT; break;
    case 0x1d: bit = MOD_CTRL; break;
    case 0x38: bit = MOD_ALT; break;
    case 0x5b:
    case 0x5c: bit = MOD_META; break;
    case 0x3a: bit = MOD_CAPS; break;
    case 0x45: bit = MOD_NUM; break;
    default: return 0;
    }

    if (make)
        kbd.modifiers |= bit;
    else
        kbd.modifiers &= ~bit;
    return bit;
}

/* Turn one make code into a key, honouring shift, caps lock and control. */
static uint32_t translate(uint8_t code, bool extended) {
    uint32_t special = special_key(code, extended);
    if (special != KEY_NONE)
        return special;

    /* The keypad digits are separate make codes but share the keymap with the
     * top row. */
    static const uint8_t keypad_as_top_row[10] = {
        0, 0x0c, 0x0c, 0x0b, 0x0b, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c
    };
    uint8_t index = code;
    if (code >= 0x47 && code <= 0x53)
        index = keypad_as_top_row[code - 0x47];

    if (index >= 128)
        return KEY_NONE;

    bool shifted = (kbd.modifiers & MOD_SHIFT) != 0;
    bool caps = kbd.caps_on;
    bool upper = (index >= 'a' && index <= 'z') ? (shifted != caps) : shifted;

    char c = upper ? keymap_upper[index] : keymap_lower[index];
    if (c == 0)
        return KEY_NONE;

    /* Control turns letters into the ASCII control codes the line discipline
     * expects, which is what ^C and friends are. */
    if ((kbd.modifiers & MOD_CTRL) && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 1);

    return (uint32_t)(unsigned char)c;
}

static void keyboard_irq(struct regs *regs) {
    (void)regs;

    while (status_has(PS2_STATUS_OUTPUT_FULL)) {
        uint8_t code = inb(PS2_DATA);
        kbd.scancodes++;

        if (code == SCAN_EXTENDED) {
            kbd.extended_prefix = true;
            continue;
        }
        bool extended = kbd.extended_prefix;
        kbd.extended_prefix = false;

        /* Pause sends E1 1D 45 E1 9D C5; skip the whole sequence. */
        if (code == SCAN_E1) {
            for (unsigned i = 0; i < 8 && wait_read(); i++) {
                uint8_t next = inb(PS2_DATA);
                kbd.scancodes++;
                if (next == 0x9d)
                    break;
            }
            continue;
        }

        bool make = (code & 0x80) == 0;
        uint8_t base = code & 0x7f;

        /* The keypad's "enter" and "/" arrive E0-prefixed but are not
         * navigation keys, so they must not be swallowed by extended handling. */
        uint32_t modifier = apply_modifier(base, make);
        if (modifier == 0 && base == 0x1c && extended)
            modifier = 0;   /* keypad enter: fall through to translate() */

        if (base == 0x3a && make)
            kbd.caps_on = !kbd.caps_on;
        else if (base == 0x45 && make)
            kbd.num_on = !kbd.num_on;

        uint32_t key = translate(base, extended);
        if (key != KEY_NONE)
            emit(key, make ? INPUT_KEY_DOWN : INPUT_KEY_UP);
        else if (modifier != 0 && make)
            emit(special_key(base, extended), INPUT_KEY_DOWN);
    }

    /* An overrun means the controller dropped a byte; reading the configuration
     * byte clears the condition, otherwise it interrupts again immediately. */
    if (status_has(PS2_STATUS_OVERRUN)) {
        kbd.overruns++;
        (void)ps2_write_command(PS2_CMD_READ_CONFIG);
        (void)ps2_read_data();
    }
}

/* How to get set-1 codes out of the keyboard.
 *
 * The 8042 has a translation bit: with it set the controller converts the
 * keyboard's set-2 output into set 1 itself.  Using that is much better than
 * translating in software, because the controller's table is the authoritative
 * one, and because it means the same decoding path works whether the keyboard
 * natively speaks set 1 or only set 2. */
typedef enum {
    SCANMODE_NATIVE_SET1,
    SCANMODE_8042_TRANSLATION,
} scanmode_t;

static scanmode_t scanmode = SCANMODE_NATIVE_SET1;

bool keyboard_uses_8042_translation(void) {
    return scanmode == SCANMODE_8042_TRANSLATION;
}

/* Set the keyboard's scancode set to 1 if it supports it, and report whether it
 * was selected. */
static bool keyboard_select_set1(void) {
    if (!kbd_command(KBD_CMD_SCAN_CODE_SET))
        return false;
    if (!kbd_command(0x00) || !wait_read())
        return false;

    uint8_t sets = inb(PS2_DATA);
    kprintf("keyboard: supported scancode sets: 0x%02x\n", sets);

    if ((sets & 0x01) == 0)
        return false;

    /* Querying the mask leaves the keyboard waiting for another 0xF0, so the
     * command has to be repeated before the set number is accepted. */
    return kbd_command(KBD_CMD_SCAN_CODE_SET) && kbd_command(0x01);
}

int keyboard_init(void) {
    input_init();

    /* Disable both ports and drain the controller before touching anything. */
    (void)ps2_write_command(PS2_CMD_DISABLE_PORT1);
    (void)ps2_write_command(PS2_CMD_DISABLE_PORT2);
    ps2_flush();

    uint8_t self_test = ps2_write_command(PS2_CMD_SELF_TEST)
                            ? ps2_read_data() : 0xff;
    if (self_test != KBD_SELFTEST_OK) {
        kprintf("[warn] keyboard: 8042 self test failed (%#x); trying to "
                "continue anyway\n", self_test);
    } else {
        kprintf("keyboard: 8042 controller self test passed\n");
    }

    ps2_flush();

    /* Read the configuration byte.  Whether the translation bit ends up set is
     * decided below, once we know what the keyboard can produce. */
    kbd.config = 0x45;
    if (ps2_write_command(PS2_CMD_READ_CONFIG)) {
        uint8_t cfg = ps2_read_data();
        if (cfg != 0xff)
            kbd.config = cfg;
    }

    kbd.present = ps2_write_command(PS2_CMD_TEST_PORT1) &&
                  ps2_read_data() == 0x00;
    if (!kbd.present) {
        kprintf("[warn] keyboard: no device answered on port 1\n");
        return -1;
    }

    /* A reset returns the keyboard to its power-on state (set 2, scanning off)
     * and makes it announce the self-test result. */
    if (!kbd_command(KBD_CMD_RESET) || !wait_read())
        return -1;
    (void)inb(PS2_DATA);   /* the keyboard's self-test result, 0xaa when good */

    /* Enable the port before anything else, because a keyboard that is still
     * not streaming will silently swallow everything typed. */
    (void)ps2_write_command(PS2_CMD_ENABLE_PORT1);

    /* Prefer set 1 straight from the keyboard; fall back to letting the 8042 do
     * the translation. */
    scanmode = SCANMODE_8042_TRANSLATION;
    if (keyboard_select_set1())
        scanmode = SCANMODE_NATIVE_SET1;

    uint8_t cfg = kbd.config;
    cfg |= CFG_PORT1_IRQ | CFG_PORT1_CLOCK;
    if (scanmode == SCANMODE_8042_TRANSLATION)
        cfg |= CFG_TRANSLATION;
    else
        cfg &= (uint8_t)~CFG_TRANSLATION;
    if (ps2_write_command(PS2_CMD_WRITE_CONFIG) && ps2_write_data(cfg))
        kbd.config = cfg;

    kprintf("keyboard: %s\n", scanmode == SCANMODE_NATIVE_SET1
                                  ? "using scancode set 1"
                                  : "using scancode set 2 with 8042 translation");

    /* Turn key scanning back on: a reset leaves it off. */
    if (!kbd_command(0xf4)) {
        kprintf("[warn] keyboard: enable-scanning was not acknowledged\n");
        return -1;
    }

    /* Read the keyboard's saved modifier state through the controller's port 2
     * "write to keyboard" channel. */
    if (ps2_write_command(PS2_CMD_WRITE_PORT2) &&
        kbd_command(KBD_CCB_ENABLE_SCAN)) {
        uint8_t state = ps2_read_data();
        if (state != 0xff) {
            kbd.num_on = (state & 0x01) != 0;
            kbd.caps_on = (state & 0x40) != 0;
            kprintf("keyboard: saved state 0x%02x (numlock %d, capslock %d)\n",
                    state, kbd.num_on, kbd.caps_on);
        }
    }

    if (irq_register(PS2_KEYBOARD_IRQ, keyboard_irq, "ps2-keyboard") != 0)
        return -1;
    if (irq_enable(PS2_KEYBOARD_IRQ) != 0)
        return -1;

    kbd.ready = true;
    kprintf("keyboard: ready on IRQ %u, config byte 0x%02x\n", PS2_KEYBOARD_IRQ,
            kbd.config);
    return 0;
}