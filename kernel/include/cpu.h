/* minx - CPU state structures: descriptor tables, the TSS, and the interrupt
 * frame that every handler receives.
 */
#ifndef MINX_CPU_H
#define MINX_CPU_H

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------- GDT --- */

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28
#define GDT_TSS_HIGH    0x30

void gdt_init(void);
void gdt_set_kernel_table(void);
void gdt_set_user_table(void);

/* Pseudo-descriptor loaded by lgdt/lidt. */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) dt_ptr_t;

static inline void lgdt(const dt_ptr_t *p) {
    __asm__ volatile("lgdt %0" :: "m"(*p) : "memory");
}

static inline void lidt(const dt_ptr_t *p) {
    __asm__ volatile("lidt %0" :: "m"(*p) : "memory");
}

/* ---------------------------------------------------------------- TSS --- */

/* The x86-64 TSS is exactly 104 bytes.  Anything smaller makes `ltr` fail with
 * #GP because the descriptor's limit would not cover the I/O map base at 0x66.
 */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;    /* 0x00 */
    uint64_t rsp[3];       /* 0x04 ring 0, ring 1, ring 2 stacks */
    uint64_t reserved1;    /* 0x1C */
    uint64_t ist[7];       /* 0x24 ist1 .. ist7 */
    uint64_t reserved2;    /* 0x5C */
    uint16_t reserved3;    /* 0x64 */
    uint16_t iomap_base;   /* 0x66 */
} __attribute__((packed)) tss_t;

extern tss_t kernel_tss;

void tss_init(void);
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
typedef struct {
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