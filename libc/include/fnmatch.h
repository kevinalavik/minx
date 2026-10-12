/* minx - <fnmatch.h>
 *
 * Shell patterns: `*`, `?`, `[...]`.
 *
 * This is what `ls '*.c'` and `sh`'s `rm *.o` are built on, and the reason it exists as a
 * separate function rather than as something each program writes is that the flags are a
 * conversation.  `*` crossing a `/` is right for a shell and wrong for `find -name`, and the
 * two cannot both be right, so there are two functions' worth of behaviour behind one flag.
 *
 * Deliberately *not* a regular expression.  `fnmatch()` patterns have no alternation and no
 * repetition operator -- no `a|b`, no `{2,3}` -- and a program that reaches for them will
 * not find them, which is the correct answer: a pattern that is a superset of the shell's is
 * a pattern whose surprises are the programmer's.
 */
#ifndef MINX_FNMATCH_H
#define MINX_FNMATCH_H

/* A `\` escapes the next character.  Without this flag a backslash is an ordinary character,
 * which is what `find -name 'a\*b'` wants and what a shell user almost never wants. */
#define FNM_NOESCAPE 0x01

/* A `*` and a `?` stop at a `/`.
 *
 * With it, `*` stays inside one path component, which is what makes `find -name '*.c'` find
 * files rather than directories that happen to end in `.c`, and what stops a recursive
 * `rm -rf *` from treating a subdirectory as a match.
 *
 * This flag is about slashes only.  It says nothing about dotfiles, which is what
 * `FNM_PERIOD` is for -- and the two really are separate: `sh` matches a pattern against a
 * whole path and needs the dot rule without the slash rule. */
#define FNM_PATHNAME 0x02

/* A leading `.` must be matched by a leading `.` in the pattern, and a `.` anywhere else is
 * an ordinary character.
 *
 * This is *not* the default, which surprises people.  `fnmatch("*", ".hidden", 0)` matches
 * on every Unix, and it has to: a program that relied on the other behaviour would stop
 * matching files whose names happen to begin with a dot.  The protection of dotfiles is
 * somebody's job and it is not this function's -- `glob()` passes this flag always, and
 * `sh` never offers a dotfile to the matcher in the first place. */
#define FNM_PERIOD   0x04

/* Case-insensitive.  Provided for the programs that ask for it; the libc itself has no
 * opinion, and this is done by lowering both sides rather than by a second comparison in
 * every loop. */
#define FNM_CASEFOLD 0x08

/* Zero on a match, `FNM_NOMATCH` on no match.  `FNM_NOMATCH` is a distinct value rather
 * than 1 so that a future "matched but with a warning" has somewhere to go. */
#define FNM_NOMATCH 1

int fnmatch(const char *pattern, const char *string, int flags);

#endif /* MINX_FNMATCH_H */
