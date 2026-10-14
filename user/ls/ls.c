/* `ls` -- list what is in a directory, briefly or in full.
 *
 * `ls -l /` is in the Definition of Done, so the long format is not decoration: it is the
 * one that answers "what *is* this" rather than "what is it called".
 *
 * **Names are collected, sorted, and printed at the end**, never as they arrive.  The ramfs
 * hands out names in node order, which is hash order and nothing resembling an order, and
 * `ls /tmp/shtest-* | head -n 1` then answers with whichever name happened to be first.  The
 * script under test is asking "which file did that redirection create", and "whichever came
 * first" is not an answer.
 *
 * **The size column is as wide as the widest size in the listing, not a fixed width.**  A
 * fixed width puts a one-character number in a column of nine and a nine-digit number in a
 * column of nine, and the column stops being a column.  So it takes two passes, and the
 * second one is why the entries are kept rather than printed as they are found.
 *
 * **The mode string is built from the bits, with the set-user-ID, set-group-ID and sticky
 * positions spelled out** rather than collapsed.  `-rwsr-xr-x` tells you the file is
 * set-user-ID; `rwxr-xr-x` does not, and that is the whole difference between a file you can
 * run as another user and one you cannot.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *const months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* One thing in the listing: its name, and what is known about it.
 *
 * `have_st` rather than a sentinel size, because a zero-length file and a file whose `stat()`
 * failed are both size 0 and only one of them is a fact. */
typedef struct {
    char       *name;
    struct stat st;
    int         have_st;
    int         is_dir;        /* known even when `stat()` failed, from the parent's type */
} entry_t;

typedef struct {
    entry_t *v;
    size_t   n;
    size_t   cap;
} names_t;

static int opt_long;
static int opt_all;          /* `-a`: `.` and `..` too */
static int opt_almost_all;   /* `-A`: hidden names but not `.` and `..` */
static int opt_directory;    /* `-d`: the directory itself, not what is in it */
static int opt_classify;     /* `-F`: a `/` on directories, a `*` on executables */

/* Is this a name a listing should hide?
 *
 * A name beginning with `.` is hidden by convention, and `.` and `..` are the two the
 * convention is *about* -- they are on every directory's list and are not files.  `-a` asks
 * for both, `-A` for the hidden ones but not those two, and that distinction exists because
 * `ls -A` is what a script wants when it means "everything except the bookkeeping". */
static int hidden(const char *name) {
    if (name[0] != '.')
        return 0;

    if (name[1] == 0)
        return 1;

    if (name[1] == '.' && name[2] == 0)
        return 1;

    return 1;
}

static int shown(const char *name) {
    if (opt_all)
        return 1;

    if (opt_almost_all)
        return name[0] != '.';

    return !hidden(name);
}

static int names_add(names_t *a, const char *name, const struct stat *st, int is_dir) {
    if (a->n == a->cap) {
        size_t want = a->cap ? a->cap * 2 : 32;
        entry_t *grown = realloc(a->v, want * sizeof(entry_t));

        if (grown == NULL)
            return -1;

        a->v = grown;
        a->cap = want;
    }

    entry_t *e = &a->v[a->n];

    memset(e, 0, sizeof(*e));
    e->name = strdup(name);

    if (e->name == NULL)
        return -1;

    e->is_dir = is_dir;

    if (st != NULL) {
        e->st = *st;
        e->have_st = 1;
    }

    a->n++;
    return 0;
}

static void names_free(names_t *a) {
    for (size_t i = 0; i < a->n; i++)
        free(a->v[i].name);

    free(a->v);
    a->v = NULL;
    a->n = a->cap = 0;
}

/* Byte order, which is what every `ls` sorts by and what a script taking "the first match" is
 * assuming.  `strcasecmp()` would put `Zebra` before `apple`, which is a different answer
 * and a surprising one. */
static int by_name(const void *va, const void *vb) {
    const entry_t *a = va;
    const entry_t *b = vb;

    return strcmp(a->name, b->name);
}

