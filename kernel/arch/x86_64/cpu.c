/* minx - the global descriptor table and task state segment.
 *
 * The kernel builds a proper GDT with a TSS instead of relying on whatever the
 * bootloader left in the register: ring 3 transitions need a known kernel stack
 * (TSS.rsp0) and double-fault/NMI want their own IST stacks.
 */
#include "cpu.h"

#include "io.h"
#include "panic.h"
#include "string.h"
#include "types.h"

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} gdt_entry_t;

/* Seven descriptors are needed: null, 32-bit kernel code, 32/64-bit kernel
 * data, 32/64-bit user data, 64-bit user code, and the two slots a 64-bit TSS
 * occupies.  Rounded up to eight so the TSS high half is inside the limit. */
static gdt_entry_t gdt[8];

static dt_ptr_t gdt_ptr;

idt_table_t idt;

/* Flag byte bits.  G is 0x80, D/B is 0x40 and L (64-bit code) is 0x20; getting
 * these wrong turns a long-mode descriptor into a 32-bit one and every far
 * jump to it raises #GP. */
#define GDT_FLAG_GRANULARITY 0x80
#define GDT_FLAG_LONG_MODE   0x20

/* Access bytes: present, DPL 0, code/data, exec/read or read/write. */
#define GDT_ACCESS_KERNEL_CODE 0x9A
#define GDT_ACCESS_KERNEL_DATA 0x92
#define GDT_ACCESS_USER_DATA   0xF2
#define GDT_ACCESS_USER_CODE   0xFA

static void gdt_encode(gdt_entry_t *out, uint32_t base, uint32_t limit,
                       uint8_t access, uint8_t flags) {
    out->limit_low   = (uint16_t)(limit & 0xffff);
    out->base_low    = (uint16_t)(base & 0xffff);
    out->base_mid    = (uint8_t)((base >> 16) & 0xff);
    out->access      = access;
    /* The top nibble holds the limit's high bits; the flags (G, D/B, L, AVL
     * and P) all live in the bottom byte and must not be masked off, or the
     * 64-bit L bit silently turns a long-mode descriptor into a 32-bit one. */
    out->granularity = (uint8_t)(((limit >> 16) & 0x0f) | flags);
    out->base_high   = (uint8_t)((base >> 24) & 0xff);
}

/* Descriptors for the interrupt gate stubs are built from these symbols. */
extern void isr_stub_0(void);
extern void isr_stub_1(void);
extern void isr_stub_2(void);
extern void isr_stub_3(void);
extern void isr_stub_4(void);
extern void isr_stub_5(void);
extern void isr_stub_6(void);
extern void isr_stub_7(void);
extern void isr_stub_8(void);
extern void isr_stub_9(void);
extern void isr_stub_10(void);
extern void isr_stub_11(void);
extern void isr_stub_12(void);
extern void isr_stub_13(void);
extern void isr_stub_14(void);
extern void isr_stub_15(void);
extern void isr_stub_16(void);
extern void isr_stub_17(void);
extern void isr_stub_18(void);
extern void isr_stub_19(void);
extern void isr_stub_20(void);
extern void isr_stub_21(void);
extern void isr_stub_22(void);
extern void isr_stub_23(void);
extern void isr_stub_24(void);
extern void isr_stub_25(void);
extern void isr_stub_26(void);
extern void isr_stub_27(void);
extern void isr_stub_28(void);
extern void isr_stub_29(void);
extern void isr_stub_30(void);
extern void isr_stub_31(void);
extern void isr_stub_32(void);
extern void isr_stub_33(void);
extern void isr_stub_34(void);
extern void isr_stub_35(void);
extern void isr_stub_36(void);
extern void isr_stub_37(void);
extern void isr_stub_38(void);
extern void isr_stub_39(void);
extern void isr_stub_40(void);
extern void isr_stub_41(void);
extern void isr_stub_42(void);
extern void isr_stub_43(void);
extern void isr_stub_44(void);
extern void isr_stub_45(void);
extern void isr_stub_46(void);
extern void isr_stub_47(void);

/* IST index for the vectors that need a private stack: #DF, NMI and the
 * machine check exception get one each. */
#define IST_DOUBLE_FAULT 1
#define IST_NMI          2

extern void isr_stub_8(void);   /* #DF, re-declared for the IST table */

