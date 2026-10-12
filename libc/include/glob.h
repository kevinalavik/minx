/* minx - <glob.h>
 *
 * Expanding a pattern into the names that match it, sorted.
 *
 * The reason this is a function rather than something `sh` does inline is that the caller
 * almost never wants *all* of it.  `ls *.c` wants the names and nothing else, `find` wants
 * to descend into each of them as it finds them, and `rm *` wants the names and then to
 * unlink them.  Three callers, three of the four flags, and a fourth caller that wants the
 * paths to be absolute.  The function takes the flags and returns the list; what a program
 * then does with the list is its own business.
 *
 * The list is in `glob_t`, which the caller owns once `glob()` returns and must hand back
 * with `globfree()`.  That is the interface every Unix has, and it is here rather than a
 * plain array because `glob_t` is also where `GLOB_APPEND` puts the results of a second
 * call -- so the storage has to be reachable from outside the function.
 */
#ifndef MINX_GLOB_H
#define MINX_GLOB_H

#include <dirent.h>
#include <stddef.h>

/* Return codes.  Zero is success; the other three are not errno values, because they say
 * what went wrong with the *pattern* rather than with the system. */
#define GLOB_NOMATCH 0   /* not an error: nothing matched, and that is a valid answer */
#define GLOB_NOSPACE 1   /* out of memory */
#define GLOB_ABORTED 2   /* an error reading a directory, with errno set */

/* Put the new matches after the old ones rather than replacing them.  The caller must have
 * zeroed the `glob_t` first, or must call `globfree()` before it -- `GLOB_APPEND` on a
 * `glob_t` that already holds a list appends to it, which is the point. */
#define GLOB_APPEND   0x01

/* Do not sort.  Provided because `find` does not want to: it walks the matches in whatever
 * order the directory gives them, and a directory order is the order that puts the entries
 * of one directory together. */
#define GLOB_NOSORT   0x02

/* Report a directory that could not be read instead of skipping it.  Without this a
 * permission error in a subdirectory is indistinguishable from an empty one, and `ls -R`
 * over a tree with one unreadable directory in it silently prints less than the tree
 * contains.
 *
 * Named `GLOB_ERR` because that is the name every Unix uses, even though it reads like a
 * return code rather than like a flag -- and it is not one of the return codes, which is why
 * the value here has nothing to do with `GLOB_ABORTED` above. */
#define GLOB_ERR      0x04

/* Match `/` as an ordinary character rather than as a directory separator.  This is what
 * turns `*` into "everything below here" instead of "everything in here", and it is the
 * flag `find` passes. */
#define GLOB_MARK     0x08

/* With `GLOB_NOCHECK`: if the pattern matches nothing, return the pattern itself as though
 * it had been a single name.
 *
 * This is the rule that makes `rm *.o` in a directory with no object files say
 * "rm: *.o: No such file or directory" rather than "rm: missing operand" -- the shell passes
 * the pattern through untouched when nothing matched, so the program sees the name the user
 * typed and can report it.  A `glob()` that returned an empty list would leave the program
 * with nothing to say, which is why this behaviour is the default on every Unix.
 *
 * Note what is *not* a flag: a hidden file is not matched by `*` unless the pattern names
 * the dot, on every Unix and here.  There is no `GLOB_MATCHHIDDEN` because the rule is not
 * optional; `GLOB_NOCHECK` is how you ask for the pattern back unchanged. */
#define GLOB_NOCHECK  0x10

/* One result. */
typedef struct {
    size_t gl_pathc;       /* how many */
    char **gl_pathv;       /* the names, each a separate allocation */
    size_t gl_offs;        /* reserved; the byte offset into the current directory */
    int    gl_flags;       /* the flags the list was built with */
} glob_t;

/* `GLOB_ABORTED` and `GLOB_NOSPACE` may leave something in the list, so a program that
 * gives up on the error still has to call `globfree()` on a `glob_t` it zeroed first. */
int   glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *out);
void  globfree(glob_t *out);

#endif /* MINX_GLOB_H */