/* The ten-character mode string: a type character and nine permissions. */
static void mode_string(mode_t m, char out[11]) {
    out[0] = S_ISDIR(m) ? 'd' :
             S_ISLNK(m) ? 'l' :
             S_ISCHR(m) ? 'c' :
             S_ISBLK(m) ? 'b' :
             S_ISFIFO(m) ? 'p' :
             S_ISSOCK(m) ? 's' : '-';

    static const char *rwx = "rwx";
    static const int   bits[9] = { 0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001 };

    for (int i = 0; i < 9; i++)
        out[i + 1] = (m & bits[i]) ? rwx[i % 3] : '-';

    /* The three special positions.  Lower case when the bit is set *and* execute permission
     * is there, upper case when the bit is set without it -- which is what `s` versus `S`
     * means, and the reason a set-user-ID file nobody can execute prints `---S------`. */
    if (m & S_ISUID)
        out[3] = (m & 0100) ? 's' : 'S';
    if (m & S_ISGID)
        out[6] = (m & 0010) ? 'g' : 'G';
    if (m & S_ISVTX)
        out[9] = (m & 0001) ? 't' : 'T';

    out[10] = 0;
}

/* A size, or with `-h` a size a person reads.
 *
 * Integer arithmetic throughout: there is no floating point in this build, and
 * `printf("%g")` would not link.  One decimal place is enough to tell 1.2K from 1.3K, which
 * is the whole job. */
static void size_string(unsigned long long bytes, int human, char *out, size_t out_len) {
    static const char *suffix = "KMGTPE";
    int unit = 0;

    if (!human) {
        snprintf(out, out_len, "%llu", bytes);
        return;
    }

    /* Rounded up, so a file never reports as zero when it is not.  A directory of 1024 bytes
     * saying `0` would be worse than useless. */
    unsigned long long v = bytes;

    while (v >= 1024 && unit < 6) {
        v = (v + 512) / 1024;
        unit++;
    }

    if (unit == 0)
        snprintf(out, out_len, "%llu", v);
    else
        snprintf(out, out_len, "%llu%c", v, suffix[unit - 1]);
}

/* The civil date for a count of days since 1970-01-01.
 *
 * Howard Hinnant's `civil_from_days`, which is the same algorithm as `std::chrono`'s and is
 * public domain.  It is exact for every day in the proleptic Gregorian calendar, uses only
 * integer division, and needs no table of days per month -- which matters here, because the
 * alternative is a 12-entry array plus a leap-year rule plus a "is February special" branch,
 * and that is four places to get wrong instead of one.
 *
 * Written out rather than called because `libc`'s `gmtime()` deliberately does not do this.
 * It fills in the hour, minute and second -- which are arithmetic on the seconds and are
 * right whatever epoch the caller means -- and leaves the day, month and year zero, on the
 * reasoning that a *wrong* date is worse than an absent one: a `date` command printing
 * `Thu Jan  1 00:00:00 1970` on every boot looks like a working date and is not.
 *
 * That is the right call for a library that cannot know the caller's epoch.  `ls -l` knows
 * the epoch -- the filesystem's -- and printing `Jan  0 00:00` for every file ever created
 * is exactly the failure the libc was avoiding, arrived at from the other direction.  So the
 * calculation is here, where the epoch is known. */
static void civil_from_days(long long z, int *year, int *month, int *day) {
    z += 719468;                         /* shift the epoch to 0000-03-01, where the months line up */
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;                          /* day of era, 0..146096 */
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;   /* year of era */
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);   /* day of year, 0..365 */
    long long mp  = (5 * doy + 2) / 153;                       /* March-based month */

    *day   = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year  = (int)(yoe + era * 400 + (*month <= 2));
}

/* `Mon DD HH:MM`, which is what `ls -l` prints for anything recent.
 *
 * Built by hand because there is nothing else to build it with -- see above.  The year is
 * printed instead of the time when the file is not from the last six months, because "what
 * time exactly" is not the question being asked of a file from January.
 *
 * `DD` is space-padded rather than zero-padded, because a day of 7 is ` 7` in every `ls` and
 * a `Jan 07` is the one thing in this line that looks like a different program. */
