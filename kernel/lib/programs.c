/* minx - the program table.  See programs.h for what this is and why it exists. */
#include "programs.h"

#include <string.h>

/* Each program's image, as a `static const` array in a generated header.  They have to be
 * included here rather than referenced across translation units, because `static` in a
 * header means "in every unit that includes it", and the registry is the unit that uses
 * them.  The list is generated, so adding a program touches nothing here. */
#include "programs_list.h"

/* `stack_pages` is 8, and it is in the table rather than at each call site because it is a
 * property of the program, not of whoever started it: a program whose stack is too small
 * for its own recursion should fail the same way whoever runs it.  The table itself is
 * generated (tools/gen-programs-list.sh), so adding a program is a directory under user/
 * and nothing else -- see programs.h for why that matters. */
static const program_image_t programs[] = {
    MINX_PROGRAM_TABLE
};

/* Read as a count rather than kept beside the table, so adding an entry cannot leave the
 * two disagreeing -- which is the kind of thing that shows up as one program that cannot be
 * found and no message saying so. */
static const unsigned program_total =
    (unsigned)(sizeof(programs) / sizeof(programs[0]));

const program_image_t *program_find(const char *name) {
    if (name == NULL)
        return NULL;

    /* Compared by hand, stopping at either string's end, rather than `strcmp`: the caller
     * is a user program, and a name it built out of its own memory may be unterminated.
     * `strcmp` would read past the end of a short one, and `strncmp` needs a length that
     * this has no way of trusting either.
     *
     * The loop has to walk off the end of *both* strings before calling them equal, and
     * getting that wrong is silent: a version that only stopped when the table's name ran
     * out matched every character and then reported no match, so no program could be found
     * by name at all -- while `program_count()` and `program_by_index()` happily reported
     * that one existed. */
    for (unsigned i = 0; i < program_total; i++) {
        const char *candidate = programs[i].name;
        unsigned j = 0;

        while (candidate[j] != 0 && name[j] != 0 && candidate[j] == name[j])
            j++;

        if (candidate[j] == 0 && name[j] == 0)
            return &programs[i];
    }

    return NULL;
}

unsigned program_count(void) {
    return program_total;
}

const program_image_t *program_by_index(unsigned i) {
    return i < program_total ? &programs[i] : NULL;
}