/* minx - application-processor startup trampoline.
 *
 * An application processor arrives here straight out of an INIT: sixteen-bit
 * real mode, paging off, no stack, and CS:IP pointing at the top of the page
 * this code lives in.  Everything below exists because of that starting point.
 *
 * The page has to be below 1 MiB, because the startup IPI carries only eight
 * bits and the CPU derives its entry point from them by multiplying by sixteen.
 * It is copied there at boot rather than linked there, because the kernel image
 * lives in the higher half and nothing can fetch from there with paging off.
 *
 * The page holds a 16-bit entry stub at offset zero, a 32-bit stub, a 64-bit
 * stub, and then a block of scratch that the boot processor fills in before
 * sending INIT.  Nothing here reads memory above the first gigabyte, so every
 * value in that block is a physical address or a small integer.
 *
 * The sequence is the one the Intel SDM prescribes (vol. 3, 8.4.4) and that
 * Limine's own trampoline uses:
 *
 *   1. 16-bit: load a throwaway GDT, far jump to 32-bit protected mode.
 *   2. 32-bit: CR4.PAE, CR3 = a PDPTE page, EFER.LME, CR0.PG.
 *   3. 64-bit: load the kernel's GDT, IDT and TSS, point GS at this processor's
 *      own state, take its stack, load the kernel's real PML4, enter the kernel.
 *
 * Step 2 needs a *separate* PDPTE page: with PAE enabled, CR3 names four PDPTE
 * entries rather than 512 PML4 entries.  The boot processor builds that page by
 * copying the kernel PML4's first four entries, which cover the very memory the
 * trampoline is executing from, and step 3 then loads the real PML4 before
 * leaving for kernel code in the higher half.
 *
 * One detail that is not optional: the CPU must branch immediately after CR0.PG
 * is set.  AMD requires it, Intel recommends it, and here it is also what makes
 * the next instruction fetch legal at all.
 *
 * Two things here cannot be written as link-time constants, and both are
 * patched into the copied page before the INIT goes out.  They are marked
 * PATCHED below, and kernel/arch/x86_64/smp.c computes them from the symbols
 * this file exports:
 *
 *   - the base field of the throwaway GDT, which has to be the *physical*
 *     address of a table inside this page, and
 *   - the selector of the far jump into 32-bit mode, which is the page number
 *     of this page rather than an index into any table.
 */

/* Offsets within the info block.  smp.c repeats these; they are gathered here
 * so the two cannot drift apart without this file being read. */
#define TPL_CR3          0    /* u32  PDPTE page, physical               */
#define TPL_PML4         4    /* u32  the kernel's PML4, physical         */
#define TPL_BOOTED       8    /* u32  set to 1 once in long mode          */
#define TPL_INDEX        12   /* u32  index into the per-CPU array        */
#define TPL_ENTRY        16   /* u64  64-bit kernel entry point           */
#define TPL_STACK        24   /* u64  64-bit stack (an HHDM address)      */
#define TPL_GS           32   /* u64  IA32_GS_BASE for this processor     */
#define TPL_GDT32_LIMIT  40   /* u16  */
#define TPL_GDT32_BASE   48   /* u64  */
#define TPL_GDT64_LIMIT  56   /* u16  */
#define TPL_GDT64_BASE   64   /* u64  */
#define TPL_IDT_LIMIT    72   /* u16  */
#define TPL_IDT_BASE     80   /* u64  */
#define TPL_PROGRESS     88   /* u32  how far the stub got, for diagnosis */

/* Scratch inside the block for the pseudo-descriptors the stubs assemble in
 * place: LGDT and LIDT both take a memory operand, and there is no convenient
 * stack-resident one to point them at after the CR3 load. */
#define TPL_GDTR32       96
#define TPL_GDTR64       112
#define TPL_IDTD         128
#define TPL_BLOCK_SIZE   144

/* Stack for the 32-bit half, at the top of this same page so that enabling
 * paging does not need a second region identity-mapped for it. */
#define TPL_STACK_SIZE   1024

    .section .text
    .code16
    .globl smp_trampoline_start
