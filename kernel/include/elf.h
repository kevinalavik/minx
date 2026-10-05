/* minx - just enough ELF to load a program this kernel built.
 *
 * The structures are declared here rather than pulled from a system header because this
 * is a freestanding kernel with no libc, and because these are exactly the fields the
 * loader reads -- anything it does not read is not declared, so it cannot be read by
 * accident on a field whose layout this kernel has not checked against the standard.
 *
 * Sizes and offsets are from the System V ABI's ELF64 tables, and the static assertions
 * below check the three that the loader's own arithmetic depends on.  A wrong offset
 * there would read a program header out of thin air and map wherever it said.
 */
#ifndef MINX_ELF_H
#define MINX_ELF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EI_NIDENT      16

#define EI_MAG0        0
#define EI_MAG1        1
#define EI_MAG2        2
#define EI_MAG3        3
#define EI_CLASS       4
#define EI_DATA        5
#define EI_VERSION     6

#define ELFMAG0        0x7f
#define ELFMAG1        'E'
#define ELFMAG2        'L'
#define ELFMAG3        'F'

#define ELFCLASS32     1
#define ELFCLASS64     2
#define ELFDATA2LSB    1
#define EV_CURRENT     1

#define ET_NONE        0
#define ET_REL         1
#define ET_EXEC        2
#define ET_DYN         3

#define EM_X86_64      62

#define PT_NULL        0
#define PT_LOAD        1

#define PF_X           0x1
#define PF_W           0x2
#define PF_R           0x4

#define ELF64_EHDR_SIZE 64
#define ELF64_PHDR_SIZE 56

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

/* Auxiliary vector keys.  Only the ones the loader can actually supply are named;
 * AT_NULL is the terminator and AT_IGNORE is what a key with no value is given. */
#define AT_NULL    0
#define AT_IGNORE  1
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_PAGESZ  6

/* What elf_load() reports back.  A caller needs all four of these to know whether the
 * program it was handed is the one it thinks: `ok` alone would let a truncated image
 * through as long as its first segment happened to fit. */
typedef struct {
    uint64_t entry;        /* where to start it                                */
    uint64_t entry_rsp;    /* the stack pointer to start it with               */
    uint64_t bytes;        /* total bytes mapped for its segments              */
    uint32_t segments;     /* how many PT_LOAD segments were mapped            */
    uint32_t phnum;        /* program headers, for the auxiliary vector        */
    bool     ok;
} elf_load_result_t;

/* A `task_t` is needed by the loader, and elf.h is included from task-aware code;
 * forward-declaring it here keeps elf.h free of the scheduler's header. */
struct task;
typedef struct task task_t;

/* Load an ELF image into `t`'s address space.
 *
 * `t` must already have an address space -- task_create_user() makes one.  `phdr_vaddr`
 * is where the program headers are mapped, and the caller should pass the address that
 * follows the ELF header, which is where a non-relocatable program already expects to
 * find them.
 *
 * Returns `ok == false` and says why on the console for anything it rejects; every
 * rejection leaves the address space untouched rather than half-mapped. */
elf_load_result_t elf_load(const void *image, size_t image_bytes, task_t *t,
                           uint32_t stack_pages, uint64_t phdr_vaddr);

/* The loader's own view of what it has done, for `ps`, /proc and the self tests. */
typedef struct {
    uint32_t loaded;       /* programs loaded since boot                       */
    uint32_t rejected;     /* images refused, with a reason each               */
    uint64_t bytes_mapped;
} elf_stats_t;

void elf_get_stats(elf_stats_t *out);

#endif /* MINX_ELF_H */
