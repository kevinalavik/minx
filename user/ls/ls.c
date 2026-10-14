/* `ls` -- list directory entries, or a file's name.
 *
 * One of the six programs `shtest`'s script needs: `echo "glob=$(ls /tmp/shtest-* | head -n
 * 1)"` asks for the first name a glob matched, which is how the script checks that a
 * redirection created the file it said it would.
 *
 * The *shell* does the globbing -- it expands `/tmp/shtest-*` into a list of words before
 * `ls` is run, and the shell's own matcher is what decides.  So `ls` here never expands
 * anything, and a pattern that reached it unexpanded is reported as no match rather than
 * listed as a file called `*`.  That is the right division: a shell that hands a program a
 * literal `*` has already decided the program should see the name `*`.
 *
 * One name per line, because the use here is a pipeline into `head`, and columns would put
 * several names on one line and make `head -n 1` return a fragment.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* What is printed for a name.  `.` and `..` are skipped, and a hidden name is listed or not
 * by the caller: a plain `ls` hides them, as every `ls` does, because a directory listing
 * with `.` and `..` in it is not what anybody means by "list". */
static int listed(const char *name, int show_hidden) {
    if (!show_hidden && name[0] == '.')
        return 0;

    return 1;
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* A growable array of names, so that the listing can be sorted before any of it is
 * printed.
 *
 * Printing as it reads would be simpler, and would list a directory in whatever order the
 * filesystem hands the entries back -- which is hash order for the ramfs and nothing
 * resembling an order at all.  `ls /tmp/shtest-* | head -n 1` then answers with whichever
 * name happened to be first, and the script under test is asking "which file did that
 * redirection create", where "whichever came first" is not an answer. */
typedef struct {
    char **v;
    size_t n;
    size_t cap;
} names_t;

static int names_add(names_t *a, const char *name) {
    if (a->n + 1 > a->cap) {
        size_t want = a->cap ? a->cap * 2 : 32;
        char **grown = realloc(a->v, want * sizeof(char *));

        if (grown == NULL)
            return -1;

        a->v = grown;
        a->cap = want;
    }

    a->v[a->n] = strdup(name);

    if (a->v[a->n] == NULL)
        return -1;

    a->n++;
    return 0;
}

static void names_free(names_t *a) {
    for (size_t i = 0; i < a->n; i++)
        free(a->v[i]);

    free(a->v);
    a->v = NULL;
    a->n = a->cap = 0;
}

static int list_dir(const char *path, int show_hidden) {
    DIR   *d = opendir(path);
    names_t names = { NULL, 0, 0 };
    int    status = 0;

    if (d == NULL) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        if (!listed(e->d_name, show_hidden))
            continue;

        if (names_add(&names, e->d_name) != 0) {
            fprintf(stderr, "ls: %s: out of memory\n", path);
            status = 1;
            break;
        }
    }

    if (status == 0 && errno != 0) {
        /* `readdir()` returning NULL with `errno` set is a failure and not an end of
         * directory, and the two are otherwise identical. */
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        status = 1;
    }

    closedir(d);

    /* Sorted by name, with `qsort()`'s byte order, which is what every `ls` sorts by and
     * what a script that takes "the first match" is assuming. */
    qsort(names.v, names.n, sizeof(char *), by_name);

    for (size_t i = 0; i < names.n; i++)
        printf("%s\n", names.v[i]);

    names_free(&names);
    return status;
}

/* `minx_main()`, not `main()`: `user/common/start.S` calls that name, and the libc's
 * `exit.S` provides it for every program that links the libc.  A plain `main()` links to
 * nothing and the error names the wrong symbol. */
int minx_main(unsigned long argc, char **argv) {
int  nargs = (int)argc;    /* `argc` arrives as an `unsigned long` because that is what the entry stub passes, and
     * every loop index here is an `int`.  They are the same value on this ABI; the cast
     * says so once rather than at every comparison, and says it because a `-Wsign-compare`
     * that has been silenced in the build is a real class of bug papered over rather than
     * answered. */
    
    /*  is  because that is what the entry stub passes, and comparing
     * it against an  index is a sign-compare warning that is right to be noisy about:
     * the two are the same on this ABI and the cast is here to say so once. */
    int  status = 0;
    int  show_hidden = 0;
    int  named = 0;
    int  i = 1;

    /* `-a` for hidden names.  The only option, because the only use is "give me a name",
     * and every other thing `ls` can do is a question about formatting. */
    for (; i < nargs; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            show_hidden = 1;
            continue;
        }

        if (argv[i][0] == '-' && argv[i][1] != 0) {
            fprintf(stderr, "ls: %s: unknown option\n", argv[i]);
            return 1;
        }

        break;
    }

    if (i >= nargs) {
        /* No arguments: the current directory.  `.` rather than `""`, because `opendir()`
         * does not take an empty path and "." is what it means. */
        return list_dir(".", show_hidden);
    }

    for (; i < nargs; i++) {
        struct stat st;

        named++;

        if (stat(argv[i], &st) != 0) {
            /* A name that does not exist is reported and skipped, and the status is
             * non-zero at the end -- `cat`'s rule again, and the rule every pipeline
             * consumer depends on: `ls /tmp/x* | head -n 1` must still see the matches
             * that do exist. */
            fprintf(stderr, "ls: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }

        if (S_ISDIR(st.st_mode))
            status |= list_dir(argv[i], show_hidden);
        else
            printf("%s\n", argv[i]);
    }

    (void)named;
    return status;
}