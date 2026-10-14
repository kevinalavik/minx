/* `grep` -- print the lines of a file that match a pattern.
 *
 * The first tool in the Definition of Done's `grep -r root /etc | sort | uniq -c | wc -l`,
 * and the one that has to be recursive for that line to mean much: without `-r` it is
 * `grep root /etc`, which is one file.
 *
 * **The pattern is a fixed string, not a regular expression.**  There is no regex engine in
 * this system and inventing a partial one would be worse than not having it.  A script that
 * writes `grep "^root:" /etc/passwd` and gets the *literal* text `^root:` finds nothing and
 * reports no match, which is indistinguishable from the file not containing what it wanted --
 * so the failure is silent and looks like an answer.  `^`, `$`, `.`, `*` and `[]` are
 * ordinary characters here.  `-F` exists and does nothing, because it is what a reader
 * reaches for when they want to be sure.
 *
 * **A line is a line of any length.**  A fixed buffer would split a long one and print
 * halves, each of which is a line the file does not contain.  So the buffer grows, and a file
 * whose longest line is longer than memory is the one input that fails.
 *
 * **Output is flushed per line.**  `grep bigfile | head -n 1` has to stop, and a `grep` that
 * holds its output in a stdio buffer until it has read the whole file makes `head` wait for
 * the whole file.  This is the difference between `head -n 1` being a window and being a
 * wait.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXPATTERNS 256
#define MAXDEPTH    64

static int opt_ignore_case;
static int opt_invert;
static int opt_number;
static int opt_files_with_matches;
static int opt_count;
static int opt_quiet;
static int opt_recursive;
static int opt_print_name;

/* Whether *any* file matched anywhere, and whether any could not be searched.
 *
 * Both are file-scope because the exit status is about the whole run and a function cannot
 * carry it out.  `search_path()` answers for a path and `search_stream()` for a stream;
 * neither knows what the other has already seen, so a version that returned "did this match"
 * up the chain lost every earlier file's answer on the way past. */
static int any_match;
static int any_error;

/* The patterns, from `-e` and from the first non-option word.
 *
 * More than one because `-e a -e b` is how a pattern beginning with `-` is written, and
 * because "this or that" has to be built out of several patterns rather than an alternation
 * this program does not have. */
static const char *patterns[MAXPATTERNS];
static int         npatterns;

/* Case-insensitive substring search.
 *
 * Written out rather than using `strcasestr()`, which is POSIX-2008 and not something to
 * depend on.
 *
 * **The line's terminator is a bound, not a hint.**  The version this replaces walked `h`
 * forward past the end of the line looking for the next pattern character and only stopped
 * when it found one -- so on `user:x:1:1` against `ROOT` it walked off the end of the string
 * and, on this system's allocator, off the end of the page with it.  It did not fault
 * because the memory past a short string usually happens to hold a match; it matched
 * *everything*.  `grep -i ROOT a.txt` printed all three lines.  A search that cannot say no
 * is not a search. */
static int contains_ci(const char *hay, const char *needle) {
    if (*needle == 0)
        return 1;

    for (; *hay != 0; hay++) {
        const char *h = hay;
        const char *n = needle;

        while (*n != 0 && *h != 0 &&
               tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }

        if (*n == 0)
            return 1;
    }

    return 0;
}

/* Does `line` contain any pattern?
 *
 * An empty pattern matches every line.  `grep '' file` prints the file, which is a real
 * answer to a real question -- "how many lines are in this" -- and a program that treated it
 * as a mistake would have to refuse a question it can answer. */
static int matches(const char *line) {
    int any = 0;

    for (int i = 0; i < npatterns && !any; i++) {
        const char *pat = patterns[i];

        if (!opt_ignore_case) {
            any = strstr(line, pat) != NULL;
            continue;
        }

        any = contains_ci(line, pat);
    }

    return opt_invert ? !any : any;
}

/* Read one line, without its terminator, into a growable buffer.
 *
 * 1 for a line, 0 for end of file, -1 for an error or for running out of memory.  A last line
 * with no terminator is still a line: every file a program writes may end without one, and
 * dropping it loses the last line of every such file. */
static int read_line(FILE *f, char **buf, size_t *cap, size_t *len_out) {
    size_t len = 0;

    for (;;) {
        int c = fgetc(f);

        if (c == EOF) {
            if (ferror(f))
                return -1;

            if (len == 0)
                return 0;

            break;
        }

        if (c == '\n')
            break;

        if (len + 2 > *cap) {
            size_t want = *cap ? *cap * 2 : 256;
            char  *grown = realloc(*buf, want);

            if (grown == NULL)
                return -1;

            *buf = grown;
            *cap = want;
        }

        (*buf)[len++] = (char)c;
    }

    (*buf)[len] = 0;
    *len_out = len;
    return 1;
}

