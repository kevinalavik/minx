/* minx - ELF64 program loader.
 *
 * Small on purpose: enough of ELF to load a static executable this kernel built, and
 * nothing else.  No dynamic linking, no TLS, no interpreter, no relocations -- the
 * programs that exist at this stage are freestanding and fully linked, and a loader
 * that half-implemented the dynamic case would turn a missing feature into a crash at
 * run time instead of a message at load time.
 *
 * Each of these is checked rather than assumed:
 *
 *  - **The header is checked field by field before anything is mapped.**  An image that
 *    claims a different class, a different endianness or a different machine is rejected
 *    before a byte of it is treated as an address, because each of those is a case where
 *    mapping anyway would eventually execute the file's contents as instructions.
 *
 *  - **The program headers are mapped too**, at the address the file says follows the
 *    header.  A loader that puts AT_PHDR in the auxiliary vector has to have mapped
 *    something, and claiming to supply an entry that points at nothing is worse than not
 *    supplying it.
 *
 *  - **The part of a segment past its file contents is mapped and zeroed.**  That is
 *    `.bss`, and a page that starts as whatever the recycled frame held is a program
 *    reading another program's memory.
 *
 *  - **The stack is built to the ABI, all of it** -- argc, argv, the NULL, envp, the
 *    NULL, the auxiliary vector, AT_NULL, and then the strings.  A program that reads
 *    its arguments is how a broken loader is noticed at all: one that only ever calls
 *    write() runs perfectly happily on a stack that is merely wrong.
 */
#include "elf.h"

#include "kprintf.h"
#include "pmm.h"
#include "string.h"
#include "syscall.h"
#include "task.h"
#include "vmm.h"

#define ALIGN16(x)  (((x) + 15ull) & ~15ull)

/* Where the program break starts, and the first address above it that is still fair
 * game.  Both clear of where the segments and the stack go, so a program that grows its
 * heap cannot walk into either -- until phase 7 gives it a real allocator, which is the
 * point at which brk() stops being ENOMEM. */
#define BRK_START         (USER_IMAGE_BASE + 0x02000000ull)
#define BRK_END           (USER_IMAGE_BASE + 0x04000000ull)

static bool in_range(size_t total, uint64_t off, size_t len) {
    if (off > total)
        return false;
    return len <= total - off;
}

/* Copy one segment into freshly allocated user pages.
 *
 * A page at a time, because `p_filesz` and `p_memsz` differ and the difference is
 * `.bss`.  The writes go through the direct map, which is a supervisor write to a user
 * page -- exactly what CR4.SMAP would forbid, and deliberately not enabled yet for the
 * reason task_create_user() records. */
/* Map a segment's pages into `tables`, not into the address space of whoever happens to
 * be running.
 *
 * `vmm_map_user()` maps through vmm_tables(), which is the *running* task's tables.  For
 * everything else in the kernel that is the right answer, and it is the reason nothing
 * has to pass an address space around.  It is the wrong answer here: a program's image
 * is loaded into the new task's address space, long before that task has ever been
 * scheduled, so it goes into the loader's own tables instead -- and then the program
 * starts on page tables that have no code in them and faults on its first instruction,
 * with a loader that reported success.
 *
 * The copy side is done through the direct map of the frame just allocated, which is
 * reachable from any address space, so only the mapping needed the address space passed
 * in. */
static bool load_segment(page_table_t *tables, const uint8_t *image, uint64_t virt,
                         uint64_t filesz, uint64_t memsz, bool writable) {
    if (memsz == 0)
        return true;

    if (filesz > memsz)
        return false;

    size_t span = (size_t)ALIGN_UP(memsz, page_size);

    for (size_t off = 0; off < span; off += page_size) {
        uint64_t phys = vmm_user_page_alloc();

        /* Every one of these used to be a bare `return false`, so a program that would
         * not load produced one line saying it would not load and nothing about why.  A
         * loader that fails silently is indistinguishable from one that was never asked
         * to load anything. */
        if (phys == 0) {
            kprintf("elf: out of frames mapping %#lx+%lu\n", (unsigned long)(virt + off),
                    (unsigned long)memsz);
            return false;
        }

        if (vmm_map_into(tables, virt + off, phys, page_size, writable) != 0) {
            kprintf("elf: could not map %#lx into %p\n",
                    (unsigned long)(virt + off), (void *)tables);
            vmm_user_page_free(phys);
            return false;
        }

        uint8_t *dst = (uint8_t *)phys_to_virt(phys);
        size_t from_file = 0;

        if (filesz > off)
            from_file = (size_t)((filesz - off < page_size) ? filesz - off : page_size);

        if (from_file != 0)
            __builtin_memcpy(dst, image + off, from_file);

        /* The tail stays zero, which vmm_user_page_alloc() arranged.  Written out
         * explicitly because "the recycled frame happened to be zero" is not a property
         * anything guarantees. */
        if (from_file < page_size)
            memset(dst + from_file, 0, page_size - from_file);
    }

    return true;
}

