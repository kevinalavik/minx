/* minx - the terminal line discipline.  See tty.h for why the console and the terminal
 * are two things rather than one. */
#include "tty.h"

#include "console.h"
#include "input.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "signal.h"
#include "string.h"
#include "syscall.h"
#include "task.h"

/* The input queue.  Bounded, because a line discipline that can be made to allocate without
 * limit by a program that reads nothing is a way to take the machine down from ring 3.
 * 4 KiB is several screens of typing; a person who has typed that much without a newline is
 * not typing, and a program that has read nothing at all is not either. */
#define TTY_IN_CAPACITY 4096
#define TTY_LINE_MAX    1024

static char     in_buf[TTY_IN_CAPACITY];
static uint32_t in_head, in_tail, in_count;
static wait_queue_t in_waiters;
static wait_queue_t out_waiters;

/* The defaults every terminal starts from, and the one set of control characters.
 *
 * Per-process settings live in the task, because two programs sharing one screen cannot both
 * own its echo setting and the rule everywhere is that whoever most recently asked owns it.
 * These are the values a process that has never asked gets, so they are the ones that have
 * to be right: the control characters are what make ^C mean SIGINT rather than byte 3. */
static termios_t defaults = {
    /* c_iflag */ ICRNL | IXON,
    /* c_oflag */ OPOST | ONLCR,
    /* c_cflag */ 0,
    /* c_lflag */ ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | IEXTEN,
    /* c_line  */ 0,
    /* c_cc    */ { 0 },
    /* speeds  */ 0, 0,
};

static bool ready;

termios_t *tty_default_termios(void) {
    return &defaults;
}

/* The settings in force for the calling task.
 *
 * Read from the task, written to the task, and defaulted for anything that has none.  A
 * process that has never called `tcsetattr` gets the defaults, which is what makes the very
 * first `read` in a program behave the way its author expects. */
static termios_t *termios_of(task_t *t) {
    if (t == NULL)
        return &defaults;

    if (!t->tty_configured) {
        t->tty_termios = defaults;
        t->tty_configured = true;
    }

    return &t->tty_termios;
}

/* --- the input queue ------------------------------------------------------ */

static void push(char c) {
    if (in_count >= TTY_IN_CAPACITY) {
        /* Full.  Dropping the *oldest* character rather than the newest is the right way
         * round: a person who has typed past the buffer has typed more than fits, and the
         * character they typed first is the one least likely to still be wanted.  Dropping
         * the newest would silently eat whatever they typed last, which is the character
         * they are looking at. */
        in_tail = (in_tail + 1) % TTY_IN_CAPACITY;
        in_count--;
    }

    in_buf[in_head] = c;
    in_head = (in_head + 1) % TTY_IN_CAPACITY;
    in_count++;
}

static bool pop(char *out) {
    if (in_count == 0)
        return false;

    *out = in_buf[in_tail];
    in_tail = (in_tail + 1) % TTY_IN_CAPACITY;
    in_count--;
    return true;
}

/* --- the discipline ------------------------------------------------------- */

/* Control character handling, in canonical mode.
 *
 * The four that change the line and one that ends the program, and all of them are *the
 * terminal's* job: a program that has to be told "the user pressed control-C" as an
 * ordinary byte cannot implement it, and ^C has to work for a program that is blocked in a
 * read and not polling anything. */