static void report(const char *path, int err) {
    fprintf(stderr, "grep: %s: %s\n", path, strerror(err));
}

/* Search one open stream, printing whatever the options ask for.  `name` is printed before
 * the first matching line and only then, so a many-file search's output is one contiguous
 * block per file and can be pasted into an editor.
 *
 * Returns how many lines matched, which is what `-c` prints and what a directory's total is
 * the sum of. */
static long search_stream(FILE *f, const char *name) {
    char  *buf = NULL;
    size_t cap = 0;
    long   lineno = 0;
    long   hits = 0;
    int    named = 0;

    for (;;) {
        size_t len = 0;
        int    got = read_line(f, &buf, &cap, &len);

        if (got <= 0)
            break;

        lineno++;

        if (!matches(buf))
            continue;

        hits++;

        /* `-q`, `-l` and `-c` all still have to *know* what matched -- that is what decides
         * the exit status -- and none of them prints the line. */
        if (opt_quiet || opt_files_with_matches || opt_count)
            continue;

        if (opt_print_name && name != NULL && !named) {
            printf("%s:", name);
            named = 1;
        }

        if (opt_number)
            printf("%ld:", lineno);

        printf("%s\n", buf);

        if (fflush(stdout) != 0)
            break;
    }

    free(buf);
    return hits;
}

static int is_dir(const struct stat *st) {
    return S_ISDIR(st->st_mode) != 0;
}

static int is_regular(const struct stat *st) {
    return S_ISREG(st->st_mode) != 0;
}

static long search_dir(const char *path, int depth);

/* Search one file.  Returns how many of its lines matched. */
static long search_file(const char *path) {
    FILE *f = fopen(path, "r");

    if (f == NULL) {
        report(path, errno);
        any_error = 1;
        return 0;
    }

    long hits = search_stream(f, path);

    fclose(f);

    if (hits > 0)
        any_match = 1;

    if (opt_files_with_matches && hits > 0)
        printf("%s\n", path);

    /* `-l` prints the *file*, so it is the one option that has to check there was a match
     * itself -- and for a while this did not, so `grep -l pattern a b` listed every file it
     * was given.  Which of the two this gets wrong is decided here and nowhere else. */
    if (opt_count) {
        /* Per file, and with the name once there is more than one file or the search was
         * recursive.  A single file gets a bare number, because `grep -c pattern logfile`
         * is read by scripts and `logfile:3` is not the answer they want.  A directory gets
         * `file:count` per file *including the zeros*, so the ones with nothing in them are
         * visible -- which is the question `-c` under a tree is usually asking. */
        if (opt_print_name)
            printf("%s:%ld\n", path, hits);
        else
            printf("%ld\n", hits);
    }

    return hits;
}

/* Search one path: a file, or a directory if `-r`. */
static long search_path(const char *path) {
    struct stat st;

    if (stat(path, &st) != 0) {
        report(path, errno);
        any_error = 1;
        return 0;
    }

    if (is_dir(&st)) {
        if (!opt_recursive) {
            fprintf(stderr, "grep: %s: Is a directory\n", path);
            any_error = 1;
            return 0;
        }

        return search_dir(path, 1);
    }

    /* Not a regular file: a device, a fifo, a socket.  Skipped quietly rather than reported,
     * because there is nothing in it to find, and a warning for every entry of `/dev` is
     * noise that teaches the reader to ignore this program's warnings. */
    if (!is_regular(&st))
        return 0;

    return search_file(path);
}