/* Build the initial stack.
 *
 * Laid out from the top down, because the ABI's block is fixed-size and the strings
 * have to go above it.  `base`/`cap` are the task's own stack pages through the direct
 * map; the layout from the bottom of the block upwards is:
 *
 *     sp+0    argc
 *     sp+8    argv[0]          -> the path string, above the block
 *     sp+16   argv[1] = NULL
 *     sp+24   envp[0] = NULL
 *     sp+32   auxv[] , ending in AT_NULL
 *             ...alignment...
 *             the argument and environment strings
 *
 * which is the order the ABI specifies, and putting `sp` at the bottom of it is what
 * makes `_start` fall out with argc in RDI and argv at [rsp]. */
static bool build_stack(task_t *t, uint64_t entry, const char *path,
                        uint32_t stack_pages, uint64_t *entry_rsp) {
    uint64_t top = USER_STACK_TOP;

    /* The kernel alias of the stack, which is the only way to reach it: the pages are
     * mapped in this task's address space alone, so there is nothing in the kernel's own
     * tables to translate `bottom` through.  `virt_to_phys()` cannot be used on it --
     * that subtracts the direct-map offset, so applied to a user address it wraps, and
     * adding the offset back lands in the hole just below the kernel image.  Which is
     * where this used to write: the argv/envp/auxv block went below the kernel image and
     * the real stack was never zeroed, so the program was entered with a stack pointer
     * into a hole. */
    if (t->user_stack_phys == 0) {
        kprintf("elf: task %lu has no recorded user stack frame\n",
                (unsigned long)t->id);
        return false;
    }

    uint8_t *base = (uint8_t *)phys_to_virt(t->user_stack_phys);
    uint64_t cap = (uint64_t)stack_pages * page_size;

    /* The top of the stack belongs to the two frames, not to the program.
     *
     * task_create_user() put the scheduler's saved context and the ring-3 return frame at
     * the very top of the stack, and this function lays out argc/argv/envp/auxv and the
     * argument strings from the top down.  Those two layouts overlap completely: the
     * strings landed at top-16 and the auxiliary vector at top-144..top-16, straight over
     * the saved context (top-96..top-40) and over CS, RFLAGS and SS.  Only RIP and RSP are
     * written again afterwards, by the caller, so the frame ended up half right: `iretq`
     * popped the right entry point with the CPU's own CS and a selector built out of two
     * words of auxiliary vector, which is a #GP before the program's first instruction.
     *
     * Nothing here is allowed to reach the frames, so the region below them is what gets
     * laid out and zeroed, and the program's stack pointer starts inside it. */
    uint64_t frames = USER_FRAME_BYTES + USER_CTX_BYTES;
    uint64_t usable = cap - frames;

    if (usable == 0 || usable > cap) {
        kprintf("elf: a %u-page stack is too small for the initial frames\n",
                (unsigned)stack_pages);
        return false;
    }

    /* The alias has to be the frame that is actually mapped at the bottom of this stack,
     * or everything below writes to one region and the program reads another.  The
     * difference would show up as a fault in the program's first stack access with
     * nothing pointing back here, so it is checked against the task's own page table --
     * which is the only place the mapping is written down.  It cannot be checked through
     * vmm_translate(), which translates through the *running* task's tables, and the
     * loader runs long before this task has ever been scheduled. */
    pte_t leaf = pt_get(t->tables, top - cap);

    if (leaf == 0 || pte_phys(leaf) != t->user_stack_phys) {
        kprintf("elf: the frame at the bottom of the stack is %s, expected %#lx\n",
                leaf == 0 ? "not mapped" : "the wrong frame",
                (unsigned long)t->user_stack_phys);
        return false;
    }

    size_t path_len = strnlen(path, 4096);

    /* argc, argv[0], the argv NULL, the envp NULL, then one auxv entry each for
     * AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_BASE, AT_ENTRY and AT_NULL. */
    struct { uint64_t key, value; } auxv[8];
    unsigned naux = 0;

    auxv[naux].key = AT_PHDR;   auxv[naux++].value = 0;      /* filled in by the caller */
    auxv[naux].key = AT_PHENT;  auxv[naux++].value = ELF64_PHDR_SIZE;
    auxv[naux].key = AT_PHNUM;  auxv[naux++].value = 0;
    auxv[naux].key = AT_PAGESZ; auxv[naux++].value = page_size;
    auxv[naux].key = AT_BASE;   auxv[naux++].value = 0;
    auxv[naux].key = AT_ENTRY;  auxv[naux++].value = entry;
    auxv[naux].key = AT_NULL;   auxv[naux++].value = 0;

    uint64_t block_bytes = 8           /* argc                       */
                         + 8           /* argv[0]                    */
                         + 8           /* argv NULL                  */
                         + 8           /* envp NULL                  */
                         + (uint64_t)naux * 16;

    uint64_t strings_bytes = ALIGN16(path_len + 1);

    /* 256 bytes of slack below the block so a program's first few frames cannot walk
     * off the bottom of the stack into whatever the allocator gave us. */
    if (block_bytes + strings_bytes + 256 > usable)
        return false;

    memset(base, 0, usable);

    /* The strings, at the top of the region the program owns. */
    uint8_t *p = base + usable - strings_bytes;
    memcpy(p, path, path_len);
    p[path_len] = 0;

    /* The *addresses the program will see* are offsets into its own stack, not offsets
     * into the kernel's alias of it.  These two were computed from the alias, so the
     * program was entered with a stack pointer into the direct map and an argv[0] pointer
     * into the direct map -- and `iretq` loading a kernel address into RSP for a ring-3
     * task is a #GP before the first instruction, with nothing to say which field of a
     * loader was wrong.
     *
     * So the alias is used only to *write*; everything the program will read back is
     * expressed against the user address of the stack. */
    uint64_t stack_base = top - cap;
    uint64_t argv0_addr = stack_base + (uint64_t)(p - base);

    /* The block, ending just below the strings. */
    uint8_t *q = base + usable - strings_bytes;
    q -= (uint64_t)naux * 16;
    memcpy(q, auxv, (size_t)naux * 16);

    q -= 8;  *((uint64_t *)q) = 0;          /* envp[0] == NULL  */
    q -= 8;  *((uint64_t *)q) = 0;          /* argv[1] == NULL  */
    q -= 8;  *((uint64_t *)q) = argv0_addr; /* argv[0]          */
    q -= 8;  *((uint64_t *)q) = 1;          /* argc == 1        */

    /* The ABI wants 16-byte alignment at the first call, so the stack pointer is
     * aligned here rather than where the arithmetic happens to land. */
    uint64_t sp = stack_base + (uint64_t)(q - base);
    sp &= ~15ull;

    /* And it has to land inside the program's own region: below the frames, and not off
     * the bottom of the stack.  The frames occupy [top - frames, top), so the region is
     * everything under top - frames. */
    if (sp > top - frames || sp < top - cap) {
        kprintf("elf: the initial stack pointer %#lx is outside the region the "
                "program owns ([%#lx, %#lx))\n", (unsigned long)sp,
                (unsigned long)(top - cap), (unsigned long)(top - frames));
        return false;
    }

    t->brk = BRK_START;
    t->exit_code = -1;

    *entry_rsp = sp;
    return true;
}

