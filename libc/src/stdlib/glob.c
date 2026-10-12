/* minx - `glob()`: expand a shell pattern into the names that match it.
 *
 * The shape of this is forced by the interface.  `glob()` hands back a `glob_t` the caller
 * owns and must `globfree()`, the list may be appended to across calls, and the names have
 * to be sorted unless the caller says not to -- which rules out returning them in any order
 * the filesystem happened to produce, because there is no way to tell "unsorted" from
 * "sorted and happened to arrive that way" afterwards.
 *
 * So: collect everything, then sort.  The alternative is a two-pass scheme that sorts each
 * directory's contribution and merges, which is what a large tree wants and what a program
 * matching patterns in one directory does not need.  Collecting first is also what makes
 * `GLOB_APPEND` possible without a special case, since the caller has the previous list and
 * the new one can simply be concatenated before the single sort.
 *
 * What is deliberately *not* here: recursion into subdirectories, and `**`.  `glob()` walks
 * exactly one directory per pattern, as it does everywhere else.  A recursive walk is
 * `find`'s job, and a `glob()` that descended would make `rm *` in a directory of
 * directories delete a tree.
 */
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* How a pattern is split into "the directory to read" and "the pattern to match against the
 * names in it".
 *
 * A pattern is either a plain name -- `*.c`, which means "read the current directory" -- or
 * a path with wildcards somewhere in its last component -- `src` plus `*.c`, or `a/Makefile`.
 *
 * The last component is the only one that can contain a wildcard; a `*` in an earlier one
 * would mean several directories, and this implementation does not do that (see the note at
 * the top).  Splitting on the last slash is therefore both the simplest split and the only
 * correct one. */
struct parts {
    const char *dir;        /* the directory to read, or NULL for the current one */
    const char *pattern;    /* the pattern for the names in it */
    int         prefix;     /* bytes to keep from `dir` when building a name */
};

/* `dir` and `pattern` point into `pattern` itself, so the caller must keep it alive; that
 * is why both are `const char *` into the caller's buffer rather than copies. */
static void split(const char *pattern, struct parts *out) {
    const char *slash = NULL;

    for (const char *p = pattern; *p != 0; p++)
        if (*p == '/')
            slash = p;

    if (slash == NULL) {
        out->dir     = NULL;
        out->pattern = pattern;
        out->prefix  = 0;
        return;
    }

    out->dir     = pattern;
    out->pattern = slash + 1;
    out->prefix  = (int)(slash - pattern) + 1;
}

/* The flags `fnmatch()` wants for a given `glob()`.
 *
 * The period rule is not optional here, and this is where it is decided: a leading `.` is
 * matched only by a leading `.` in the pattern, so `*` does not find `.profile`.  Without
 * it `cp * elsewhere` would copy every dotfile, and `rm *` would delete them. */
static int match_flags(int flags) {
    int f = FNM_PERIOD;

    /* `GLOB_MARK` means "treat `/` as an ordinary character", which is exactly the absence
     * of the slash rule. */
    if (flags & GLOB_MARK)
        return f;

    return f | FNM_PATHNAME;
}

/* One name, appended to the list.  The list grows by doubling.
 *
 * Both the count and the array are reallocated together, which is the whole reason this is
 * a function rather than three lines in the caller: a `realloc()` that fails after the count
 * was incremented leaves a list that claims more entries than it has, and every later read
 * walks off the end of the array. */
static int push(glob_t *g, char *name) {
    if (g->gl_pathc + 1 >= g->gl_offs) {
        size_t want = g->gl_offs ? g->gl_offs * 2 : 16;
        char **grown = realloc(g->gl_pathv, want * sizeof(char *));

        if (grown == NULL) {
            free(name);
            return -1;
        }

        g->gl_pathv = grown;
        g->gl_offs  = want;
    }

    g->gl_pathv[g->gl_pathc++] = name;
    g->gl_pathv[g->gl_pathc]   = NULL;     /* kept NULL-terminated, as every Unix does */

    return 0;
}

