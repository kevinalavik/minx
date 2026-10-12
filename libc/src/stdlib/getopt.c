/* minx - `getopt()`, in one pass over `argv`.
 *
 * The interface is the one every Unix has, which means the interface is a pile of global
 * state and a convention about where the argument of an option lives.  Reproducing it
 * exactly is the whole job, because the programs that use it are written against it: a
 * `getopt()` that returned the options in a different order, or that clobbered `argv`, or
 * that required the options to come first, would break every coreutils in the tree rather
 * than any of them individually.
 *
 * Four behaviours are worth stating, because they are what people are surprised by and they
 * are all load-bearing:
 *
 *   - Parsing stops at the first non-option.  `ls -l file -R` reports `-l` and stops, with
 *     `optind` pointing at `file`.  This is what makes `sh -c 'cmd' arg1 -x` work: the
 *     shell's own `-c` stops at `arg1` and leaves `-x` for the command.  A parser that kept
 *     going would take the command's options for its own.
 *
 *   - `--` ends the options.  Everything after it is a file name, which is the only way to
 *     name a file that begins with a dash.
 *
 *   - A bare `-` is a file name, not an option.  It is the conventional way to say "standard
 *     input", and a parser that treated it as an option would look up the character the
 *     terminating NUL is made of and -- because `strchr()` finds the terminator -- find it.
 *
 *   - An option's argument may be attached (`-Wall`) or separate (`-W all`), and when
 *     `optstring` begins with ':' a missing argument is reported as ':' with `optopt` set,
 *     rather than as the '?' that also means "no such option".  Without the leading colon
 *     the two are the same value and the caller cannot write a specific message.
 */
#include <getopt.h>
#include <stdio.h>
#include <string.h>

char *optarg;
int   optind = 1;
int   opterr = 1;
int   optopt = '?';

/* Where in the current element the next option character is.
 *
 * 0 means "at the start of a fresh element, which has not been classified yet" -- which is
 * not the same as "one past the end", because classifying an element is what decides
 * whether it is an option at all.
 *
 * Needed at all because an option's argument may be attached: `-Wall` is `-W` with the
 * argument "all", and there is nowhere else to record that the rest of `argv[optind]` has
 * already been eaten. */
static int nextchar;

void optreset(void) {
    optind   = 1;
    optarg   = NULL;
    optopt   = '?';
    nextchar = 0;
}

int getopt(int argc, char *const argv[], const char *optstring) {
    /* The leading ':' means "do not print, tell me", and it is stripped before anything
     * looks for an option character -- otherwise ':' would itself be a valid option. */
    int quiet = (optstring[0] == ':');

    if (quiet)
        optstring++;

    optarg = NULL;

    /* A program that sets `optind = 0` is asking to start again.  Honouring it is what
     * makes the GNU convention work, and it is the only way a program can reparse an
     * `argv` it has already permuted. */
    if (optind == 0)
        optreset();

    if (optind >= argc || argv[optind] == NULL)
        return -1;

    if (nextchar == 0) {
        const char *elem = argv[optind];

        /* Not an option.  `optind` is left *on* it rather than one past it, because the
         * caller loops with `optind < argc` and expects to read the file names itself. */
        if (elem[0] != '-' || elem[1] == 0)
            return -1;

        /* `--`, and nothing after it is an option ever again. */
        if (elem[1] == '-' && elem[2] == 0) {
            optind++;
            return -1;
        }

        nextchar = 1;
    }

    const char *elem = argv[optind];
    char        c    = elem[nextchar];

    nextchar++;

    /* End of this element: the next one may be this option's argument, and may equally be
     * another option or a file name in its own right. */
    if (elem[nextchar] == 0) {
        nextchar = 0;
        optind++;
    }

    const char *found = strchr(optstring, c);

    if (found == NULL || c == ':') {
        /* An option this program does not know.  Reported through `optopt` so a caller
         * using the leading colon can name it, and then *skipped* rather than treated as
         * the end of the options: a program that gives up on the first unknown option
         * cannot be given a new one by whoever invokes it, which is the whole point of
         * options being optional. */
        optopt = c;

        if (opterr && !quiet)
            fprintf(stderr, "%s: unknown option -- '%c'\n", argv[0], c);

        return '?';
    }

    /* Does it take an argument?  A ':' immediately after it in `optstring` is the spelling,
     * and looking for the character rather than a fixed offset is what lets `find` accept
     * ":" as an option without ':' being the "takes an argument" marker. */
    if (found[1] != ':')
        return c;

    /* It takes one.  Attached is the common spelling and costs nothing to check. */
    if (nextchar != 0) {
        optarg   = (char *)elem + nextchar;
        nextchar = 0;
        return c;
    }

    /* Not attached, so it has to be the next element -- and there may not be one. */
    if (optind >= argc) {
        optopt = c;

        if (opterr && !quiet)
            fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);

        return quiet ? ':' : '?';
    }

    optarg = argv[optind++];
    return c;
}
