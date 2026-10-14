#!/bin/sh
# The shell's lexer and parser, checked against `bash -n`.
#
# Two checks over a corpus of shell lines, and the second is the one that matters:
#
#   1. *Agreement*: for every line, does this parser accept exactly what bash accepts?  A
#      shell that refuses a valid script or accepts an invalid one is wrong before anything
#      runs, and this is the check that found the reserved-word and separator defects.
#
#   2. *Word order*, for the subset where "the right tree" has one unambiguous meaning: a
#      single simple command of plain words.  The words in the tree must be the words on the
#      line, in order.  Accept/reject cannot see a wrong tree -- `if true then A elif true
#      then B fi` parsed cleanly and still ran B -- and this can.
#
# Both need `bash`, because bash is the reference: an implementation nobody is changing, on
# the machine, answering the same question.  Without it this reports that it could not run
# and exits non-zero rather than silently passing.
#
# This is a fast loop, not a gate.  `make test` boots the real shell in the guest and is what
# a phase claim rests on.  This exists because five parser defects each cost a five-minute
# boot to reproduce, all of them showing the same unhelpful message in the guest:
#
#   - a keyword consumed twice, once by `accept()` and once by the parser it dispatched to
#   - a separator eaten by the parser of the *first* command of a list, so every `;` in a
#     script lost its right-hand side
#   - `$( )` splitting a word in half, so `n=$((n + 1))` reached the parser as eight tokens
#   - a reserved word recognised only when it was the first token on the line, so `then`,
#     `else` and `fi` were ordinary words
#   - an `elif` chain that overwrote itself as it was read and kept only the last branch
#
# `lex.c` and `parse.c` are linked here *unmodified*, against the host compiler and the host
# libc, so this tests the files the guest links and not a copy of them.  Nothing from the
# kernel is needed: `cmd_tree_dump()` prints with `printf()`, which the libc provides in the
# guest and the host provides here.
set -eu

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
corpus="$root/user/sh/parser-corpus.txt"
tree="$root/build/sh-parser-tree.txt"
bin="$root/build/sh-parser-test"

CC=${CC:-cc}

if ! command -v bash >/dev/null 2>&1; then
    echo "sh-parser-test: bash not found, and it is the reference for both checks" >&2
    exit 2
fi

mkdir -p "$root/build"

$CC -std=gnu11 -O1 -g -Wall -Wextra \
    -I"$root/user/sh" \
    "$here/sh-parser-test.c" \
    "$root/user/sh/lex.c" \
    "$root/user/sh/parse.c" \
    -o "$bin"

"$bin" "$corpus" > "$tree" 2>&1 || true

# `getline`-free POSIX shell, so each line is read once into a numbered file that awk can
# join against the tree.  Correlating by position is the whole point: a corpus line may
# contain quotes, `$`, backslashes and parentheses, and every one of them is a metacharacter
# in some quoting rule, so matching on the text silently fails on exactly the interesting
# lines.
awk '
    { print NR "\t" $0 }
' "$corpus" > "$root/build/sh-parser-corpus-numbered"

awk -v tree="$tree" '
    # Split the tree into one record per LINE, keeping the words of that record.
    /^LINE [0-9]+$/ { n = $2; next }
    /^VERDICT / {
        verdicts[n] = $2
        next
    }
    /^  word: / {
        words[n] = words[n] (words[n] == "" ? "" : " ") substr($0, 9)
        next
    }
    END { for (i = 1; i <= 3000; i++) if (i in verdicts) print i "\t" verdicts[i] "\t" words[i] }
' "$tree" > "$root/build/sh-parser-verdicts"

# ---- both checks ---------------------------------------------------------------
checked=0
disagree=0
order_checked=0
order_differs=0

while IFS="$(printf '\t')" read -r lineno line; do
    verdict=$(awk -F'\t' -v want="$lineno" '$1 == want { print $2; exit }' \
              "$root/build/sh-parser-verdicts")
    words=$(awk -F'\t' -v want="$lineno" '$1 == want { print $3; exit }' \
            "$root/build/sh-parser-verdicts")

    [ -z "$verdict" ] && verdict=none
    checked=$((checked + 1))

    if bash -n -c "$line" 2>/dev/null; then bash_says=yes; else bash_says=no; fi

    case "$verdict" in
        OK)              mine=yes ;;
        PARSEFAIL|LEXFAIL) mine=no ;;
        *)               mine=unknown ;;
    esac

    if [ "$mine" = "$bash_says" ]; then
        :
    else
        disagree=$((disagree + 1))
        printf 'sh-parser-test: bash=%s this=%s : %s\n' "$bash_says" "$mine" "$line"
    fi

    # Word order, only for the plain-command subset: no substitution, no quotes to strip,
    # no assignment, no metacharacter, no compound keyword.  Anything else needs a real
    # shell to know what "the words" are, and guessing here would make the test's own
    # answer the thing under test.
    # The metacharacter test.  `grep` with a bracket expression rather than a shell `case`:
    # a `case` pattern containing `(` is a syntax error in some shells and a pattern in
    # others, and the answer to that is not to depend on which shell runs the test.
    if printf '%s' "$line" | grep -q '[=$"'"'"';&|<>(){}]'; then
        continue
    fi

    case "$line" in
        if|then|else|fi|do|done|for|in|while|until|case|esac|elif) continue ;;
        if*|then*|else*|fi*|do*|done*|for*|in*|while*|until*|case*|esac*|elif*) continue ;;
    esac

    [ "$mine" = yes ] || continue

    # Both sides are normalised to single spaces with no trailing one, so the comparison is
    # about the words and not about which side happened to append a blank.
    want=$(printf '%s\n' "$line" | awk '{ for (i = 1; i <= NF; i++) { printf "%s", $i; if (i < NF) printf " " } }')
    got=$(printf '%s\n' "$words" | awk '{ for (i = 1; i <= NF; i++) { printf "%s", $i; if (i < NF) printf " " } }')
    order_checked=$((order_checked + 1))

    if [ "$got" != "$want" ]; then
        order_differs=$((order_differs + 1))
        printf 'sh-parser-test: words differ: %s\n  in the line: [%s]\n  in my tree: [%s]\n' \
               "$line" "$want" "$got"
    fi
done < "$root/build/sh-parser-corpus-numbered"

printf '\nsh-parser-test: %d line(s); bash agrees on accept/reject for %d, %d differ\n' \
       "$checked" "$((checked - disagree))" "$disagree"
printf 'sh-parser-test: %d plain command(s) checked for word order, %d differ\n' \
       "$order_checked" "$order_differs"

if [ "$disagree" -ne 0 ] || [ "$order_differs" -ne 0 ]; then
    exit 1
fi

echo "sh-parser-test: PASS"