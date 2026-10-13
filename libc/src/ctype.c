/* minx - <ctype.h>.
 *
 * A table, not a chain of comparisons.  Every one of these functions is called in a loop by
 * a lexer or a scanner, and a table turns each call into one load and one test where a chain
 * of ranges would be six.  That is the whole reason this is a table and the reason it is
 * written out rather than generated: a program that classifies a few million characters
 * should not be paying for arithmetic to decide that a letter is a letter.
 *
 * The table is indexed by the `unsigned char` value, which is the rule that answers for the
 * character and not for whatever the caller happened to have in a register.  Every entry
 * takes and returns `int` because that is ISO C's signature, and every one of them casts
 * its argument first -- so a program passing a negative value gets the answer for the
 * character it actually meant rather than an answer about somewhere else in the encoding.
 *
 * **ASCII only, and that is a promise rather than a limitation.**  There is one locale and
 * it is "C"; `setlocale()` refuses anything else with NULL.  A `tolower()` that depended on
 * an environment variable would make a program's output depend on the machine it ran on,
 * and every "ASCII only" note elsewhere in this libc is saying the same thing for the same
 * reason.
 */

#include <string.h>

/* The bits.  Chosen so that a single table serves every function: a character is a letter
 * if it has `UPPER` or `LOWER`, a digit if it has `DIGIT`, and so on. */
#define CT_UPPER 0x01
#define CT_LOWER 0x02
#define CT_DIGIT 0x04
#define CT_SPACE 0x08
#define CT_PUNCT 0x10
#define CT_CNTRL 0x20
#define CT_BLANK 0x40
#define CT_XDIGT 0x80

static const unsigned char ctype_table[256] = {
    /* 0x00 */ CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL,
    /* 0x08 */ CT_CNTRL, CT_CNTRL | CT_SPACE, CT_CNTRL | CT_SPACE, CT_CNTRL | CT_SPACE,
                CT_CNTRL | CT_SPACE, CT_CNTRL, CT_CNTRL, CT_CNTRL,
    /* 0x10 */ CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL,
    /* 0x18 */ CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL, CT_CNTRL,
    /* 0x20 */ CT_SPACE | CT_BLANK, CT_PUNCT, CT_PUNCT, CT_PUNCT, CT_PUNCT, CT_PUNCT,
                CT_PUNCT, CT_PUNCT,
    /* 0x28 */ CT_PUNCT, CT_PUNCT, CT_PUNCT, CT_PUNCT, CT_PUNCT, CT_PUNCT, CT_PUNCT, CT_PUNCT,
    /* 0x30 */ CT_DIGIT | CT_XDIGT, CT_DIGIT | CT_XDIGT, CT_DIGIT | CT_XDIGT, CT_DIGIT | CT_XDIGT,
                CT_DIGIT | CT_XDIGT, CT_DIGIT | CT_XDIGT, CT_DIGIT | CT_XDIGT, CT_DIGIT | CT_XDIGT,
    /* 0x38 */ CT_DIGIT | CT_XDIGT, CT_DIGIT | CT_XDIGT, CT_PUNCT, CT_PUNCT, CT_PUNCT, CT_PUNCT,
                CT_PUNCT, CT_PUNCT,
    /* 0x40 */ CT_PUNCT, CT_UPPER | CT_XDIGT, CT_UPPER | CT_XDIGT, CT_UPPER | CT_XDIGT,
                CT_UPPER | CT_XDIGT, CT_UPPER | CT_XDIGT, CT_UPPER | CT_XDIGT, CT_PUNCT,
    /* 0x48 */ CT_UPPER, CT_UPPER, CT_UPPER, CT_UPPER, CT_UPPER, CT_UPPER, CT_UPPER, CT_UPPER,
    /* 0x50 */ CT_UPPER, CT_UPPER, CT_UPPER, CT_UPPER, CT_UPPER, CT_UPPER, CT_UPPER, CT_UPPER,
    /* 0x58 */ CT_UPPER, CT_UPPER, CT_UPPER, CT_PUNCT, CT_PUNCT, CT_PUNCT, CT_PUNCT, CT_PUNCT,
    /* 0x60 */ CT_PUNCT, CT_LOWER | CT_XDIGT, CT_LOWER | CT_XDIGT, CT_LOWER | CT_XDIGT,
                CT_LOWER | CT_XDIGT, CT_LOWER | CT_XDIGT, CT_LOWER | CT_XDIGT, CT_PUNCT,
    /* 0x68 */ CT_LOWER, CT_LOWER, CT_LOWER, CT_LOWER, CT_LOWER, CT_LOWER, CT_LOWER, CT_LOWER,
    /* 0x70 */ CT_LOWER, CT_LOWER, CT_LOWER, CT_LOWER, CT_LOWER, CT_LOWER, CT_LOWER, CT_LOWER,
    /* 0x78 */ CT_LOWER, CT_LOWER, CT_LOWER, CT_PUNCT, CT_PUNCT, CT_PUNCT, CT_PUNCT, 0,
    /* 0x80 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x90 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xa0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xb0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xc0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xd0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xe0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xf0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static unsigned char bit_of(int c) {
    /* The cast is the whole contract: a program passing a negative `char` gets the answer
     * for the character it meant, not an answer read from before the table. */
    return (unsigned char)(c & 0xff);
}

int isalnum(int c) {
    return (ctype_table[bit_of(c)] & (CT_UPPER | CT_LOWER | CT_DIGIT)) != 0;
}

int isalpha(int c) {
    return (ctype_table[bit_of(c)] & (CT_UPPER | CT_LOWER)) != 0;
}

int isblank(int c) {
    return (ctype_table[bit_of(c)] & CT_BLANK) != 0;
}

int iscntrl(int c) {
    return (ctype_table[bit_of(c)] & CT_CNTRL) != 0;
}

int isdigit(int c) {
    return (ctype_table[bit_of(c)] & CT_DIGIT) != 0;
}

int isgraph(int c) {
    unsigned char t = ctype_table[bit_of(c)];

    return t != 0 && (t & CT_SPACE) == 0 && (t & CT_CNTRL) == 0;
}

int islower(int c) {
    return (ctype_table[bit_of(c)] & CT_LOWER) != 0;
}

int isprint(int c) {
    unsigned char t = ctype_table[bit_of(c)];

    return t != 0 && (t & CT_CNTRL) == 0;
}

int ispunct(int c) {
    return (ctype_table[bit_of(c)] & CT_PUNCT) != 0;
}

int isspace(int c) {
    return (ctype_table[bit_of(c)] & CT_SPACE) != 0;
}

int isupper(int c) {
    return (ctype_table[bit_of(c)] & CT_UPPER) != 0;
}

int isxdigit(int c) {
    return (ctype_table[bit_of(c)] & CT_XDIGT) != 0;
}

int tolower(int c) {
    unsigned char t = ctype_table[bit_of(c)];

    return (t & CT_UPPER) != 0 ? c + ('a' - 'A') : c;
}

int toupper(int c) {
    unsigned char t = ctype_table[bit_of(c)];

    return (t & CT_LOWER) != 0 ? c - ('a' - 'A') : c;
}

/* `setlocale()`.
 *
 * "C" is installed, which it already was, and anything else is refused with NULL.  Returning
 * NULL is the whole point: a program that asked for a locale it did not get has to notice,
 * and a libc that quietly gave it the wrong one would leave it to find out by its output
 * being subtly different on another machine. */
char *setlocale(int category, const char *locale) {
    (void)category;

    if (locale == NULL || strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0)
        return (char *)"C";

    return NULL;
}