/* Sort.
 *
 * `strcmp`, because that is the order `ls` prints in and therefore the order a user
 * recognises: a byte comparison, so `Z` sorts before `a`, which is not "alphabetical" to a
 * person and is exactly what every Unix does.  `GLOB_NOSORT` exists for the callers who
 * want the directory's own order instead. */
static int cmp_name(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *out) {
    struct parts p;
    DIR         *d;

    if (pattern == NULL || out == NULL) {
        errno = EINVAL;
        return GLOB_ABORTED;
    }

    if (out->gl_pathv == NULL && out->gl_pathc == 0 && out->gl_offs == 0) {
        /* A `glob_t` the caller has not zeroed is not something this function can guess
         * about -- and `GLOB_APPEND` is defined in terms of the caller zeroing it first, so
         * an unzeroed one is a bug in the caller rather than something to paper over. */
        if (flags & GLOB_APPEND) {
            errno = EINVAL;
            return GLOB_ABORTED;
        }

        out->gl_flags = flags;
    }

    split(pattern, &p);

    /* A pattern that ends in `/` names a directory and not its contents, which is the
     * difference between listing a directory's contents and listing the directory itself,
     * and is checked here rather than being left to produce a name that does not exist. */
    if (*p.pattern == 0) {
        errno = ENOENT;
        return GLOB_NOMATCH;
    }

    if (p.dir == NULL) {
        d = fdopendir(open(".", O_RDONLY | O_DIRECTORY));
    } else {
        char buf[512];
        size_t n = (size_t)p.prefix;

        if (n >= sizeof(buf)) {
            errno = ENAMETOOLONG;
            return GLOB_NOSPACE;
        }

        memcpy(buf, p.dir, n);
        buf[n] = 0;

        d = opendir(buf);
    }

    if (d == NULL) {
        /* A directory that is not there is not a failure.  A glob under a directory that
         * does not exist says "no such file" about the *directory*, not about every name in
         * it, and turning ENOENT into an error here would make that unprintable. */
        if (errno == ENOENT || errno == ENOTDIR)
            return GLOB_NOMATCH;

        if (errfunc != NULL && errfunc(pattern, errno) != 0)
            return GLOB_ABORTED;

        if (flags & GLOB_ERR)
            return GLOB_ABORTED;

        return GLOB_NOMATCH;
    }

    int f = match_flags(flags);
    int matched = 0;

    for (struct dirent *e = readdir(d); e != NULL; e = readdir(d)) {
        if (fnmatch(p.pattern, e->d_name, f) != 0)
            continue;

        matched++;

        /* The name, with the directory in front of it.  `p.prefix` is the length of the
         * directory *including* its trailing slash, so this is a plain concatenation -- and
         * for the no-directory case it is the name on its own. */
        size_t dlen = (size_t)p.prefix;
        size_t nlen = strlen(e->d_name);
        char  *name = malloc(dlen + nlen + 1);

        if (name == NULL) {
            closedir(d);
            return GLOB_NOSPACE;
        }

        if (dlen != 0)
            memcpy(name, p.dir, dlen);

        memcpy(name + dlen, e->d_name, nlen + 1);

        if (push(out, name) != 0) {
            closedir(d);
            return GLOB_NOSPACE;
        }
    }

    closedir(d);

    /* Nothing matched.  The answer depends on `GLOB_NOCHECK`, and the other answer is
     * "nothing matched" -- a success with an empty list, not an error, because `sh` needs to
     * be able to tell "no files" from "something went wrong". */
    if (!matched && (flags & GLOB_NOCHECK)) {
        char *name = strdup(pattern);

        if (name == NULL)
            return GLOB_NOSPACE;

        if (push(out, name) != 0)
            return GLOB_NOSPACE;
    }

    if (!(flags & GLOB_NOSORT) && out->gl_pathc > 1)
        qsort(out->gl_pathv, out->gl_pathc, sizeof(char *), cmp_name);

    out->gl_flags = flags;

    return GLOB_NOMATCH;
}

void globfree(glob_t *g) {
    if (g == NULL)
        return;

    for (size_t i = 0; i < g->gl_pathc; i++)
        free(g->gl_pathv[i]);

    free(g->gl_pathv);

    g->gl_pathv = NULL;
    g->gl_pathc = 0;
    g->gl_offs  = 0;
}