smp_trampoline_start:
    cli
    cld

    /* The kernel's own GDT is full of 64-bit descriptors, which a 16-bit LGDT
     * rejects outright.  This one exists only far enough to get into 32-bit
     * protected mode, where the kernel's table is loaded for real.  The address
     * is written as an offset rather than a symbol so that it assembles to a
     * plain constant with no relocation: CS already holds this page's number,
     * so CS plus an offset within the page is the physical address, and a
     * relocation would put a kernel-image address there instead. */
    lgdtl %cs:(tpl_gdt16 - smp_trampoline_start)

    /* PATCHED: the selector is this page's number.  The offset is a link-time
     * constant, because mode32 sits at a fixed place inside the page. */
    .globl smp_trampoline_farjmp
smp_trampoline_farjmp:
    ljmp $tpl_cs32, $(smp_trampoline_mode32 - smp_trampoline_start)

/* --- 32-bit protected mode ---------------------------------------------- */
    .code32
    .globl smp_trampoline_mode32
smp_trampoline_mode32:
    /* CS:IP was set to (page << 4):0, so shifting CS left by four gives the
     * physical address of this page and everything below is relative to it.
     * ebx survives into long mode and is the base for the rest of the stub. */
    movl %cs, %ebx
    shll $4, %ebx

    movw $tpl_ds32, %ax
    movw %ax, %ds
    movl $1, smp_trampoline_block+TPL_PROGRESS(%ebx)
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    leal smp_trampoline_stack_top(%ebx), %esp

    /* Assemble a 10-byte LGDT operand out of the block.  The 6-byte one the
     * 16-bit stub loaded is not a protected-mode pseudo-descriptor. */
    movw smp_trampoline_block+TPL_GDT32_LIMIT(%ebx), %ax
    movw %ax, smp_trampoline_block+TPL_GDTR32(%ebx)
    movl smp_trampoline_block+TPL_GDT32_BASE(%ebx), %eax
    movl %eax, smp_trampoline_block+TPL_GDTR32+2(%ebx)
    lgdt smp_trampoline_block+TPL_GDTR32(%ebx)

    /* PAE has to come first: without it CR3 names a page directory, and the
     * PDPTE page the boot processor built would be read as 1024 PDEs. */
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    movl smp_trampoline_block+TPL_CR3(%ebx), %eax
    movl %eax, %cr3
    movl $2, smp_trampoline_block+TPL_PROGRESS(%ebx)

    /* Long mode, plus NXE because the kernel's EFER has it. */
    movl $0xc0000080, %ecx
    rdmsr
    movl $3, smp_trampoline_block+TPL_PROGRESS(%ebx)
    orl $(1 << 8), %eax      /* LME */
    orl $(1 << 11), %eax     /* NXE */
    wrmsr

    movl %cr0, %eax
    orl $(1 << 31), %eax     /* PG */
    movl %eax, %cr0

    jmp smp_trampoline_paging_on
smp_trampoline_paging_on:
    /* A far *return* is the only way to change CS while long mode is off, and
     * this has to be one: a near RET would leave CS pointing into the throwaway
     * table, whose 64-bit segment is not the one the kernel's own table uses
     * for kernel code.  `lret` is the far RET (opcode CB); the assembler has no
     * `retf` spelling.  RETF pops the offset first and the selector second, so
     * the selector is pushed last. */
    leal smp_trampoline_mode64(%ebx), %eax
    movl $4, smp_trampoline_block+TPL_PROGRESS(%ebx)
    pushl $tpl_cs64
    pushl %eax
    lret

/* --- 64-bit long mode --------------------------------------------------- */
    .code64
    .globl smp_trampoline_mode64