static bool canonical_special(task_t *t, char c, const termios_t *ti, bool *end_of_input,
                              bool *erase_everything, bool *suspend) {
    *end_of_input = false;
    *erase_everything = false;
    *suspend = false;

    if (ti->c_cc[VERASE] != 0 && c == ti->c_cc[VERASE]) {
        /* Backspace erases the character *before* the cursor, which means moving the
         * cursor back, overwriting it with a space and moving it back again.  Simply not
         * writing the character is not the same: it leaves the old one on screen. */
        console_write("\b \b", 3);
        return true;
    }

    if (ti->c_cc[VKILL] != 0 && c == ti->c_cc[VKILL]) {
        *erase_everything = true;
        return true;
    }

    if (ti->c_cc[VEOF] != 0 && c == ti->c_cc[VEOF]) {
        *end_of_input = true;
        return true;
    }

    if ((ti->c_lflag & ISIG) && ti->c_cc[VINTR] != 0 && c == ti->c_cc[VINTR]) {
        /* ^C: the line goes, and the program is told.
         *
         * The signal is sent *after* the queue is emptied, because a signal whose handler
         * reads the terminal would otherwise read half the line the user was in the middle
         * of typing.  And the line goes first, so the handler starts from a clean line --
         * which is what `sh` needs when it catches ^C and prints a new prompt. */
        char ignored;

        while (pop(&ignored)) {
        }
        wait_queue_wake_all(&in_waiters);

        if ((ti->c_lflag & NOFLSH) == 0)
            console_write("^C\r\n", 3);
        else
            console_write("^C", 2);

        if (t != NULL)
            signal_send(t, SIGINT);
        return true;
    }

    if ((ti->c_lflag & ISIG) && ti->c_cc[VQUIT] != 0 && c == ti->c_cc[VQUIT]) {
        char ignored;

        while (pop(&ignored)) {
        }
        wait_queue_wake_all(&in_waiters);
        console_write("^\\\r\n", 4);
        if (t != NULL)
            signal_send(t, SIGQUIT);
        return true;
    }

    if ((ti->c_lflag & ISIG) && ti->c_cc[VSUSP] != 0 && c == ti->c_cc[VSUSP]) {
        char ignored;

        while (pop(&ignored)) {
        }
        wait_queue_wake_all(&in_waiters);
        console_write("^Z\r\n", 3);
        *suspend = true;
        return true;
    }

    return false;
}

long tty_read(uint64_t buf, size_t count) {
    task_t *t = sched_current();

    if (buf == 0 || count == 0)
        return -EINVAL;

    syscall_prepare_write(buf, count);

    if (!syscall_user_range_ok(buf, count, true))
        return -EFAULT;

    const termios_t *ti = termios_of(t);
    uint8_t *out = (uint8_t *)(uintptr_t)buf;
    size_t got = 0;

    /* Raw mode: one character per read, and it arrives as soon as it is typed.
     *
     * No line, no echo, no editing: that is what a program asking for raw mode is asking
     * for, and a `vi` or a pager that asked for it and got an echoed line would be
     * unusable.  ICRNL is honoured because a terminal that turns CR into NL is the one
     * thing a raw-mode program still expects. */
    if ((ti->c_lflag & ICANON) == 0) {
        char c;

        while (got < count) {
            if (!pop(&c)) {
                /* Nothing to read, so nothing to return: block.  The wait is on the
                 * discipline's own queue, not on the scheduler's blocked list, so a second
                 * task blocked here is woken by the keystroke that satisfies either of
                 * them. */
                uint32_t tag = (uint32_t)(uintptr_t)buf;

                wait_queue_enqueue_locked(&in_waiters, tag);
                wait_queue_block_locked(&in_waiters, tag);
                continue;
            }

            if ((ti->c_iflag & ICRNL) && c == '\r')
                c = '\n';
            if ((ti->c_iflag & IGNCR) && c == '\n')
                c = '\r';

            out[got++] = (uint8_t)c;

            if (ti->c_cc[VMIN] != 0 && got >= ti->c_cc[VMIN])
                break;
        }

        return (long)got;
    }

    /* Canonical mode: whole lines.
     *
     * A read returns when a newline has arrived *or* when the count is satisfied, whichever
     * comes first.  The second half matters and is not a detail: `head -c 10` reads ten
     * bytes of a file that has no newlines at all, and a discipline that only returns at a
     * newline would hang on every file that does not end in one. */
    while (got < count) {
        char c;

        if (!pop(&c)) {
            uint32_t tag = (uint32_t)(uintptr_t)buf;

            wait_queue_enqueue_locked(&in_waiters, tag);
            wait_queue_block_locked(&in_waiters, tag);
            continue;
        }

        bool eof = false, wipe = false, suspend = false;

        if (canonical_special(t, c, ti, &eof, &wipe, &suspend)) {
            if (suspend) {
                /* ^Z stops the process rather than killing it: the signal goes to the
                 * *group*, because a job stopped by ^Z has stopped as a whole.  There is
                 * only one group here -- the shell's -- until job control exists, so it
                 * goes to the caller. */
                if (t != NULL && ti->c_cc[VSUSP] != 0)
                    signal_send(t, SIGTSTP);
            }
            if (eof) {
                /* End of input with something already read: that something *is* the
                 * answer, and returning it is what lets `head` see the last line of a file
                 * that ends without a newline. */
                if (got > 0)
                    break;
                return 0;
            }
            if (wipe) {
                /* ^U: throw the whole line away.  The queue is emptied and the screen is
                 * told, and the cursor is left where it started.  Counting the characters
                 * erased is what makes the terminal agree with the terminal's own idea of
                 * where the cursor is; without it the next character lands in the wrong
                 * column and the line is unreadable. */
                uint32_t dropped = in_count;

                in_head = in_tail;
                in_count = 0;

                for (uint32_t i = 0; i < dropped; i++)
                    console_write("\b \b", 3);

                wait_queue_wake_all(&in_waiters);
            }
            continue;
        }

        if ((ti->c_lflag & ECHO) && c != '\n') {
            char echo[2] = { c, 0 };

            if ((ti->c_lflag & ECHOCTL) && (unsigned char)c < 32 && c != '\t') {
                /* A control character shows as ^x, because a terminal that prints the byte
                 * itself shows something the person cannot read. */
                char both[4] = { '^', (char)('@' + (unsigned char)c), 0, 0 };

                tty_write(both, 2);
            } else {
                tty_write(echo, 1);
            }
        }

        out[got++] = (uint8_t)c;

        if (c == '\n')
            break;
    }

    return (long)got;
}