static struct {
    uint8_t  vector;
    uint8_t  ist;
    void    *handler;
} const exception_table[] = {
    {  0, 0, isr_stub_0  }, {  1, 0, isr_stub_1  },
    {  2, 0, isr_stub_2  }, {  3, 0, isr_stub_3  },
    {  4, 0, isr_stub_4  }, {  5, 0, isr_stub_5  },
    {  6, 0, isr_stub_6  }, {  7, 0, isr_stub_7  },
    {  8, IST_DOUBLE_FAULT, isr_stub_8 },
    {  9, 0, isr_stub_9  },
    { 10, 0, isr_stub_10 }, { 11, 0, isr_stub_11 },
    { 12, 0, isr_stub_12 }, { 13, 0, isr_stub_13 },
    { 14, 0, isr_stub_14 }, { 15, 0, isr_stub_15 },
    { 16, 0, isr_stub_16 }, { 17, 0, isr_stub_17 },
    { 18, 0, isr_stub_18 }, { 19, 0, isr_stub_19 },
    { 20, 0, isr_stub_20 }, { 21, 0, isr_stub_21 },
    { 22, 0, isr_stub_22 }, { 23, 0, isr_stub_23 },
    { 24, 0, isr_stub_24 }, { 25, 0, isr_stub_25 },
    { 26, 0, isr_stub_26 }, { 27, 0, isr_stub_27 },
    { 28, 0, isr_stub_28 }, { 29, 0, isr_stub_29 },
    { 30, 0, isr_stub_30 }, { 31, 0, isr_stub_31 },
    { 32, 0, isr_stub_32 }, { 33, 0, isr_stub_33 },
    { 34, 0, isr_stub_34 }, { 35, 0, isr_stub_35 },
    { 36, 0, isr_stub_36 }, { 37, 0, isr_stub_37 },
    { 38, 0, isr_stub_38 }, { 39, 0, isr_stub_39 },
    { 40, 0, isr_stub_40 }, { 41, 0, isr_stub_41 },
    { 42, 0, isr_stub_42 }, { 43, 0, isr_stub_43 },
    { 44, 0, isr_stub_44 }, { 45, 0, isr_stub_45 },
    { 46, 0, isr_stub_46 }, { 47, 0, isr_stub_47 },
};

/* --- TSS ---------------------------------------------------------------- */

tss_t kernel_tss;

/* Stacks for the IST entries, allocated once at boot. */
#define IST_STACK_SIZE 8192
static uint8_t ist_stack_1[IST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t ist_stack_2[IST_STACK_SIZE] __attribute__((aligned(16)));

void tss_init(void) {
    memzero(&kernel_tss, sizeof(kernel_tss));
    /* iomap_base past our GDT limit means "no I/O permission bitmap". */
    kernel_tss.iomap_base = sizeof(tss_t);
    kernel_tss.ist[0] = (uint64_t)ist_stack_1 + IST_STACK_SIZE; /* ist1 */
    kernel_tss.ist[1] = (uint64_t)ist_stack_2 + IST_STACK_SIZE; /* ist2 */
}

void tss_set_rsp0(uint64_t rsp) {
    kernel_tss.rsp[0] = rsp;
}

void tss_set_ist(int ist, uint64_t rsp) {
    if (ist >= 1 && ist <= 7)
        kernel_tss.ist[ist - 1] = rsp;
}

/* The 16-byte form of a 64-bit TSS descriptor.
 *
 * The upper quadword is not a descriptor in its own right: it holds base
 * bits 63:32 followed by a reserved double word that the CPU requires to be
 * zero.  Encoding it like an ordinary descriptor shifts those fields, and
 * `ltr` then fails with #GP reporting the TSS selector.
 */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;       /* bits 15:0   */
    uint16_t base_low;        /* base 15:0   */
    uint8_t  base_mid;        /* base 23:16  */
    uint8_t  access;          /* 0x89: present, DPL 0, type 9 */
    uint8_t  limit_high;      /* limit 19:16 and flags */
    uint8_t  base_high;       /* base 31:24  */
    uint32_t base_upper;      /* base 63:32  */
    uint32_t reserved;        /* must be 0   */
} __attribute__((packed)) tss_desc_t;

static tss_desc_t tss_desc;

