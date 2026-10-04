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

/* The 64-bit TSS, at the offsets the hardware actually reads them from.
 *
 * The four bytes at 0x04 look spare -- that is where a 32-bit TSS puts SS:0 --
 * but leaving them out shifts every field that follows down by one quadword, and
 * the CPU does not shift with them: RSP0 then reads as zero, the IST entries
 * pick up the wrong eight bytes, and the I/O map base lands where a reserved word
 * belongs.  All three fail silently, the last two only when something faults or
 * ports.
 */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;    /* 0x00 */
    uint32_t reserved1;    /* 0x04 */
    uint64_t rsp[3];       /* 0x08 ring 0, ring 1, ring 2 stacks */
    uint64_t reserved2;    /* 0x20 */
    uint64_t ist[7];       /* 0x28 ist1 .. ist7 */
    uint64_t reserved3;    /* 0x60 */
    uint16_t reserved4;    /* 0x68 */
    uint16_t iomap_base;   /* 0x6A */
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