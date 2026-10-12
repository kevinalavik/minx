/* minx - <getopt.h>
 *
 * The option parser every coreutils shares.
 *
 * The contract is short and the state is global, which is not a design this file chose but
 * one it is reproducing: `getopt()` reads `argv` in place, leaves `optind` pointing at the
 * next unparsed argument for the *next* call, and hands back one option character at a time.
 * A program with two independent sets of options cannot use it, and every Unix that has it
 * has that same limitation, because the state is `optind` rather than anything the caller
 * passes in.
 *
 * The four knobs below are the whole interface:
 *
 *   optind    which argv element the next call looks at
 *   optarg    the argument of the option just returned, or NULL if it takes none
 *   optopt    the option character that caused an error, for the caller's own message
 *   opterr    0 to stay quiet about a bad option, 1 (the default) to print about it
 *
 * `getopt()` returns the option character, or -1 when the options are done, and ':' when
 * something is wrong *and* `optstring` began with a ':'.  That leading colon is the only way
 * to tell "an option that needed an argument did not have one" from "an option that is not
 * in the list", because both would otherwise be '?'.  A program that wants to say its own
 * message asks for it with the colon and uses `optopt`.
 */
#ifndef MINX_GETOPT_H
#define MINX_GETOPT_H

/* The three outside parameters.  They are not `extern int` in a header because every
 * program that includes this must see the *same* object, and a `static` in a header would
 * give each program its own -- which is the single most common way a hand-rolled `getopt`
 * fails, silently, in a program that works perfectly on its own. */
extern char *optarg;
extern int   optind;
extern int   opterr;
extern int   optopt;

/* `getopt()` takes `argc`/`argv` on every call rather than reading them from somewhere of
 * its own, which is what lets a program parse a list that is not `argv` at all. */
int getopt(int argc, char *const argv[], const char *optstring);

/* `optreset()` is not POSIX, and is on BSD, on glibc and on musl with the same name and the
 * same meaning: put the parser back at the beginning so a second option set can be parsed.
 * A program with two modes -- `tar`, `grep` -- needs it, and setting `optind = 1` by hand
 * is not the same thing, because the "rest of this element" position is state too. */
void optreset(void);

#endif /* MINX_GETOPT_H */