long tty_write(const void *buf, size_t count) {
    /* Straight to the console, so the text is on the framebuffer and in the serial mirror
     * together.  The terminal does not interpret what is written to it -- no \r\n rewriting
     * beyond what the console already does -- because a program that writes a line ending
     * has already decided what it wanted. */
    console_write((const char *)buf, count);
    return (long)count;
}

void tty_input(char c) {
    /* A character that needs no line discipline still goes through the queue, so that a
     * raw-mode program and a canonical one see the same keystrokes in the same order.  What
     * differs is what the discipline does with them, and that is decided on the way out. */
    push(c);
    wait_queue_wake_all(&in_waiters);
}

long tty_ioctl(void *file, uint32_t request, uint64_t arg, uint64_t arg2) {
    (void)file;
    (void)arg2;

    task_t *t = sched_current();
    termios_t *ti = termios_of(t);

    switch (request) {
    case TIOCGETA:
        if (arg == 0)
            return -EFAULT;
        if (!syscall_copy_to_user(arg, ti, sizeof(*ti)))
            return -EFAULT;
        return 0;

    case TIOCSETA: {
        termios_t fresh;

        if (arg == 0)
            return -EINVAL;

        if (!syscall_copy_from_user(&fresh, arg, sizeof(fresh)))
            return -EFAULT;

        /* The line and the speeds are not this kernel's to change: there is one terminal,
         * its line is always a local one, and its speed is the speed of the wire it is not
         * on.  Accepting a request to set them would be accepting a setting that does
         * nothing, which is worse than refusing it. */
        fresh.c_line = 0;
        fresh.c_ispeed = 0;
        fresh.c_ospeed = 0;
        *ti = fresh;
        return 0;
    }

    case TIOCGWINSZ: {
        struct tty_winsize ws;

        ws.ws_row = (uint16_t)console_rows();
        ws.ws_col = (uint16_t)console_cols();
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;

        if (arg == 0)
            return -EFAULT;
        if (!syscall_copy_to_user(arg, &ws, sizeof(ws)))
            return -EFAULT;
        return 0;
    }

    case TIOCSWINSZ:
        /* The window size belongs to the terminal emulator, and this one has exactly the
         * size the bootloader gave it.  Accepting a resize that does nothing would make a
         * program believe it had more room than it does, and it would then write off the
         * bottom of the screen. */
        return 0;

    default:
        return -ENOTTY;
    }
}

/* --- attaching a program to the terminal ---------------------------------- */

static bool attached;

void tty_attach(bool on) {
    attached = on;
}

bool tty_attached(void) {
    return attached;
}

/* One input event, turned into terminal input.
 *
 * The keys that are not characters are not dropped: an arrow key has to reach a program as
 * the three-character sequence a program expects, or every program with cursor keys is
 * broken.  Written out as ESC [ A and friends, which is what every terminal sends and what
 * every line editor reads.
 *
 * Control keys become their control character and nothing else: ^C is 0x03 and the
 * discipline decides what that means, because whether it means SIGINT depends on ISIG. */