smp_trampoline_mode64:
    /* Real mode leaves the high halves of the general registers undefined. */
    movl %ebx, %ebx

    movw smp_trampoline_block+TPL_GDT64_LIMIT(%rbx), %ax
    movw %ax, smp_trampoline_block+TPL_GDTR64(%rbx)
    movq smp_trampoline_block+TPL_GDT64_BASE(%rbx), %rax
    movq %rax, smp_trampoline_block+TPL_GDTR64+2(%rbx)
    lgdt smp_trampoline_block+TPL_GDTR64(%rbx)

    /* The kernel's selectors, not the throwaway ones above: in the kernel's
     * table 0x10 is the kernel data segment and 0x28 the TSS.  Loading 0x18 or
     * 0x20 from the throwaway table into a data register now faults, because in
     * the kernel's table they are user segments. */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* LIDT and LTR are per-CPU and this CPU has neither.  The IDT is the
     * kernel's, filled in once; without this, the first interrupt taken here
     * has no table to consult. */
    movw smp_trampoline_block+TPL_IDT_LIMIT(%rbx), %ax
    movl $5, smp_trampoline_block+TPL_PROGRESS(%ebx)
    movw %ax, smp_trampoline_block+TPL_IDTD(%rbx)
    movq smp_trampoline_block+TPL_IDT_BASE(%rbx), %rax
    movq %rax, smp_trampoline_block+TPL_IDTD+2(%rbx)
    lidt smp_trampoline_block+TPL_IDTD(%rbx)

    /* LTR takes a register, so the selector goes through AX.  The kernel's TSS
     * occupies two GDT slots, which is why the descriptor carries both. */
    movw $0x28, %ax
    ltr %ax
    movl $6, smp_trampoline_block+TPL_PROGRESS(%ebx)

    /* This processor's own per-CPU area, so that %gs:0 reads back its own
     * identity and every other processor reads a different one. */
    movl $0xc0000101, %ecx
    movq smp_trampoline_block+TPL_GS(%rbx), %rax
    wrmsr

    /* Onto the kernel's stack, which is an HHDM address because the identity
     * map this stub has been using goes away with the PDPTE page. */
    movq smp_trampoline_block+TPL_STACK(%rbx), %rsp
    movl $7, smp_trampoline_block+TPL_PROGRESS(%ebx)

    movq smp_trampoline_block+TPL_PML4(%ebx), %rax
    movl $8, smp_trampoline_block+TPL_PROGRESS(%ebx)
    movq %rax, %cr3
    jmp smp_trampoline_paging_on64
smp_trampoline_paging_on64:
    /* Tell the boot processor this CPU made it, then enter the kernel with the
     * per-CPU index in edi. */
    movl $1, smp_trampoline_block+TPL_BOOTED(%ebx)
    movl $9, smp_trampoline_block+TPL_PROGRESS(%ebx)

    movl smp_trampoline_block+TPL_INDEX(%ebx), %edi
    movq smp_trampoline_block+TPL_ENTRY(%rbx), %rax
    jmp *%rax

/* --- the throwaway 16-bit GDT ------------------------------------------ */
    .code16
.balign 16
tpl_gdt16:
    .word tpl_gdt16_end - tpl_gdt16 - 1
    /* PATCHED: the physical address of this table inside the copied page.
     * Written as a bare zero rather than a reference to the symbol, because the
     * symbol's link-time address is a kernel-image address above 4 GiB and does
     * not fit in the 32-bit base field -- which is exactly why the value has to
     * be filled in at run time. */
    .globl smp_trampoline_gdt16_base
smp_trampoline_gdt16_base:
    .long 0
    .quad 0                       /* null                    */
    .quad 0x00cf9a000000ffff      /* tpl_cs32: 32-bit code   */
    .quad 0x00cf92000000ffff      /* tpl_ds32: 32-bit data   */
    .quad 0x00af9a000000ffff      /* tpl_cs64: 64-bit code   */
    .quad 0x00af92000000ffff      /* tpl_ds64: 64-bit data   */
tpl_gdt16_end:

.set tpl_cs32, 0x08
.set tpl_ds32, 0x10
.set tpl_cs64, 0x18
.set tpl_ds64, 0x20

/* --- the wait-for-SIPI stub -------------------------------------------- */
    .code16
.balign 16
    .globl smp_trampoline_wait
smp_trampoline_wait:
    /* Where a processor goes when it is reset and released but has not been
     * told where to start yet.  Real firmware parks itself here; the kernel maps
     * this page over the reset vector so that the INIT/SIPI sequence behaves the
     * same way on a machine whose firmware left something else there.
     *
     * A spin rather than a halt, because the startup IPI is not an interrupt: it
     * changes CS and IP directly, and a processor stopped in HLT with interrupts
     * disabled is not guaranteed to be looking at its instruction pointer.
     * Nothing happens here for longer than the boot processor's wait loop, and
     * that loop is bounded. */
    sti
    hlt
    jmp smp_trampoline_wait

/* --- the block the boot processor fills in, and the 32-bit stack --------- */
    .code32
.balign 16
    .globl smp_trampoline_block
smp_trampoline_block:
    .space TPL_BLOCK_SIZE

.balign 16
smp_trampoline_stack:
    .space TPL_STACK_SIZE
smp_trampoline_stack_top:

smp_trampoline_end:
    .globl smp_trampoline_size
smp_trampoline_size:
    .quad smp_trampoline_end - smp_trampoline_start
