/* minx - <ctype.h>
 *
 * The character classification and conversion functions.
 *
 * All of them take an `int` and all of them answer for the `unsigned char` value it
 * represents, which is the rule that stops a program passing a negative value getting an
 * answer about a character at the other end of the encoding.  The seven functions take the
 * argument as `int` and this libc's implementations cast it -- so the contract is kept here
 * rather than being each implementation's problem.
 *
 * **The C locale is the only locale.**  `setlocale()` exists and does nothing but install the
 * C locale, which is what a program compiled for this system should assume: a `tolower()`
 * that depends on an environment variable would make a program's output depend on the
 * machine it ran on, and every other function here that has an "ASCII only" note is saying
 * the same thing for the same reason.
 *
 * The table is written out rather than computed, because it is 128 entries of a constant and
 * a program that classifies a few million characters should not pay for arithmetic.
 */
#ifndef MINX_CTYPE_H
#define MINX_CTYPE_H

int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);

/* The locale.  A real libc returns the *previous* locale so that a program can put it back;
 * this one always returns "C", because there is no other to have had.  Setting it to
 * anything else is refused with NULL, rather than accepted and ignored -- a program that
 * asks for a Turkish locale and is quietly given English would have to find the bug
 * somewhere else entirely. */
#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5

char *setlocale(int category, const char *locale);

#endif /* MINX_CTYPE_H */
