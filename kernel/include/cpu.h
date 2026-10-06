/* minx - CPU state structures: descriptor tables, the TSS, and the interrupt
 * frame that every handler receives.
 */
#ifndef MINX_CPU_H
#define MINX_CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "boot.h"   /* MAX_CPU_COUNT */

/* ---------------------------------------------------------------- GDT --- */

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20

/* The same two descriptors requested at privilege level 3.
 *
 * The selectors above carry RPL 0, which is right for naming a descriptor and wrong for
 * *entering* one: `iretq` takes the new CPL from the RPL of the CS in the frame, so a
 * frame built with 0x20 and 0x18 does not drop privilege at all.  It pops RIP, CS and
 * RFLAGS, stops there, and leaves SS and RSP sitting on the stack -- so the task runs its
 * first instruction in ring 0, on the stack pointer it had as a kernel task, with two
 * words of its frame still unconsumed.  Nothing about that looks like a segment problem
 * until something reads SS.
 *
 * The RPL is in the low two bits, so `| 3` names the same GDT entry: the index used to
 * reach it is `selector >> 3`, which ignores them. */
#define GDT_USER_DATA_RPL3 (GDT_USER_DATA | 3)
#define GDT_USER_CODE_RPL3 (GDT_USER_CODE | 3)

/* The first TSS descriptor, and the distance between one processor's descriptor
 * and the next.  A 64-bit TSS descriptor is 16 bytes -- two ordinary 8-byte slots
 * -- and each processor needs its own, so the table has room for all of them. */
#define GDT_TSS          0x28
#define GDT_TSS_STRIDE   16

/* Processor `index`'s task state segment descriptor. */
#define gdt_tss_selector(index) (GDT_TSS + (unsigned)(index) * GDT_TSS_STRIDE)

/* Size of an IST stack.  IST 1 is the double-fault stack and IST 2 the
 * non-maskable-interrupt one; both are entered by the CPU on a fault the handler
 * cannot survive, so they cannot be shared between processors. */
#define IST_STACK_SIZE 8192

void gdt_init(void);
void gdt_set_kernel_table(void);
void gdt_set_user_table(void);

/* Reload the GDT and the task register, for a CPU that installed its own TSS. */
void gdt_reload(void);
void gdt_reload_for(unsigned index);

/* Re-load processor `index`'s TSS from its (just rewritten) descriptor.
 *
 * `ltr` caches the TSS's base, limit and attributes inside the processor, so a descriptor
 * rewritten afterwards is not seen until this runs.  Unlike gdt_reload_for() this leaves
 * %gs alone, which matters because the per-CPU base here lives in MSR_GS_BASE and loading
 * %gs from the table would zero it. */
void tss_reload_for(unsigned index);

/* Publish processor `index`'s TSS in the table, so `gdt_reload_for()` can load
 * it.  Called with the TSS already programmed. */
void gdt_write_tss_descriptor(unsigned index, uint64_t base, uint32_t limit);

/* Decode processor `index`'s TSS descriptor.  False if the index is out of range
 * or the entry is not a 64-bit TSS descriptor. */
bool gdt_tss_descriptor_for(unsigned index, uint64_t *base, uint32_t *limit,
                            uint8_t *type);

/* Pseudo-descriptor loaded by lgdt/lidt. */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) dt_ptr_t;

static inline void lgdt(const dt_ptr_t *p) {
    __asm__ volatile("lgdt %0" :: "m"(*p) : "memory");
}

/* A copy of the GDT pseudo-descriptor, so a CPU that starts with paging off can
 * load the table itself. */
dt_ptr_t gdt_descriptor(void);

/* Load the IDT into this CPU.  LIDT is per-processor state, so an application
 * processor needs it even though the kernel fills the table in only once. */
void idt_reload(void);

/* Copies of the descriptor pseudo-descriptors, for a CPU that has to load the
 * tables itself because it starts with paging off. */
dt_ptr_t gdt_descriptor(void);
void     idt_descriptor(dt_ptr_t *out);

static inline void lidt(const dt_ptr_t *p) {
    __asm__ volatile("lidt %0" :: "m"(*p) : "memory");
}

/* ---------------------------------------------------------------- TSS --- */