elf_load_result_t elf_load(const void *image, size_t image_bytes, task_t *t,
                           uint32_t stack_pages, uint64_t phdr_vaddr) {
    elf_load_result_t r = { 0, 0, 0, 0, 0, false };
    const uint8_t *base = (const uint8_t *)image;

    /* The whole header has to be inside the image before a single field is read.  The
     * image is the kernel's own copy so it cannot fault, but a truncated *file* inside
     * it can, and reading past the end of the copy reads whatever the allocator placed
     * next. */
    if (image == NULL || image_bytes < ELF64_EHDR_SIZE)
        return r;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;

    if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        kprintf("elf: not a little-endian 64-bit ELF (class %u, data %u)\n",
                eh->e_ident[EI_CLASS], eh->e_ident[EI_DATA]);
        return r;
    }
    if (eh->e_type != ET_EXEC) {
        kprintf("elf: only ET_EXEC is supported; this one is type %u\n", eh->e_type);
        return r;
    }
    if (eh->e_machine != EM_X86_64) {
        kprintf("elf: wrong machine %u, expected %u\n", eh->e_machine, EM_X86_64);
        return r;
    }
    if (eh->e_phentsize != ELF64_PHDR_SIZE || eh->e_phnum == 0) {
        kprintf("elf: no usable program headers (%u entries of %u bytes)\n",
                eh->e_phnum, eh->e_phentsize);
        return r;
    }
    if (!in_range(image_bytes, eh->e_phoff, (size_t)eh->e_phnum * ELF64_PHDR_SIZE))
        return r;

    r.entry = eh->e_entry;
    r.phnum = eh->e_phnum;

    bool loaded_any = false;

    for (unsigned i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff
                                                    + (uint64_t)i * ELF64_PHDR_SIZE);

        if (ph->p_type != PT_LOAD)
            continue;

        if (!vmm_is_user_addr(ph->p_vaddr)) {
            kprintf("elf: segment %u is at %#lx, which is not user space\n", i,
                    (unsigned long)ph->p_vaddr);
            return (elf_load_result_t){ 0, 0, 0, 0, 0, false };
        }

        if (!in_range(image_bytes, ph->p_offset, (size_t)ph->p_filesz)) {
            kprintf("elf: segment %u runs past the end of the image\n", i);
            return (elf_load_result_t){ 0, 0, 0, 0, 0, false };
        }

        if (!load_segment(t->tables, base + ph->p_offset, ph->p_vaddr, ph->p_filesz,
                          ph->p_memsz, (ph->p_flags & PF_W) != 0))
            return (elf_load_result_t){ 0, 0, 0, 0, 0, false };

        loaded_any = true;
        r.segments++;
        r.bytes += ph->p_memsz;
    }

    if (!loaded_any) {
        kprintf("elf: no loadable segments\n");
        return (elf_load_result_t){ 0, 0, 0, 0, 0, false };
    }

    /* AT_PHDR in the auxiliary vector has to point at something real.  For an ordinary
     * link the headers are inside the first PT_LOAD already -- the file's first bytes
     * are mapped where the file puts them -- so this is usually a translation that
     * succeeds, and the mapping below is the fallback for a link that does not put them
     * there.  Mapping them unconditionally would be simpler and would quietly discard
     * whatever the first segment had put at that address. */
    /* Asked of *this task's* tables.  Asked of the running task's -- which is the loader's
     * own -- the answer is always "not mapped", so this fallback always ran, and it maps
     * over a page the first segment has already put there. */
    if (!vmm_translate_in(t->tables, phdr_vaddr) ||
        !syscall_user_range_ok_in(t->tables, phdr_vaddr, 16, false)) {
        size_t phbytes = ALIGN_UP((size_t)eh->e_phnum * ELF64_PHDR_SIZE, page_size);

        if (!load_segment(t->tables, base + eh->e_phoff, phdr_vaddr,
                          (uint64_t)eh->e_phnum * ELF64_PHDR_SIZE, phbytes, false)) {
            kprintf("elf: could not map the program headers at %#lx\n",
                    (unsigned long)phdr_vaddr);
            return (elf_load_result_t){ 0, 0, 0, 0, 0, false };
        }
    }

    if (!build_stack(t, r.entry, "minx", stack_pages, &r.entry_rsp)) {
        kprintf("elf: could not build the initial stack\n");
        return (elf_load_result_t){ 0, 0, 0, 0, 0, false };
    }

    r.ok = true;
    return r;
}
