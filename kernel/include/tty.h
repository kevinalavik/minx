/* minx - the terminal line discipline.
 *
 * The console turns out to be two different things and conflating them is what makes a
 * shell's job impossible.  There is a *screen* -- an 80x24 grid of characters that a
 * program can print to -- and there is a *terminal*, which is a line discipline in front of
 * a screen: it assembles what a person types into lines, echoes it, handles ^C and ^U and
 * backspace, and hands a program one whole line at a time.
 *
 * A program that reads a line wants the second.  `getchar()` in a program with no line
 * discipline is reading the keyboard's event stream and has to do the assembling itself,
 * and every program then does it slightly differently, and ^C becomes "whatever the driver
 * decided to send for the interrupt key".
 *
 * So this is the terminal half, and the console is the screen half.  A program that opens
 * /dev/tty gets this; a program that writes to fd 1 gets the screen.  That is what makes
 * `echo hi > /dev/tty` put its output *on the terminal* while `cat` waits for a line.
 *
 * The state is per-process.  A program that turns off echo turns it off for itself; two
 * programs sharing one terminal cannot both have their own echo setting, and the rule here
 * is the one every system uses: the settings belong to whichever process most recently
 * asked, and a process that never asked gets the defaults.
 */
#ifndef MINX_TTY_H
#define MINX_TTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "input.h"

/* --- termios ------------------------------------------------------------- */

/* The `termios` a program sees.
 *
 * Four flags this implementation acts on and a flags field for the rest: a program compiled
 * against a system that has forty flags sets all of them, and a kernel that refuses to
 * accept the structure because it does not recognise every bit would make every such
 * program fail on a field it never uses.  Unknown bits are kept and echoed back. */
typedef struct {
    uint32_t c_iflag;     /* input modes  */
    uint32_t c_oflag;     /* output modes */
    uint32_t c_cflag;     /* control modes */
    uint32_t c_lflag;     /* local modes   */
    uint8_t  c_line;      /* line discipline, always 0 here */
    uint8_t  c_cc[19];    /* control characters, see below */
    uint32_t c_ispeed;
    uint32_t c_ospeed;
} termios_t;

/* Input modes. */
#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define INPCK   0x0008
#define ISTRIP  0x0010
#define INLCR   0x0020
#define IGNCR   0x0040
#define ICRNL   0x0100
#define IXON    0x0400

/* Output modes. */
#define OPOST   0x0001
#define ONLCR   0x0004
#define OCRNL   0x0008

/* Local modes: the ones that decide whether a person types characters or lines. */
#define ISIG    0x0001    /* ^C ^\ and friends do their thing          */
#define ICANON  0x0002    /* lines, not characters                       */
#define ECHO    0x0008    /* what is typed appears on the terminal      */
#define ECHOE   0x0010    /* and backspace erases                        */
#define ECHOK   0x0020    /* and ^C erases the line                      */
#define ECHONL  0x0040
#define NOFLSH  0x0080    /* ^C does not flush the screen                */
#define TOSTOP  0x0100
#define ECHOCTL 0x0200    /* a control character shows as ^x             */
#define IEXTEN  0x8000

/* Control characters, as indices into c_cc. */
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSTART  8
#define VSTOP   9
#define VSUSP   10
#define NCCS    19

/* The defaults, which are what a terminal with nothing configured behaves like. */
termios_t *tty_default_termios(void);

/* --- the line discipline -------------------------------------------------- */

/* Bring the terminal up.  Called once from kmain(), after the console exists. */
int  tty_init(void);
bool tty_ready(void);

/* Read a line, or `count` bytes, or one character -- whichever the settings ask for.
 * Blocks until there is something.  The kernel reads for the calling task, so this is only
 * ever called from a syscall. */
long tty_read(uint64_t buf, size_t count);

/* Write to the terminal: through the screen, so that the echo is visible and mirrored. */
long tty_write(const void *buf, size_t count);

/* `ioctl` on a terminal descriptor: TIOCGTERMIO, TIOCSTERMIO, TIOCGWINSZ, TIOCSWINSZ. */
long tty_ioctl(void *file, uint32_t request, uint64_t arg, uint64_t arg2);

/* The request numbers, which are every system's. */
#define TIOCGETA   0x5401
#define TIOCSETA   0x5402
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCSCTTY  0x540E

struct tty_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* Put a character into the line discipline from outside -- the keyboard driver calls this,
 * and so does anything that wants to type at a program. */
void tty_input(char c);

/* Whether a user program has the terminal.
 *
 * This is the answer to "who gets the keystrokes", and the kernel's own console command
 * loop is the other party.  When a program is attached it gets them and the loop does not,
 * because a keystroke that is echoed twice -- once by the loop's line editor and once by
 * the terminal's -- is a keystroke a person sees twice and a program reads once. */
void tty_attach(bool attached);
bool tty_attached(void);

/* Turn one input event into terminal input, and nothing at all for a key that has no
 * character.  Called by the console's event pump, which is the only thing that reads the
 * keyboard queue. */
bool tty_forward_event(const input_event_t *ev);

#endif /* MINX_TTY_H */