/* The shell's lexer and parser, on the host.
 *
 * Reads one shell line per line from a corpus and reports, for each, whether it parsed and
 * if so the tree that came out.  Nothing about the shell is stubbed: `lex.c` and `parse.c`
 * are the very files the guest links, compiled by the host compiler against the host libc.
 * `kprintf()` is the one substitution, and it is `printf()`.
 *
 * Why this exists, and what it was worth:
 *
 * Five separate defects in this parser -- a keyword consumed twice, a separator eaten by
 * the wrong sub-parser, `$( )` splitting a word in half, a reserved word recognised only as
 * the first token on a line, and an `elif` chain that kept only its last branch -- all
 * presented in the guest as the same sentence ("something after the end of the command")
 * for every compound construct.  Each one cost a five-minute QEMU boot to reproduce, and
 * the fastest of them was found by reading the disassembly of a shell that had already run
 * most of a script correctly.
 *
 * Here the same corpus is checked against `bash -n` in well under a second, and against an
 * implementation that is not being changed.  `tools/sh-parser-test.sh` drives it; this
 * program only does the parsing and the printing, so that the comparison logic -- which is
 * where the judgement is -- is in one readable place.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh.h"

int main(int argc, char **argv) {
    int         rc = 0;
    const char *path;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <corpus-file>\n", argv[0]);
        return 2;
    }

    path = argv[1];

    FILE *f = fopen(path, "r");

    if (f == NULL) {
        fprintf(stderr, "%s: cannot read %s\n", argv[0], path);
        return 2;
    }

    char line[1024];
    int  lineno = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;

        if (len == 0)
            continue;

        lineno++;

        tokens_t toks;
        const char *err = NULL;

        printf("=== %s\n", line);

        /* Numbered, so that the driving script can correlate a verdict with a line of the
         * corpus by position rather than by matching its text.  Matching on the text is
         * what this replaces: a corpus line may contain quotes, `$`, backslashes and
         * parentheses, and every one of them is a metacharacter in some quoting rule, so a
         * `case "$line" in` pattern or a `-v pat=` awk variable silently failed to match and
         * the line was reported as having no verdict at all. */
        printf("LINE %d\n", lineno);

        if (lex_line(line, &toks) != 0) {
            printf("VERDICT LEXFAIL\n");
            rc = 1;
            continue;
        }

        printf("VERDICT pending\n");

        for (int i = 0; i < toks.n; i++)
            printf("tok %d kind=%d %s\n", i, (int)toks.v[i].kind,
                   toks.v[i].text != NULL ? toks.v[i].text : "-");

        cmd_t *c = parse_program(&toks, &err);

        if (c == NULL) {
            printf("VERDICT PARSEFAIL %s\n", err != NULL ? err : "(no message)");
            rc = 1;
            tokens_free(&toks);
            continue;
        }

        printf("VERDICT OK\n");
        cmd_tree_dump(c, 0);
        cmd_free(c);
        tokens_free(&toks);
    }

    fclose(f);
    return rc;
}