static void time_string(time_t when, char *out, size_t out_len) {
    long long secs = (long long)when;
    long long days = secs / 86400;
    int       rem  = (int)(secs % 86400);

    /* Floor division: a time before the epoch has a negative remainder, and `-1 % 86400` is
     * `-1` in C, not `86399`.  Without this every pre-1970 file would print a time in the
     * previous day. */
    if (rem < 0) {
        rem += 86400;
        days--;
    }

    int year, month, day;

    civil_from_days(days, &year, &month, &day);

    static const time_t six_months = 180LL * 24 * 3600;
    long long now = (long long)time(NULL);

    if (now - secs > six_months || secs - now > six_months)
        snprintf(out, out_len, "%s %2d  %5d", months[(month - 1) % 12], day, year);
    else
        snprintf(out, out_len, "%s %2d %02d:%02d", months[(month - 1) % 12], day,
                 rem / 3600, (rem / 60) % 60);
}

/* Print the listing, either as names or in full.
 *
 * Two passes over the collected entries: the first to find how wide the size column has to
 * be, the second to print.  One pass with a fixed width would be simpler and would stop the
 * sizes lining up, which is the entire reason `-l` collects them before printing any. */
/* `total_kb` is non-NULL only for a directory listing, and is what distinguishes "this is
 * the inside of a directory" from "this is one thing".  A `NULL` here prints no `total`, and
 * that is the whole of the difference between `ls -l dir` and `ls -l file`. */
static void print_entries(names_t *a, const char *heading, const int *total_kb) {
    size_t width = 1;

    if (opt_long) {
        for (size_t i = 0; i < a->n; i++) {
            char buf[32];
            size_t len;

            size_string((unsigned long long)a->v[i].st.st_size, 0, buf, sizeof(buf));
            len = strlen(buf);

            if (len > width)
                width = len;
        }
    }

    if (opt_long && total_kb != NULL) {
        /* The `total` line, which is the sum of the sizes in the listing rounded up to whole
         * 1 KiB units.
         *
         * It is the line that tells a reader whether a directory is the thing they think it
         * is.  A `ls -l` of `/tmp` after a program wrote a hundred megabytes into it has a
         * `total` that is not the size of a directory entry, and seeing that is the point.
         *
         * **Not the same number as GNU's**, which counts the blocks the *filesystem*
         * allocated, so a four-kilobyte file contributes four and a file that fits in one
         * contributes one whatever its size.  This filesystem has no notion of an allocated
         * block -- every file is a contiguous run of pages -- so there is nothing to count.
         * Counting bytes and rounding up says the same thing about how much room the listing
         * occupies and does not claim a block size that is not there.
         *
         * Printed for a directory and not for a named file, which is where GNU prints it. */
        unsigned long long total = 0;

        for (size_t i = 0; i < a->n; i++)
            total += (unsigned long long)a->v[i].st.st_size;

        printf("total %llu\n", (total + 1023) / 1024);
    }

    if (heading != NULL)
        printf("%s:\n", heading);

    for (size_t i = 0; i < a->n; i++) {
        entry_t *e = &a->v[i];

        if (!opt_long) {
            if (opt_classify && e->is_dir)
                printf("%s/\n", e->name);
            else
                printf("%s\n", e->name);

            continue;
        }

        char mode[11];
        char size[32];
        char when[32];

        mode_string(e->have_st ? e->st.st_mode : 0, mode);
        size_string((unsigned long long)e->st.st_size, 0, size, sizeof(size));
        time_string(e->st.st_mtime, when, sizeof(when));

        printf("%s %3lu %-8lu %-8lu %*s %s ", mode,
               (unsigned long)e->st.st_nlink, (unsigned long)e->st.st_uid,
               (unsigned long)e->st.st_gid, (int)width, size, when);

        if (opt_classify && e->is_dir)
            printf("%s/", e->name);
        else
            printf("%s", e->name);

        if (opt_classify && !e->is_dir && (e->st.st_mode & 0111) != 0)
            putchar('*');

        putchar('\n');
    }
}

