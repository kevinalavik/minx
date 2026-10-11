/* minx - `exectest`: replace this process with another program, and check that the old one
 * is really gone.
 *
 * It exists as its own program rather than as a section of `hello` for the reason the
 * registry exists: a test that lives inside another program's text is a test that changes
 * what it is testing.  `hello` grew past 4 KiB of text while the signal checks were being
 * added, and the loader bug that exposed had been there all along.
 *
 * It execs `hello`, which prints and exits 0.  So a successful run ends with the output of
 * a program that was never started by this one, and the self test that runs this can tell
 * the difference: if `exectest` had merely returned, its own marker would still be on the
 * console afterwards.
 */
#include "../common/minx_user.h"

/* Not static: the entry point in start.S calls it, and a `static` would be a warning the
 * compiler is right to give and nobody can act on. */
int minx_main(unsigned long argc, char **argv);

int minx_main(unsigned long argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Printed before, so that reaching the end is visible: if `exec` returns instead of
     * replacing this process, these two lines are the whole report. */
    minx_write(1, "exectest: about to become hello\n", 30);

    char *args[2];
    args[0] = (char *)"hello";
    args[1] = 0;

    long r = minx_exec("hello", args, 0);

    /* Only reached on failure.  A successful `exec` has unmapped this program's text and
     * stack by now, so there is nothing here to return to and nothing here to be. */
    minx_write(1, "exectest: FAILED, exec returned ", 31);
    minx_write(1, "?", 1);
    minx_write(1, "\n", 1);
    minx_exit((int)r);

    return 90;
}