static void write_tss_descriptor(uint64_t base, uint32_t limit) {
    tss_desc.limit_low   = (uint16_t)(limit & 0xffff);
    tss_desc.base_low    = (uint16_t)(base & 0xffff);
    tss_desc.base_mid    = (uint8_t)((base >> 16) & 0xff);
    tss_desc.access      = 0x89;   /* present, DPL 0, system, available 64-bit TSS */
    tss_desc.limit_high  = (uint8_t)((limit >> 16) & 0x0f);
    tss_desc.base_high   = (uint8_t)((base >> 24) & 0xff);
    tss_desc.base_upper  = (uint32_t)(base >> 32);
    tss_desc.reserved    = 0;

    memcpy(&gdt[GDT_TSS >> 3], &tss_desc, sizeof(tss_desc));
}

/* Load the task register, which points at the TSS descriptor's 16-bit
 * (limit, base) form held in the GDT.  The instruction only accepts a 16-bit
 * register or a memory operand, so the selector goes in through AX. */
static inline void ltr(uint16_t selector) {
    __asm__ volatile("ltr %%ax" :: "a"(selector) : "memory");
}

void gdt_init(void) {
    boot_trace('[');
    memzero(gdt, sizeof(gdt));

    /* 0x00: null descriptor, left zeroed. */
    gdt_encode(&gdt[GDT_KERNEL_CODE >> 3], 0, 0xfffff,
               GDT_ACCESS_KERNEL_CODE, GDT_FLAG_GRANULARITY | GDT_FLAG_LONG_MODE);
    gdt_encode(&gdt[GDT_KERNEL_DATA >> 3], 0, 0xfffff,
               GDT_ACCESS_KERNEL_DATA, GDT_FLAG_GRANULARITY);
    gdt_encode(&gdt[GDT_USER_DATA >> 3], 0, 0xfffff,
               GDT_ACCESS_USER_DATA, GDT_FLAG_GRANULARITY);
    gdt_encode(&gdt[GDT_USER_CODE >> 3], 0, 0xfffff,
               GDT_ACCESS_USER_CODE, GDT_FLAG_GRANULARITY | GDT_FLAG_LONG_MODE);

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)gdt;

    boot_trace('|');
    lgdt(&gdt_ptr);
    boot_trace('|');
    gdt_set_kernel_table();
    boot_trace('|');

    /* The TSS occupies two GDT slots; fill them in after the base table is
     * live so the reload below also refreshes them. */
    tss_init();
    boot_trace('|');
    /* The TSS descriptor spills into two GDT slots; both must be written
     * before the reload below refreshes the CPU's cache of the table. */
    write_tss_descriptor((uint64_t)&kernel_tss, sizeof(kernel_tss) - 1);
    gdt_set_kernel_table();
    boot_trace('|');
    ltr(GDT_TSS);
    boot_trace(']');
}

void gdt_set_kernel_table(void) {
    __asm__ volatile(
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        ::: "rax", "memory");
}

void gdt_set_user_table(void) {
    __asm__ volatile(
        "movw $0x18, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "pushq $0x20\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        ::: "rax", "memory");
}

/* Refresh the TSS descriptors after the descriptor table is reloaded (for
 * example when an AP installs its own copy). */
void gdt_reload(void) {
    gdt_set_kernel_table();
    ltr(GDT_TSS);
}

void idt_init(void) {
    memzero(&idt, sizeof(idt));

    for (size_t i = 0; i < array_len(exception_table); i++) {
        idt_set_gate(exception_table[i].vector, exception_table[i].handler,
                     exception_table[i].ist, 0);
    }

    idt.ptr.limit = sizeof(idt.entries) - 1;
    idt.ptr.base = (uint64_t)idt.entries;
    lidt(&idt.ptr);
}

void idt_set_gate(int vector, void *handler, uint8_t ist, uint8_t dpl) {
    if (vector < 0 || vector > 255)
        return;

    uint64_t addr = (uint64_t)handler;
    idt_entry_t *e = &idt.entries[vector];

    e->offset_low  = (uint16_t)(addr & 0xffff);
    e->selector    = GDT_KERNEL_CODE;
    e->ist         = ist & 0x7;
    /* 0x8E = present, DPL, 64-bit interrupt gate (clears IF on entry). */
    e->type_attr   = (uint8_t)(0x8e | ((dpl & 0x3) << 5));
    e->offset_mid  = (uint16_t)((addr >> 16) & 0xffff);
    e->offset_high = (uint32_t)(addr >> 32);
    e->reserved    = 0;
}