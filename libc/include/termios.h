/* minx - <termios.h>
 *
 * The terminal's attributes, and the calls that read and write them.
 *
 * The layout is the kernel's own, copied in one go: `kernel/include/tty.h`'s `termios_t`,
 * field for field and in the same order.  The two have to agree exactly, and the reason
 * they are written out twice rather than shared is that the kernel is a freestanding build
 * with no include path to a libc.  `libc/src/termios.c` has a compile-time assertion about
 * the sizes, which is the only thing that notices when one of them changes.
 *
 * The flags are 32-bit fields rather than the `tcflag_t` of other systems, and that is
 * deliberate for the reason the kernel's comment gives: a program compiled against a
 * system with forty flags in each field sets all of them, and a structure the kernel
 * refuses because it does not recognise every bit makes that program fail on a field it
 * never reads.  Unknown bits are kept and echoed back instead.
 */
#ifndef MINX_TERMIOS_H
#define MINX_TERMIOS_H

#include <sys/sysdep.h>

typedef uint32_t tcflag_t;
typedef uint8_t  cc_t;
typedef uint32_t speed_t;

struct termios {
    tcflag_t c_iflag;      /* input modes  */
    tcflag_t c_oflag;      /* output modes */
    tcflag_t c_cflag;      /* control modes */
    tcflag_t c_lflag;      /* local modes   */
    cc_t     c_line;       /* line discipline; always 0 on a local terminal */
    cc_t     c_cc[19];     /* control characters, indexed by the V* constants */
    tcflag_t c_ispeed;     /* input speed; 0, because there is one speed */
    tcflag_t c_ospeed;     /* output speed; 0, for the same reason */
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* --- input modes ------------------------------------------------------------- */

/* The only one of these that changes anything: without it the terminal delivers every byte
 * the keyboard produces, including the one that is Ctrl-C.  A program that installs a
 * SIGINT handler and does not ask for this gets a stream of 0x03 instead. */
#define IGNBRK 0000001
#define BRKINT 0000002
#define IGNPAR 0000004
#define PARMRK 0000010
#define INPCK  0000020
#define ISTRIP 0000040
#define INLCR  0000100
#define IGNCR  0000200
#define ICRNL  0000400
#define IXON   0002000

/* --- output modes ------------------------------------------------------------ */
#define OPOST  0000001
#define ONLCR  0000004
#define OCRNL  0000008
#define ONOCR  0000010
#define ONLRET 0000040

/* --- control modes ----------------------------------------------------------- */
#define CSIZE   0000060
#define CS7     0000040
#define CS8     0000060
#define CSTOPB 0000100
#define CREAD   0000200
#define PARENB  0000400
#define PARODD  0001000
#define HUPCL   0004000
#define CLOCAL  0008000
#define CRTSCTS 0040000

/* --- local modes ------------------------------------------------------------- */
/*
 * These have to be the *same bits* as the kernel's, and they were not.
 *
 * The old spellings were `ISIG 1, ICANON 2, ECHO 8, ECHOE 16, ECHOK 32, ECHONL 64,
 * NOFLSH 128, TOSTOP 256, IEXTEN 4096`; the kernel's are the same up to `TOSTOP` and then
 * `ECHOCTL 512, IEXTEN 32768`.  Two consequences, both of them quiet:
 *
 *   - A program that asked for `ECHOCTL` had no name for it, so its `~(ICANON|ECHO)` mask
 *     left the kernel's `ECHOCTL` bit standing and every `^C` in a line arrived as a raw
 *     0x03 on the screen.
 *   - A program that set `IEXTEN` set bit 12, which the kernel reads as nothing, so
 *     `IEXTEN` was never actually set by anything written against this header.
 *
 * The kernel's values are in `kernel/include/tty.h`, and this list is the same list.  There
 * is no shared header -- the kernel is a freestanding build with no include path to a
 * libc, and libc is built without one either -- so the two have to agree by reading, and
 * the check in kernel/include/tty.h is only about the *size* of the structure. */
#define ISIG    0000001    /* Ctrl-C, Ctrl-\ and Ctrl-Z generate signals     */
#define ICANON  0000002    /* line-buffered: deliver a line at a time        */
#define ECHO    0000010    /* echo what is typed                              */
#define ECHOE   0000020    /* echo the erase character as an erase            */
#define ECHOK   0000040    /* echo a newline for a kill                        */
#define ECHONL  0000100    /* echo a newline even in canonical mode            */
#define NOFLSH  0000200    /* ^C does not flush the screen                    */
#define TOSTOP  0000400
#define ECHOCTL 0001000    /* a control character shows as ^x                 */
#define IEXTEN  0200000

/* What `tcsetattr()` does about output that has not been written yet.  `TCSANOW` is the
 * one that does not wait, and it is the one a program in a hurry uses -- a shell redrawing
 * its prompt with `TCSADRAIN` waits for a slow terminal to finish first, which is a visible
 * pause for no benefit on a terminal whose output is the same buffer it is writing to. */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* --- control characters ------------------------------------------------------ */

/* Indices into `c_cc`, not values.  A program that sets `c_cc[VINTR]` to something other
 * than 3 is telling the terminal which byte *is* interrupt, which is what these names
 * mean; the number 3 is a default, not the meaning. */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

#define NCCS 19

#define _POSIX_VDISABLE 0

/* Index by the *name*, not the value: `c_cc[VINTR]` is interrupt, and 3 is only the byte
 * that happens to be there. */
#define NCCS_INDEX_MAX NCCS

/* --- the calls --------------------------------------------------------------- */

int tcgetattr(int fd, struct termios *out);
int tcsetattr(int fd, int actions, const struct termios *in);

/* `cfgetispeed()` and `cfsetispeed()` exist because <termios.h> that does not have them is
 * a <termios.h> a program has to work around.  They answer 0, which is the honest answer
 * here: there is one terminal and it runs at the speed of a machine that has none. */
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int     cfsetispeed(struct termios *t, speed_t speed);
int     cfsetospeed(struct termios *t, speed_t speed);

/* The speeds, as the numbers other systems use, so that a program which names one gets a
 * name rather than a missing identifier.  None of them changes anything. */
#define B0        0
#define B50       1
#define B75       2
#define B110      3
#define B134      4
#define B150      5
#define B200      6
#define B300      7
#define B600      8
#define B1200     9
#define B1800    10
#define B2400    11
#define B4800    12
#define B9600    13
#define B19200   14
#define B38400   15
#define B57600   16
#define B115200  17
#define B230400  18
#define B460800  19
#define B921600  20

#endif /* MINX_TERMIOS_H */