/* Search a directory and everything under it.  Returns how many lines matched in total. */
static long search_dir(const char *path, int depth) {
    DIR           *d;
    struct dirent *e;
    char         **names = NULL;
    size_t         n = 0;
    size_t         cap = 0;
    size_t         path_len = strlen(path);
    long           total = 0;

    if (depth > MAXDEPTH) {
        fprintf(stderr, "grep: %s: too deep (over %d levels)\n", path, MAXDEPTH);
        any_error = 1;
        return 0;
    }

    d = opendir(path);

    if (d == NULL) {
        report(path, errno);
        any_error = 1;
        return 0;
    }

    /* The whole directory is read into memory and closed before any of it is searched.
     *
     * Two reasons, and the second is the one that matters.  `readdir()` returns a pointer
     * into the `DIR`'s own buffer, so a name has to be copied before anything else runs.
     * And **this libc has sixteen directory slots in total**, so a `grep -r` that held each
     * directory open while descending would go sixteen levels deep and no further -- and
     * sixteen is not deep for a tree with symlinks, but it is a *global* limit on a program
     * that should not have one.  Reading the names and closing it makes depth cost memory
     * instead, which is a limit a caller can reason about. */
    for (;;) {
        e = readdir(d);

        if (e == NULL)
            break;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        if (n == cap) {
            size_t want = cap ? cap * 2 : 32;
            char **grown = realloc(names, want * sizeof(char *));

            if (grown == NULL)
                break;

            names = grown;
            cap = want;
        }

        names[n] = strdup(e->d_name);

        if (names[n] == NULL)
            break;

        n++;
    }

    closedir(d);

    for (size_t i = 0; i < n; i++) {
        /* Built by hand rather than with `snprintf()` into a fixed buffer, because a path can
         * be longer than any buffer this program could carry -- and truncating one produces
         * a path naming a *different* file, which is then searched and reported as if it
         * were the one asked for.
         *
         * The separator is not added when the directory already ends in one.  `grep -r pat
         * /etc/` is the same search as `grep -r pat /etc`, and without this the output names
         * `/etc//passwd`, which is a path that resolves correctly and prints differently
         * from the one the reader typed -- so a script comparing `grep -r` output against a
         * filename gets a difference that is only punctuation. */
        size_t nlen = strlen(names[i]);
        size_t sep  = (path_len > 0 && path[path_len - 1] == '/') ? 0 : 1;
        size_t need = path_len + sep + nlen + 1;
        char  *sub = malloc(need);

        if (sub != NULL) {
            memcpy(sub, path, path_len);

            if (sep != 0)
                sub[path_len] = '/';

            memcpy(sub + path_len + sep, names[i], nlen + 1);

            total += search_path(sub);
            free(sub);
        }

        free(names[i]);
    }

    free(names);

    return total;
}

int minx_main(unsigned long argc, char **argv) {
    int nargs = (int)argc;
    int argi = 1;
    int nfiles;

    /* Options are bundled, so `-rn` is `-r -n`.  `--` ends them, which is how a pattern
     * beginning with `-` is written, though `-e -x` says it more clearly. */
    while (argi < nargs && argv[argi][0] == '-' && argv[argi][1] != 0) {
        const char *p = argv[argi] + 1;

        if (strcmp(argv[argi], "--") == 0) {
            argi++;
            break;
        }

        if (strcmp(argv[argi], "--help") == 0) {
            printf("usage: grep [-rincvlqF] -e PATTERN [FILE...]\n"
                   "       grep [-rincvlqF] PATTERN [FILE...]\n"
                   "\n"
                   "The pattern is a fixed string, not a regular expression: there is no\n"
                   "regex engine here, and a partial one would fail silently.\n");
            return 0;
        }

        for (; *p != 0; p++) {
            switch (*p) {
            case 'r': opt_recursive = 1; break;
            case 'i': opt_ignore_case = 1; break;
            case 'v': opt_invert = 1; break;
            case 'n': opt_number = 1; break;
            case 'l': opt_files_with_matches = 1; break;
            case 'c': opt_count = 1; break;
            case 'q': opt_quiet = 1; break;
            case 'F': break;                  /* the only behaviour; said out loud */
            case 'e':
                if (argi + 1 >= nargs) {
                    fprintf(stderr, "grep: -e needs a pattern\n");
                    return 2;
                }

                if (npatterns < MAXPATTERNS)
                    patterns[npatterns++] = argv[argi + 1];

                argi++;
                break;
            default:
                fprintf(stderr, "grep: %s: unknown option\n", argv[argi]);
                return 2;
            }
        }

        argi++;
    }

    /* The first non-option word is the pattern, unless `-e` already supplied one. */
    if (npatterns == 0) {
        if (argi >= nargs) {
            fprintf(stderr, "usage: grep [-rincvlqF] PATTERN [FILE...]\n");
            return 2;
        }

        patterns[npatterns++] = argv[argi++];
    }

    nfiles = nargs - argi;

    /* One file, or standard input: the name is not printed, so `grep x file | wc -l` counts
     * lines and not `file:line`.  More than one, and it is.  Decided once, before any file
     * is opened -- which is why it is a flag rather than something `grep -r` works out
     * halfway down a tree. */
    /* A recursive search always names its files, even when one directory was named, because
     * its output is the output of many searches and a line that does not say where it came
     * from is a line nobody can act on.  GNU does the same, and `grep -r root /etc` is the
     * line in the Definition of Done. */
    opt_print_name = nfiles > 1 || opt_recursive;

    if (nfiles == 0) {
        long hits = search_stream(stdin, NULL);

        if (hits > 0)
            any_match = 1;

        if (opt_files_with_matches)
            printf("(standard input)\n");

        if (opt_count)
            printf("%ld\n", hits);
    } else {
        for (; argi < nargs; argi++)
            (void)search_path(argv[argi]);
    }

    if (fflush(stdout) != 0)
        return 2;

    /* Two failures and one answer, in the order a script cares about: something unreadable
     * is reported *and* the status is 2 even if something else matched, because a script
     * that checks only "did it match" must not be told yes when a third of the files were
     * missing. */
    if (any_error)
        return 2;

    return any_match ? 0 : 1;
}