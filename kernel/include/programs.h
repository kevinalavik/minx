/* minx - the programs this kernel can start, by name.
 *
 * A program is named by a string, and something has to turn that string into bytes.  Until
 * there is a filesystem, the only bytes there are: each program's image is linked into the
 * kernel as an array, and this is the table that says which array belongs to which name.
 *
 * It exists for two reasons, and the second is the one that matters.
 *
 * The first is `exec`.  A program has to be able to say "run *that*" by name, and a kernel
 * that can only start the one program it was handed as a pointer cannot.
 *
 * The second is that one program doing everything stops being a test.  `hello` grew to the
 * point where its own growth was the bug: adding the signal checks pushed its text past
 * 4 KiB, and the loader had been filling every page of a segment with a copy of the first
 * one -- which one page of text never shows and two pages show as a `#GP` out of the `iretq`
 * that entered ring 3.  A test that lives inside another program's text is a test that
 * changes what it is testing.  Separate programs cost one directory each and nothing else.
 *
 * Phase 8 replaces the lookup with a real read of the initrd.  The loader does not change;
 * only where the bytes come from.
 */
#ifndef MINX_PROGRAMS_H
#define MINX_PROGRAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct program_image {
    const char *name;       /* what a program calls itself to `exec` */
    const void *image;      /* the ELF file, whole */
    size_t      bytes;
    uint32_t    stack_pages;
    const char *description;   /* for `ps` and for the loader's own messages */
} program_image_t;

/* The image for `name`, or NULL if this kernel has no such program.
 *
 * A `const char *` that may be NULL, because the caller is a user program and cannot be
 * trusted to pass a string at all -- the pointer is checked like any other. */
const program_image_t *program_find(const char *name);

/* How many programs are registered, and the i'th, for `ps` and for diagnostics. */
unsigned           program_count(void);
const program_image_t *program_by_index(unsigned i);

#endif /* MINX_PROGRAMS_H */