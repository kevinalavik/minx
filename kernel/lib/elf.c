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
#include "serial.h"

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
/* Where a process's heap starts, and where its anonymous mappings start.
 *
 * The heap is a long way above the program so that however far `brk` grows it can never
 * reach the program's own segments -- a heap that runs into its own text is a heap that
 * corrupts the program when it grows.  Two megabytes is far more than a small program's
 * segments need; the linker put them at 4 MiB and they end a few megabytes later.
 *
 * `mmap` starts much higher still, so a program's segments, its heap and its mappings are
 * three regions that never meet.  See memsys.c. */
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

    /* Whole pages, from the first the segment touches to the first it does not.
     *
     * A segment does not start at a page boundary and does not end at one either, so the
     * first and last pages are shared with whatever lies either side of it -- usually with
     * the *next* segment, since a linker packs text and data into adjacent pages and the
     * boundary between them is almost never page-aligned.  Those pages are mapped once and
     * filled in twice.
     *
     * Which is what used to go wrong, and only showed up once `hello` grew past two pages
     * of text: this allocated a fresh frame for every page of every segment, and
     * pt_map_page() deliberately refuses to replace a different frame.  So the second
     * segment to reach a shared page found the first one's frame there and refused, and the
     * program would not load -- with "elf: could not map 0x4022ae" as the whole report.
     * A loader that cannot map a page another segment already mapped is not a loader. */
    uint64_t first = virt & ~(uint64_t)(page_size - 1);
    uint64_t end = (virt + memsz + page_size - 1) & ~(uint64_t)(page_size - 1);

    for (uint64_t page = first; page < end; page += page_size) {
        /* Where this page sits within the segment, and within the buffer the file's bytes
         * for the segment are copied from.  Two different distances, and conflating them is
         * the bug this arithmetic exists to avoid.
         *
         * `skip` is how far the page is from the *start of the segment*: zero on the first
         * page, 4 KiB on the second, and so on.  It is what says where in the file's bytes
         * this page begins.
         *
         * `seg_from` is where the segment begins *within this page*: zero on every page but
         * the first, and the segment's misalignment on the first.  It is what says where in
         * this page the bytes go.
         *
         * Confusing the two gives a source offset of 0 on every page, so every page of a
         * segment gets a copy of the segment's *first* page.  One page of text loads
         * perfectly and two pages load with the second one full of the first one's bytes --
         * so the program's entry point, which the linker has placed on the second page by
         * then, executes whatever was at the top of the first.  There is nothing to fault
         * on: the page is mapped, it is writable, it is full of valid text, and the CPU
         * starts somewhere nobody asked for.  The observable symptom is a program that
         * dies on its first instruction with a #GP out of `iretq`, which points at the
         * privilege transition and not at the loader at all. */
        uint64_t skip = (page >= virt) ? page - virt : 0;
        uint64_t seg_from = (virt > page) ? virt - page : 0;
        uint64_t seg_to = (virt + memsz < page + page_size) ? virt + memsz - page
                                                             : page_size;

        /* Where this page's slice of the *file* content starts and ends.
         *
         * The file provides `filesz` bytes, beginning where the segment begins -- which on
         * the first page is `seg_from` bytes into it, not at the page's start.  So the two
         * ends are `seg_from` and `seg_from + filesz`, both clamped to the segment's own
         * extent in this page.
         *
         * They used to be clamped against `filesz` *as if it were a page offset*, which is
         * right only when the segment happens to start at the top of a page.  A `.data`
         * that starts 0x4a0 bytes into its page and is 0x160 bytes long has
         * `filesz < seg_from`, so both ends collapsed onto `filesz` and the segment was
         * never copied: its `.data` came up **zeroed**, silently, with the loader reporting
         * success.
         *
         * What that looks like is a program whose initialised globals are all NULL.  It
         * showed up when libc arrived, because libc is the first thing in this tree with a
         * `.data` big enough to be shorter than its own offset into the page: `stdout` was
         * a NULL `FILE *`, `printf()` wrote through it and did nothing, and `write(1, ...)`
         * from the same program worked perfectly.  A loader that drops a segment's contents
         * is worse than one that refuses the segment, because nothing says it happened. */
        uint64_t file_from = seg_from;
        uint64_t file_to = seg_from + filesz;

        if (file_to > seg_to)
            file_to = seg_to;

        uint64_t phys;
        pte_t leaf = pt_get(tables, page);

        if (pte_present(leaf) && pte_user(leaf)) {
            /* A page another segment already put here.  Fill it in rather than replacing
             * it, and take the union of what the two segments ask for: a page that one
             * segment calls read-only and another calls writable has to end up writable,
             * or the second segment faults on its own text. */
            phys = pte_phys(leaf);

            if (writable && !pte_writable(leaf) &&
                !pt_set_writable(tables, page, true)) {
                kprintf("elf: could not make the shared page at %#lx writable\n",
                        (unsigned long)page);
                return false;
            }
        } else {
            phys = vmm_user_page_alloc();

            /* Every one of these used to be a bare `return false`, so a program that
             * would not load produced one line saying it would not load and nothing about
             * why.  A loader that fails silently is indistinguishable from one that was
             * never asked to load anything. */
            if (phys == 0) {
                kprintf("elf: out of frames mapping %#lx+%lu\n",
                        (unsigned long)virt, (unsigned long)memsz);
                return false;
            }

            if (vmm_map_into(tables, page, phys, page_size, writable) != 0) {
                kprintf("elf: could not map %#lx into %p\n",
                        (unsigned long)page, (void *)tables);
                vmm_user_page_free(phys);
                return false;
            }
        }

        /* Copy what the file holds for this page.  The rest of the segment's range --
         * .bss, and the slack below a segment that starts mid-page -- is already zero,
         * because vmm_user_page_alloc() hands out zeroed frames and the segment ranges in
         * one image do not overlap, so nothing another segment wrote here can be in it. */
        if (file_to > file_from)
            __builtin_memcpy((uint8_t *)phys_to_virt(phys) + file_from,
                             image + skip + (file_from - seg_from),
                             (size_t)(file_to - file_from));
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
/* Write one quadword into a task's user stack.
 *
 * Through the direct map of whatever frame backs that particular address, because the
 * stack's pages are not physically contiguous -- see build_stack().
 */
static void stack_put(task_t *t, uint64_t vaddr, uint64_t value) {
    uint64_t phys = vmm_translate_in(t->tables, vaddr & ~(page_size - 1));

    if (phys == 0)
        return;
    *(uint64_t *)phys_to_virt(phys + (vaddr & (page_size - 1))) = value;
}

/* Write `n` bytes into a task's user stack, crossing page boundaries as needed. */
static void stack_write(task_t *t, uint64_t vaddr, const void *src, size_t n) {
    const uint8_t *from = (const uint8_t *)src;

    while (n != 0) {
        uint64_t page = vaddr & ~(page_size - 1);
        size_t off = (size_t)(vaddr - page);
        size_t chunk = page_size - off;
        uint64_t phys = vmm_translate_in(t->tables, page);

        if (phys == 0)
            return;
        if (chunk > n)
            chunk = n;

        memcpy((void *)(phys_to_virt(phys) + off), from, chunk);
        from += chunk;
        vaddr += chunk;
        n -= chunk;
    }
}

/* How many entries one of these arrays has, NULL-terminated or not.
 *
 * A caller that passes NULL gets zero, and a caller that passes a non-NULL first element
 * gets a count that stops at the first NULL.  The second case is the one that matters:
 * every user program builds its argument vector NULL-terminated, because that is what
 * `execve` is given, and a count that ran off the end would be a walk through whatever
 * followed. */
static size_t count_of(const char *const *v) {
    size_t n = 0;

    if (v == NULL)
        return 0;

    while (v[n] != NULL && n < 4096)
        n++;

    return n;
}

static bool build_stack(task_t *t, uint64_t entry, const char *path,
                        const char *const *argv, const char *const *envp,
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

    /* The arguments and the environment, as the program will see them.
     *
     * `argv[0]` is the program's name as it was invoked -- the *path*, not the path with a
     * slash replaced by a base name.  `basename()`-ing it here would be helpful and wrong:
     * a program that re-execs itself by `argv[0]` has to get back the name it was given,
     * and a program that prints its usage message wants the name the user typed.  What
     * `argv[0]` is *not* allowed to be is NULL: a program that does `strcmp(argv[0], "sh")`
     * has to be able to, and an empty `argv[0]` is a lie about something the caller knows.
     *
     * A NULL vector is replaced by the path alone, which is what a program started with no
     * arguments at all should see: one argument, which is its name. */
    size_t argc = count_of(argv);
    size_t envc = count_of(envp);

    /* The one-argument vector used when the caller passed none.
     *
     * Declared *here*, at function scope, and not inside the `if` that uses it.  That is
     * not style: C ends an automatic object's lifetime at the end of its block, and
     * `argv` -- which is used for the rest of this function -- would be left pointing at
     * sixteen bytes of stack that the compiler is free to hand to the next local it needs.
     * And it does: the two address arrays below are several kilobytes, so they land
     * exactly there.  `argv[0]` then reads a zero out of the middle of `argv_addr`, and
     * `strnlen()` is asked for the length of a NULL pointer -- a kernel page fault at
     * address 0, inside the loader, on the *first* program the self tests start.
     *
     * So the array is as long-lived as the pointer that refers to it. */
    const char *fallback[2] = { path, NULL };

    if (argc == 0) {
        argv = fallback;
        argc = 1;
    }

    /* The auxiliary vector, then one auxv entry each for
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

    uint64_t block_bytes = 8                       /* argc                   */
                         + 8 * (argc + 1)          /* argv[], and its NULL   */
                         + 8 * (envc + 1)          /* envp[], and its NULL   */
                         + (uint64_t)naux * 16;

    /* Every string the program can reach, each padded to 16 so the block below stays
     * aligned.  `path` is counted as well as the vector because the fallback above may have
     * put it into `argv[0]`, and counting it twice would reserve space that is used --
     * the strings are *written* one per vector entry, so what has to fit is the vector's
     * contents, not the vector and the path separately. */
    uint64_t strings_bytes = 0;

    for (size_t i = 0; i < argc; i++)
        strings_bytes += ALIGN16(strnlen(argv[i], 4096) + 1);

    for (size_t i = 0; i < envc; i++)
        strings_bytes += ALIGN16(strnlen(envp[i], 4096) + 1);

    /* The name is written unconditionally, above the vector's own strings, so that a
     * program with a NULL vector still has *something* in the slot rather than a null
     * pointer it would have to check for. */
    strings_bytes += ALIGN16(path_len + 1);

    /* 256 bytes of slack below the block so a program's first few frames cannot walk
     * off the bottom of the stack into whatever the allocator gave us. */
    if (block_bytes + strings_bytes + 256 + 16 > usable)
        return false;

    /* Zero every page of the stack, one page at a time, through the direct map of each
     * page's *own* frame.
     *
     * The obvious spelling of this -- take the bottom page's frame and memset `usable`
     * bytes from it -- is wrong, and wrong in a way that took the whole phase-6 boundary
     * with it.  The pages of a task's stack are **not physically contiguous**: they come
     * from the same allocator as the page tables, so the frames the loader allocated
     * afterwards land *between* them.  Measured on this kernel: the eight stack pages are
     * 0x513d000, 0x5141000, 0x5142000 ... 0x5147000 -- with page directories, a PDPT and a
     * leaf frame in the gaps.  A memset from the bottom page runs straight through those
     * gaps, so zeroing the user's stack silently zeroed the page tables, and the entry
     * point's mapping vanished between being written and being used.
     *
     * Every access to the stack therefore goes through a translation now, and the layout
     * is arithmetic on *user* virtual addresses rather than on a kernel pointer, which is
     * also what the program sees.
     *
     * The topmost page is skipped: it holds the scheduler's bootstrap context and the
     * ring-3 return frame, which task_create_user() built. */
    /* The condition is on the *end* of each page, not its start: the topmost page holds
     * the frames, and testing `a < top - frames` still admits it because its first byte is
     * well below them -- which zeroes the frame and leaves the first `ret` returning to
     * address zero.  The topmost page is skipped deliberately; it arrives zeroed anyway,
     * because `vmm_user_page_alloc()` hands out zeroed frames. */
    for (uint64_t a = top - cap; a + page_size <= top - frames; a += page_size) {
        uint64_t phys = vmm_translate_in(t->tables, a);

        if (phys == 0) {
            kprintf("elf: the stack page at %#lx is not mapped\n", (unsigned long)a);
            return false;
        }
        memset((void *)phys_to_virt(phys), 0, page_size);
    }

    /* The strings, at the top of the region the program owns. */
    /* Lay the block out from an *aligned* stack pointer rather than from the strings down.
     *
     * `sp = q & ~15` rounds the block's low end down, which quietly moves the stack
     * pointer off the `argc` word whenever the block does not happen to end 16-byte
     * aligned -- and whether it does depends on the length of the program's name, so the
     * same kernel built `argc = 1` for one program and `argc = 0` for the next.  A
     * program entered that way reads a zero where its argument count should be, which is
     * not a crash: it is a program that runs happily with no arguments, and the loader
     * looks correct.
     *
     * So the stack pointer is computed first, aligned, and the rest of the block hangs
     * off it. */
    uint64_t sp = ALIGN_UP(top - frames - strings_bytes - block_bytes, 16);

    /* Two cursors, and the difference between them is the whole layout.
     *
     * `b` is the *bottom* of the string region: the block hangs downward from it, and it
     * never moves.  `p` walks upward through the strings, because the block holds pointers
     * to them and those addresses have to be known before the block is written -- a two-pass
     * layout is cheaper than a patch-up pass over the block afterwards.
     *
     * Deriving the block's top from `p` instead is the mistake that was made first: `p`
     * ends up `strings_bytes` above `b` after the strings are written, so a block laid out
     * downward from it ends `strings_bytes` above `sp` -- and the check that the block ends
     * exactly on the stack pointer then fails for every program, with a message about the
     * argument block and the stack pointer that is really about a cursor. */
    uint64_t b = sp + block_bytes;
    uint64_t p = b;

    /* The name first, at the very top, and the vector's strings below it.
     *
     * Top-down rather than bottom-up, for the reason above. */
    stack_write(t, p, path, path_len);
    {
        char nul = 0;

        stack_write(t, p + path_len, &nul, 1);
    }

    uint64_t argv_addr[4096 / sizeof(uint64_t) + 1];
    uint64_t envp_addr[4096 / sizeof(uint64_t) + 1];

    p += ALIGN16(path_len + 1);

    for (size_t i = 0; i < argc; i++) {
        size_t len = strnlen(argv[i], 4096);

        stack_write(t, p, argv[i], len);
        stack_write(t, p + len, &(char){ 0 }, 1);
        argv_addr[i] = p;
        p += ALIGN16(len + 1);
    }

    for (size_t i = 0; i < envc; i++) {
        size_t len = strnlen(envp[i], 4096);

        stack_write(t, p, envp[i], len);
        stack_write(t, p + len, &(char){ 0 }, 1);
        envp_addr[i] = p;
        p += ALIGN16(len + 1);
    }

    /* The *addresses the program will see* are offsets into its own stack, not offsets
     * into the kernel's alias of it.  These two were computed from the alias, so the
     * program was entered with a stack pointer into the direct map and an argv[0] pointer
     * into the direct map -- and `iretq` loading a kernel address into RSP for a ring-3
     * task is a #GP before the first instruction, with nothing to say which field of a
     * loader was wrong.
     *
     * So the alias is used only to *write*; everything the program will read back is
     * expressed against the user address of the stack. */
    /* The block, written *upward* from `sp`.
     *
     * Upward, and not downward from `b`, and the reason is not style.  Laying it out
     * downward means every field's address is "wherever the cursor is after subtracting
     * this field", and the order of the subtractions has to be the reverse of the order of
     * the fields -- which is exactly the kind of thing that is right in the source, wrong
     * by one, and indistinguishable from a loader bug until a program reads `envp[0]` and
     * finds eight bytes of a string.  The fields went in the wrong order: `argv[0]` was
     * written where `argv[argc]` belongs and the NULL markers went where the arrays belong,
     * so `__libc_start_main()` read a string's *address* as an argument count and eight
     * characters of `PATH=/bin:/usr/bin` as the end of argv.  The program's own report was
     * `getenv()` faulting on a non-canonical address.
     *
     * Upward, each field is written where it is, the cursor only ever increases, and the
     * one check at the end -- that the cursor arrived at `b` -- is the whole proof that
     * `block_bytes` and what was written agree.  That check is the same one that was there
     * before, and it passed for the layout that was wrong: it verifies the *size*, and the
     * size was right.  It just could not see which field went where. */
    uint64_t q = sp;

    stack_put(t, q, (uint64_t)argc);
    q += 8;

    for (size_t i = 0; i < argc; i++) {
        stack_put(t, q, argv_addr[i]);
        q += 8;
    }

    stack_put(t, q, 0);                     /* argv[argc] == NULL */
    q += 8;

    for (size_t i = 0; i < envc; i++) {
        stack_put(t, q, envp_addr[i]);
        q += 8;
    }

    stack_put(t, q, 0);                     /* envp[envc] == NULL */
    q += 8;

    stack_write(t, q, auxv, (size_t)naux * 16);
    q += (uint64_t)naux * 16;

    /* The block has to fill exactly the space `block_bytes` reserved, ending where the
     * strings begin.  Together with the 16-byte alignment of `sp`, that is what puts the
     * ABI's words in the right places; both are checked rather than rounded, because
     * rounding is what moved the stack pointer off `argc` in the first place. */
    if (q != b || (sp & 15) != 0) {
        kprintf("elf: the argument block ends at %#lx but the strings begin at "
                "%#lx (stack pointer %#lx)\n",
                (unsigned long)q, (unsigned long)b, (unsigned long)sp);
        return false;
    }

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
    t->brk_start = BRK_START;
    t->exit_code = -1;

    *entry_rsp = sp;
    return true;
}

elf_load_result_t elf_load(const void *image, size_t image_bytes, task_t *t,
                           uint32_t stack_pages, uint64_t phdr_vaddr,
                           const char *const *argv, const char *const *envp) {
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

    if (!build_stack(t, r.entry, "minx", argv, envp, stack_pages, &r.entry_rsp)) {
        kprintf("elf: could not build the initial stack\n");
        return (elf_load_result_t){ 0, 0, 0, 0, 0, false };
    }

    r.ok = true;
    return r;
}
