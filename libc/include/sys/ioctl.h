/* minx - <sys/ioctl.h>
 *
 * `ioctl()` and the terminal's request numbers.
 *
 * The requests are the kernel's own, written out.  There is no `_IOC()` encoding macro here
 * on purpose: the usual scheme packs a direction, a type letter, a number and an argument
 * size into one word, and shipping the macros would mean shipping a promise about how this
 * kernel encodes a request.  It does not -- `kernel/include/tty.h` numbers them directly --
 * so a program that computed a request with a glibc `_IOR()` would get a number this
 * kernel has never heard of, and would fail with ENOTTY on a terminal it was talking to.
 *
 * A number written down cannot be wrong in that particular way.  What it can be is
 * different from another system's, which is a smaller and honest failure: `tcgetattr()`
 * and friends work, because they go through these numbers, and a program with a hard-coded
 * request number is a program that has to be checked.
 *
 * Both spellings are here -- `TCGETS` and `TIOCGETA` -- because the two systems that
 * disagree about the name also disagree about the number, and a program that includes this
 * header should find the name it is looking for either way.
 */
#ifndef MINX_SYS_IOCTL_H
#define MINX_SYS_IOCTL_H

#include <termios.h>

/* --- the terminal's requests -------------------------------------------------- */

#define TIOCGETA   0x5401    /* read the attributes into a `struct termios`   */
#define TIOCSETA   0x5402    /* set them, waiting for output to drain        */
#define TIOCSETSW  0x5403    /* set them, waiting for output to drain        */
#define TIOCSETSF  0x5404    /* set them and discard queued input            */
#define TIOCSCTTY  0x540E    /* make this terminal the controlling one       */
#define TIOCNOTTY  0x5422    /* give it up                                     */
#define TIOCGWINSZ 0x5413    /* read the window size                          */
#define TIOCSWINSZ 0x5414    /* set it                                         */

/* The other two systems' names for the first four.  They are the same requests: a
 * terminal program that says `TCGETS` and one that says `TIOCGETA` are asking the same
 * question, and the header that makes both spellings work is what lets one source do. */
#define TCGETS  TIOCGETA
#define TCSETS  TIOCSETA
#define TCSETSW TIOCSETSW
#define TCSETSF TIOCSETSF

/* --- ioctl() ----------------------------------------------------------------- */

/* Variadic because the argument is a pointer on some requests, an integer on others, and
 * nothing on a third -- which is precisely why `ioctl` is variadic and why this function
 * is the one place in a libc where the argument's type cannot be described by the
 * prototype.  Callers cast it: `ioctl(fd, TIOCGWINSZ, &ws)`. */
int ioctl(int fd, int request, ...);

#endif /* MINX_SYS_IOCTL_H */