/* The 64-bit TSS, at the offsets the processor actually reads them from.
 *
 * There is *one* reserved quadword at the top and the first stack pointer is at
 * 0x04, not 0x08.  It looks like the 32-bit TSS's `esp0` slot widened to 64 bits,
 * because it is: the 32-bit TSS puts `esp0` at 0x04 and `ss0` at 0x08, and the
 * 64-bit TSS keeps the `esp0` position and spends the next eight bytes on the ring-1
 * pointer that the 32-bit layout spends on `ss0`.  Linux's `struct x86_hw_tss` says
 * the same thing (`u32 reserved1; u64 sp0; u64 sp1; u64 sp2;`), and it is the
 * authority here: Linux takes a ring-3 stack switch on every syscall on every
 * machine anyone runs it on.
 *
 * Adding a second reserved dword "for symmetry" is the mistake this layout is
 * written to prevent, and it is worth spelling out what it costs, because it costs
 * it silently.  Every field after the top moves up four bytes, and a TSS is a
 * flat struct of offsets rather than a structure of pointers, so nothing notices:
 *
 *   - RSP0 reads as the *reserved* word, which is zero, so the first interrupt out
 *     of ring 3 pushes onto address zero.  That is a #PF at 0xfffffff8 and then a
 *     #DF, reported at the *original* privilege level and RIP -- so a user program
 *     that makes a syscall appears to fault twice at its own entry point, and the
 *     kernel never gets to look at it.
 *   - IST1 reads as the low half of `reserved2`, also zero, so the double-fault
 *     handler has no stack either and #DF becomes a triple fault.  Everything from
 *     phase 1 onwards passes regardless, because an exception at CPL 0 needs no
 *     stack switch: the CPU pushes onto the stack it is already on.
 *   - iomap_base lands on a reserved word, which happens to still mean "no bitmap",
 *     so even the test that checks it passes.
 *
 * That is three separate failures that all read as "the ring-3 boundary does not
 * work yet", which is why this comment used to assert the opposite. */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;    /* 0x00 */
    uint64_t rsp[3];       /* 0x04 ring 0, 0x0C ring 1, 0x14 ring 2 */
    uint64_t reserved2;    /* 0x1C */
    uint64_t ist[7];       /* 0x24 ist1 .. ist7 */
    uint64_t reserved3;    /* 0x5C */
    uint16_t reserved4;    /* 0x64 */
    uint16_t iomap_base;   /* 0x66 */
} __attribute__((packed)) tss_t;

/* One TSS per processor, indexed the same way the per-CPU slots are.
 *
 * They cannot be shared.  Loading a task register sets the descriptor's busy bit
 * *in the descriptor table it came from*, so the first processor to run `ltr`
 * leaves the entry marked busy and every other processor that tries to load the
 * same entry is refused with #GP and the selector as the error code.  Each
 * processor therefore needs its own TSS and its own slot in the table. */
extern tss_t kernel_tss[MAX_CPU_COUNT];

static inline tss_t *tss_for(unsigned index) { return &kernel_tss[index]; }

void tss_init(void);                              /* this processor's */
void tss_init_for(unsigned index, uint64_t ist1, uint64_t ist2);
void tss_set_rsp0(uint64_t rsp);
void tss_set_rsp0_for(unsigned index, uint64_t rsp);
void tss_report(unsigned index);
void tss_set_ist(int ist, uint64_t rsp);

/* ---------------------------------------------------------------- IDT --- */

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

/* The IDT plus the pseudo-descriptor handed to lidt(). */
typedef struct {
    idt_entry_t entries[256];
    dt_ptr_t    ptr;
} idt_table_t;

extern idt_table_t idt;

void idt_init(void);
void idt_set_gate(int vector, void *handler, uint8_t ist, uint8_t dpl);

/* ------------------------------------------------------- interrupt frame - */

/* Pushed by the assembly stubs in idt_asm.S.  The layout matches the order in
 * which pushq instructions run, and is also the ABI between the kernel and
 * user space (see syscall entry). */
/* Named `regs` as well as typedef'd, because driver headers take the frame as
 * `struct regs *` and that reads better than the typedef everywhere. */
typedef struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) regs_t;

/* Handlers registered by the architecture layer. */
typedef void (*isr_handler_t)(regs_t *regs);
void isr_install_handler(uint8_t vector, isr_handler_t handler);
void isr_dispatch(regs_t *regs);

/* Console cursor helpers used when reporting a fault. */
void isr_report_exception(regs_t *regs);

#endif /* MINX_CPU_H */