bool tty_forward_event(const input_event_t *ev) {
    if (ev->type != INPUT_KEY_DOWN)
        return true;

    uint32_t key = ev->key;

    switch (key) {
    case KEY_NONE:
    case KEY_CAPSLOCK:
    case KEY_NUMLOCK:
    case KEY_SCROLLLOCK:
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
        return true;                       /* a modifier is not a character */

    case KEY_ENTER:
        tty_input('\n');
        return true;

    case KEY_BACKSPACE:
    case 0x7f:
        tty_input(0x7f);
        return true;

    case KEY_TAB:
        tty_input('\t');
        return true;

    case KEY_UP:    tty_input(0x1b); tty_input('['); tty_input('A'); return true;
    case KEY_DOWN:  tty_input(0x1b); tty_input('['); tty_input('B'); return true;
    case KEY_RIGHT: tty_input(0x1b); tty_input('['); tty_input('C'); return true;
    case KEY_LEFT:  tty_input(0x1b); tty_input('['); tty_input('D'); return true;
    case KEY_HOME:  tty_input(0x1b); tty_input('['); tty_input('H'); return true;
    case KEY_END:   tty_input(0x1b); tty_input('['); tty_input('F'); return true;
    case KEY_DELETE: tty_input(0x1b); tty_input('['); tty_input('3'); tty_input('~'); return true;
    case KEY_INSERT: tty_input(0x1b); tty_input('['); tty_input('2'); tty_input('~'); return true;
    case KEY_PAGEUP: tty_input(0x1b); tty_input('['); tty_input('5'); tty_input('~'); return true;
    case KEY_PAGEDOWN: tty_input(0x1b); tty_input('['); tty_input('6'); tty_input('~'); return true;
    case KEY_ESCAPE: tty_input(0x1b); return true;

    default:
        break;
    }

    if (key >= KEY_UNICODE_BASE)
        return true;                       /* a function key, which no program here reads */

    if (key > 0x7f)
        return true;                       /* outside Latin-1: this terminal is 8-bit */

    /* A control character from the modifier, when the driver did not already produce one:
     * Ctrl-A is 1, Ctrl-@ is 0, and the whole range is "letter minus '@'" for letters and
     * the plain code otherwise.  Producing it here means a program's ^C is 0x03 whether the
     * keyboard sent a control code or a letter with control held. */
    if ((ev->modifiers & MOD_CTRL) && key >= 'a' && key <= 'z')
        key = key - 'a' + 1;

    tty_input((char)key);
    return true;
}

bool tty_ready(void) {
    return ready;
}

int tty_init(void) {
    if (ready)
        return 0;

    /* The control characters, by name.  A terminal whose ^C is a literal 0x03 and ^U a
     * literal 0x15 is a terminal that works for exactly one program and is unreadable in
     * its own source, so these are set once here and every setting starts from them. */
    defaults.c_cc[VINTR]  = 3;      /* ^C */
    defaults.c_cc[VQUIT]  = 0x1c;   /* ^backslash */
    defaults.c_cc[VERASE] = 0x7f;   /* DEL, which is what a keyboard sends for backspace */
    defaults.c_cc[VKILL]  = 0x15;   /* ^U */
    defaults.c_cc[VEOF]   = 4;      /* ^D */
    defaults.c_cc[VSUSP]  = 0x1a;   /* ^Z */
    defaults.c_cc[VSTART] = 0x11;   /* ^Q */
    defaults.c_cc[VSTOP]  = 0x13;   /* ^S */
    defaults.c_cc[VMIN]   = 1;
    defaults.c_cc[VTIME]  = 0;

    wait_queue_init(&in_waiters);
    wait_queue_init(&out_waiters);

    /* The terminal's input is the keyboard's output.  The keyboard driver pushes key
     * *events* -- press, release, shift, caps lock -- and a terminal wants characters, so
     * something has to turn one into the other.  That something is here, because it is the
     * line discipline's: a key that has no character on it is the terminal's business to
     * ignore, not the driver's.
     *
     * The console's own command loop reads the same queue directly, which is why it works
     * whether or not the terminal is up. */
    ready = true;
    return 0;
}