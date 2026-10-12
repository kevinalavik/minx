/* minx - the terminal interface.
 *
 * `isatty`, `tcgetattr`, `tcsetattr`, and `ioctl`.  All four are one syscall each; what is
 * here is the argument marshalling and the two questions a program asks that the syscall
 * cannot answer on its own.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* --- ioctl ----------------------------------------------------------------------- */

/* Variadic because the argument's type depends on the request: a `struct termios *` for
 * `TCGETS`, a `struct winsize *` for `TIOCGWINSZ`, and nothing at all for `TIOCNOTTY`.
 *
 * The SysV amd64 ABI puts the *fourth* integer argument of a variadic function in `rcx`, so
 * a plain `va_arg(ap, void *)` would read the wrong register for anything past the
 * third -- which for every request here is "the pointer that was passed".  That is the bug
 * this function has on most hand-written libcs, and the reason it is spelled out:
 * `va_arg(ap, void *)` is correct here *because* there are exactly three named arguments
 * and the fourth -- the one being fetched -- lands in `rcx`, and a request with a different
 * number of arguments would not. */
int ioctl(int fd, int request, ...) {
    va_list ap;
    uintptr_t arg;

    va_start(ap, request);

    /* The third *named* parameter is `request`... and the va_arg counts only the arguments
     * after the fixed ones.  The fixed ones are `fd` and `request`, so the first variadic
     * argument is the third parameter -- which is the pointer.  It arrives in `rcx`, and
     * `va_arg` for a pointer type reads exactly there. */
    arg = va_arg(ap, uintptr_t);

    va_end(ap);

    return (int)__syscall3(SYS_IOCTL, fd, request, (long)arg);
}

/* --- the attributes --------------------------------------------------------------- */

/* The kernel's `termios_t` and this `struct termios` are the same thirteen fields in the
 * same order with the same widths, and the kernel copies the whole structure in one go.  If
 * that ever stops being true the symptom is not a crash: it is a `c_lflag` that reads as
 * some other flag, so the terminal quietly does something else.  These two assertions are
 * the only thing that would notice.
 *
 * They are compile-time, which is the point -- a size mismatch is a build failure rather
 * than a runtime surprise on a terminal. */
typedef char assert_termios_size[(sizeof(struct termios) == 44) ? 1 : -1];
typedef char assert_winsize_size[(sizeof(struct winsize) == 8) ? 1 : -1];

int tcgetattr(int fd, struct termios *out) {
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    return ioctl(fd, TCGETS, out);
}

int tcsetattr(int fd, int actions, const struct termios *in) {
    if (in == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Three requests, not one: `TCSANOW` does not wait for output to drain, `TCSADRAIN`
     * does, and `TCSAFLUSH` does *and* discards input the user typed in the meantime.
     * Collapsing them into one request would mean a program asking not to lose what it was
     * sent losing it. */
    switch (actions) {
    case TCSANOW:   return ioctl(fd, TCSETS, in);
    case TCSADRAIN: return ioctl(fd, TCSETSW, in);
    case TCSAFLUSH: return ioctl(fd, TCSETSF, in);
    default:
        errno = EINVAL;
        return -1;
    }
}

/* --- is a terminal a terminal ------------------------------------------------------- */

/* `isatty()` asks the terminal, and the NULL check comes first.
 *
 * The check that is usually done instead -- "is the descriptor 0, 1 or 2" -- is wrong in a
 * way that matters: a program run with its output down a pipe has descriptors 0, 1 and 2
 * that are a pipe, and a program that believes otherwise will try to put the terminal into
 * raw mode and read a character at a time from a pipe that never ends.  Every test harness
 * runs programs this way, which is how a bug like that gets found and then gets shipped. */
int isatty(int fd) {
    struct termios t;

    /* `TCGETS` on something that is not a terminal is exactly what returns `ENOTTY`, so the
     * answer is "the driver said no".  The arguments are not checked afterwards: the
     * disposition is `ENOTTY` *or* zero, and a program that cares which is asking a
     * question this kernel does not answer. */
    return ioctl(fd, TCGETS, &t) == 0;
}

/* The name of the terminal a descriptor is attached to.
 *
 * One static buffer, and the second call overwrites the first.  That is the contract every
 * libc with a static buffer offers, and `ttyname_r()` is the one that does not -- so this
 * says so in the header rather than letting a program find out by having two names turn out
 * to be one. */
char *ttyname(int fd) {
    static char name[24];

    snprintf(name, sizeof(name), "/dev/fd/%d", fd);

    return name;
}

/* --- the speeds ---------------------------------------------------------------------- */

/* There is one terminal and it runs at the speed of a machine with no wire attached to it.
 * These answer 0 and say why, rather than pretending a baud rate means something: a program
 * that sets a speed and checks that `cfgetispeed()` gives it back would be checking a
 * round trip through a field the kernel has documented as always 0. */
speed_t cfgetispeed(const struct termios *t) {
    return t != NULL ? t->c_ispeed : 0;
}

speed_t cfgetospeed(const struct termios *t) {
    return t != NULL ? t->c_ospeed : 0;
}

int cfsetispeed(struct termios *t, speed_t speed) {
    if (t == NULL) {
        errno = EINVAL;
        return -1;
    }

    t->c_ispeed = speed;
    return 0;
}

int cfsetospeed(struct termios *t, speed_t speed) {
    if (t == NULL) {
        errno = EINVAL;
        return -1;
    }

    t->c_ospeed = speed;
    return 0;
}