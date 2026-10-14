/* `echo` -- write its arguments to standard output, separated by one blank.
 *
 * One of the six programs `shtest`'s script needs, and the reason phase 9 could not finish
 * before phase 10 started: the script pipes into `cat`, counts lines with `wc -l`, takes
 * the last with `tail`, globs with `ls`, and truncates with `rm`.  A shell test that fails
 * because a program does not exist is testing the wrong thing -- the report says the shell
 * is wrong, and the shell is right.
 *
 * Written against the libc, not the raw door: a program whose job is formatting output
 * should not also be the thing that tests the formatter.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The escapes a POSIX shell's `echo` knows, because a script that uses them and a shell that
 * does not implement them disagree in a way that is very hard to read.
 *
 * `-n` is the flag form rather than an escape, and it is checked first because `echo -n`
 * is a flag and `echo "\-n"` is a word -- the backslash is what tells them apart, and this is
 * why the escape handling runs *after* the flag check rather than before it. */
static void put_escapes(const char *s) {
    for (; *s != 0; s++) {
        if (*s != '\\' || s[1] == 0) {
            putchar(*s);
            continue;
        }

        s++;

        switch (*s) {
        case 'a': putchar('\a'); break;
        case 'b': putchar('\b'); break;
        case 'e': putchar(0x1b); break;
        case 'f': putchar('\f'); break;
        case 'n': putchar('\n'); break;
        case 'r': putchar('\r'); break;
        case 't': putchar('\t'); break;
        case 'v': putchar('\v'); break;
        case '\\': putchar('\\'); break;
        /* A backslash before anything else is that character, as in a C string and as in
         * every shell.  Dropping it is what lets `echo a\ b` print one `a b`. */
        default: putchar(*s); break;
        }
    }
}

/* `minx_main()`, not `main()`: `user/common/start.S` calls that name, and the libc's
 * `exit.S` provides it for every program that links the libc.  A plain `main()` links to
 * nothing and the error names the wrong symbol. */
int minx_main(unsigned long argc, char **argv) {
    /* `argc` arrives as an `unsigned long` because that is what the entry stub passes, and
     * every loop index here is an `int`.  They are the same value on this ABI; the cast
     * says so once rather than at every comparison, and says it because a `-Wsign-compare`
     * that has been silenced in the build is a real class of bug papered over rather than
     * answered. */
    int  nargs = (int)argc;
    int  start = 1;
    int  newline = 1;
    int  i;

    /* `-n` only when it is the first argument and nothing follows it in the same word: a
     * program called `-nope` is a program called `-nope`. */
    if (nargs > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        start = 2;
    }

    for (i = start; i < nargs; i++) {
        if (i > start)
            putchar(' ');

        put_escapes(argv[i]);
    }

    if (newline)
        putchar('\n');

    /* The status is a failure to *write*, not a failure to format: `echo hi > /read-only`
     * has to fail, or a script that redirects into the wrong place carries on as if it had
     * worked.  `fflush()` first so that a buffered write is on its way before the check. */
    if (fflush(stdout) != 0 || ferror(stdout))
        return 1;

    return 0;
}