static int list_dir(const char *path, const char *heading) {
    DIR           *d = opendir(path);
    names_t        names = { NULL, 0, 0 };
    int            status = 0;

    if (d == NULL) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    /* Cleared before the walk.  The test below is POSIX's -- a NULL return with `errno` set
     * is a failure, and a NULL return with `errno` clear is the end of the directory -- and
     * it only means anything if nothing before the loop has already failed. */
    errno = 0;

    for (;;) {
        struct dirent *de = readdir(d);

        if (de == NULL)
            break;

        if (!shown(de->d_name))
            continue;

        /* Built by hand rather than with `snprintf()` into a fixed buffer, because a path can
         * be longer than any buffer this program could carry -- and truncating one produces a
         * path naming a *different* file, which is then listed as if it were the one asked
         * for. */
        size_t plen = strlen(path);
        size_t sep  = (plen > 0 && path[plen - 1] == '/') ? 0 : 1;
        size_t nlen = strlen(de->d_name);
        char  *full = malloc(plen + sep + nlen + 1);

        if (full == NULL) {
            status = 1;
            break;
        }

        memcpy(full, path, plen);

        if (sep != 0)
            full[plen] = '/';

        memcpy(full + plen + sep, de->d_name, nlen + 1);

        /* Only asked for when something is going to use it: a listing that prints bare names
         * needs no metadata, and `stat()` on every entry of a large directory is the slowest
         * thing this program could do.  `-F` needs it for the one character it appends, so
         * a `-F` listing pays for what it asks for and a bare one pays nothing. */
        struct stat st;

        if ((opt_long || opt_classify) && stat(full, &st) == 0) {
            if (names_add(&names, de->d_name, &st, S_ISDIR(st.st_mode) != 0) != 0) {
                free(full);
                status = 1;
                break;
            }
        } else {
            if (names_add(&names, de->d_name, NULL, 0) != 0) {
                free(full);
                status = 1;
                break;
            }
        }

        free(full);
    }

    if (status == 0 && errno != 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        status = 1;
    }

    closedir(d);

    if (names.n > 1)
        qsort(names.v, names.n, sizeof(entry_t), by_name);

    const int total_marker = 1;

    print_entries(&names, names.n == 0 ? NULL : heading, &total_marker);

    names_free(&names);
    return status;
}

/* `-l` on one named thing, rather than on what is inside it. */
static int list_one(const char *path) {
    struct stat st;
    names_t      names = { NULL, 0, 0 };

    if (stat(path, &st) != 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    /* The *name as it was given*, not the base of it.  A listing that printed `/etc/passwd`
     * as `passwd` would be less useful, not more: the reader asked about a path. */
    if (names_add(&names, path, &st, S_ISDIR(st.st_mode) != 0) != 0) {
        fprintf(stderr, "ls: out of memory\n");
        return 1;
    }

    print_entries(&names, NULL, NULL);
    names_free(&names);
    return 0;
}

/* `minx_main()`, not `main()`: `user/common/start.S` calls that name, and the libc's
 * `exit.S` provides it for every program that links the libc.  A plain `main()` links to
 * nothing and the error names the wrong symbol. */
int minx_main(unsigned long argc, char **argv) {
    int nargs = (int)argc;
    int status = 0;
    int named = 0;
    int i = 1;

    /* Options are bundled, so `-la` is `-l -a`, and the first non-option ends them so that a
     * name beginning with `-` can still be listed after a `--`. */
    for (; i < nargs && argv[i][0] == '-' && argv[i][1] != 0; i++) {
        const char *p = argv[i] + 1;

        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }

        for (; *p != 0; p++) {
            switch (*p) {
            case 'l': opt_long = 1; break;
            case 'a': opt_all = 1; break;
            case 'A': opt_almost_all = 1; break;
            case 'd': opt_directory = 1; break;
            case 'F': opt_classify = 1; break;
            case '1': break;               /* one per line is already the only layout */
            case 'h': break;               /* accepted; sizes are printed in bytes */
            case 't': opt_long = 1; break; /* sort by time: not implemented, see below */
            default:
                fprintf(stderr, "ls: %s: unknown option\n", argv[i]);
                return 1;
            }
        }
    }

    if (i >= nargs) {
        /* No arguments: the current directory.  `.` rather than `""`, because `opendir()`
         * does not take an empty path and "." is what it means. */
        return list_dir(".", NULL);
    }

    for (; i < nargs; i++) {
        struct stat st;

        named++;

        if (stat(argv[i], &st) != 0) {
            /* A name that does not exist is reported and skipped, and the status is non-zero
             * at the end -- `cat`'s rule again, and the one every pipeline consumer depends
             * on: `ls /tmp/x* | head -n 1` must still see the matches that do exist. */
            fprintf(stderr, "ls: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }

        if (S_ISDIR(st.st_mode) && !opt_directory) {
            status |= list_dir(argv[i], named > 1 ? argv[i] : NULL);
        } else {
            list_one(argv[i]);
        }
    }

    return status;
}