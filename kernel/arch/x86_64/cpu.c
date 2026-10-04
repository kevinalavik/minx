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

/* The table has to hold one TSS descriptor per processor, each 16 bytes, so it is
 * sized from MAX_CPU_COUNT rather than from the five descriptors the base table
 * needs.  A single shared TSS descriptor is not enough: loading a task register
 * marks its descriptor busy, and the second processor to try gets #GP. */
#define GDT_ENTRIES ((GDT_TSS >> 3) + 2 * MAX_CPU_COUNT)

static gdt_entry_t gdt[GDT_ENTRIES];

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
extern void isr_stub_48(void);
extern void isr_stub_49(void);
extern void isr_stub_50(void);
extern void isr_stub_51(void);
extern void isr_stub_52(void);
extern void isr_stub_53(void);
extern void isr_stub_54(void);
extern void isr_stub_55(void);
extern void isr_stub_56(void);
extern void isr_stub_57(void);
extern void isr_stub_58(void);
extern void isr_stub_59(void);
extern void isr_stub_60(void);
extern void isr_stub_61(void);
extern void isr_stub_62(void);
extern void isr_stub_63(void);

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
    { 48, 0, isr_stub_48 }, { 49, 0, isr_stub_49 },
    { 50, 0, isr_stub_50 },
    { 51, 0, isr_stub_51 },
    { 52, 0, isr_stub_52 },
    { 53, 0, isr_stub_53 },
    { 54, 0, isr_stub_54 },
    { 55, 0, isr_stub_55 },
    { 56, 0, isr_stub_56 },
    { 57, 0, isr_stub_57 },
    { 58, 0, isr_stub_58 },
    { 59, 0, isr_stub_59 },
    { 60, 0, isr_stub_60 },
    { 61, 0, isr_stub_61 },
    { 62, 0, isr_stub_62 },
    { 63, 0, isr_stub_63 },
};

/* --- TSS ---------------------------------------------------------------- */

/* One TSS per processor.  Index 0 is the boot processor's; the rest are filled
 * in as each application processor is handed its entry point, because the IST
 * stacks have to be somewhere its fault handler can use before it has an address
 * space of its own. */
tss_t kernel_tss[MAX_CPU_COUNT];

/* Stacks for the boot processor's IST entries.  Application processors get their
 * own from the heap at hand-off -- see smp_start_all() -- because these are
 * static and there is only one of everything here. */
static uint8_t ist_stack_1[IST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t ist_stack_2[IST_STACK_SIZE] __attribute__((aligned(16)));

void tss_init_for(unsigned index, uint64_t ist1, uint64_t ist2) {
    if (index >= MAX_CPU_COUNT)
        return;

    tss_t *tss = &kernel_tss[index];
    memzero(tss, sizeof(*tss));
    /* iomap_base past the TSS limit means "no I/O permission bitmap". */
    tss->iomap_base = sizeof(tss_t);
    if (ist1 != 0)
        tss->ist[0] = ist1;    /* ist1 */
    if (ist2 != 0)
        tss->ist[1] = ist2;    /* ist2 */
}

void tss_init(void) {
    tss_init_for(0, (uint64_t)ist_stack_1 + IST_STACK_SIZE,
                 (uint64_t)ist_stack_2 + IST_STACK_SIZE);
}

void tss_set_rsp0(uint64_t rsp) {
    kernel_tss[0].rsp[0] = rsp;
}

void tss_set_ist(int ist, uint64_t rsp) {
    if (ist >= 1 && ist <= 7)
        kernel_tss[0].ist[ist - 1] = rsp;
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

/* Publish processor `index`'s TSS in the table.
 *
 * Every processor gets a slot of its own, and `access` starts out at 0x89 --
 * available, not busy.  The CPU flips bit 0 of the type field to 1 in the table
 * itself when it loads the register, so writing 0x89 for a second processor that
 * wants the same entry would be refused with #GP. */
void gdt_write_tss_descriptor(unsigned index, uint64_t base, uint32_t limit) {
    if (index >= MAX_CPU_COUNT)
        return;

    tss_desc.limit_low   = (uint16_t)(limit & 0xffff);
    tss_desc.base_low    = (uint16_t)(base & 0xffff);
    tss_desc.base_mid    = (uint8_t)((base >> 16) & 0xff);
    tss_desc.access      = 0x89;   /* present, DPL 0, system, available 64-bit TSS */
    tss_desc.limit_high  = (uint8_t)((limit >> 16) & 0x0f);
    tss_desc.base_high   = (uint8_t)((base >> 24) & 0xff);
    tss_desc.base_upper  = (uint32_t)(base >> 32);
    tss_desc.reserved    = 0;

    memcpy(&gdt[gdt_tss_selector(index) >> 3], &tss_desc, sizeof(tss_desc));
}

/* Load the task register, which points at the TSS descriptor's 16-bit
 * (limit, base) form held in the GDT.  The instruction only accepts a 16-bit
 * register or a memory operand, so the selector goes in through AX. */
static inline void ltr(uint16_t selector) {
    /* LTR takes a 16-bit register operand and nothing else.  Writing it as
     * `ltr %ax` looks right and is not: neither assembler honours an operand-size
     * suffix on this instruction, and both emit the 32-bit form, which is
     * undefined and raises #GP with the selector as the error code.  The prefix
     * has to be written out.
     *
     * 0x0f 0x00 /3 with mod=11 and r/m=EAX; the /3 is the important part of the
     * last byte, since /2 in the same place is LGDT. */
    __asm__ volatile(".byte 0x66, 0x0f, 0x00, 0xd8"
                     :: "a"((uint64_t)selector) : "memory");
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
    gdt_write_tss_descriptor(0, (uint64_t)&kernel_tss[0],
                             sizeof(kernel_tss[0]) - 1);
    gdt_set_kernel_table();
    boot_trace('|');
    ltr(gdt_tss_selector(0));
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

/* Refresh the descriptor tables after they have been changed (for example when
 * the TSS descriptor is rewritten), and reload this processor's view of them.
 *
 * The LGDT has to be part of this, not just the segment reload.  A processor that
 * starts from the bootloader's handoff is still using the *bootloader's*
 * descriptor table, and a far return to a selector that only exists in ours finds
 * whatever the bootloader's table has at that index instead -- which on this
 * machine is a 32-bit code segment, so the processor drops into compatibility mode
 * on the first instruction after the reload and faults on the next fetch.  The
 * segments and the task register both come after the table, not instead of it. */
void gdt_reload(void) {
    lgdt(&gdt_ptr);
    gdt_set_kernel_table();
}

/* The table, the segments, and processor `index`'s task register.
 *
 * Each processor needs a TSS descriptor of its own: loading a task register marks
 * the descriptor busy *in the table*, so a second processor that runs `ltr` on the
 * same entry is refused with #GP and the selector as the error code. */
void gdt_reload_for(unsigned index) {
    gdt_reload();
    ltr(gdt_tss_selector(index));
}

/* Decode the task state segment descriptor published for processor `index`.
 *
 * Returns false if the index is out of range, or if the entry in the table is
 * not a 64-bit TSS descriptor at all -- which is what the table looks like
 * before `gdt_write_tss_descriptor()` has run for it, and what it stops looking
 * like if a processor's descriptor is written at the wrong slot. */
bool gdt_tss_descriptor_for(unsigned index, uint64_t *base, uint32_t *limit,
                            uint8_t *type) {
    if (index >= MAX_CPU_COUNT)
        return false;

    const tss_desc_t *d = (const tss_desc_t *)&gdt[gdt_tss_selector(index) >> 3];
    uint8_t access = d->access;

    /* Present, ring 0, and a system segment of type 9 (available 64-bit TSS) or
     * 11 (busy 64-bit TSS).  11 is what the entry becomes once a processor has
     * loaded it, which is normal; anything else is not a TSS. */
    if ((access & 0xf) != 0x9 && (access & 0xf) != 0xb)
        return false;
    if ((access & 0xf0) != 0x80)
        return false;

    if (base != NULL)
        *base = (uint64_t)d->base_low | ((uint64_t)d->base_mid << 16) |
                ((uint64_t)d->base_high << 24) |
                ((uint64_t)d->base_upper << 32);
    if (limit != NULL)
        *limit = (uint32_t)d->limit_low | ((uint32_t)d->limit_high << 16);
    if (type != NULL)
        *type = access & 0xf;

    return true;
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

/* LIDT and LGDT are per-processor state, not part of the table itself, so a CPU
 * that comes up later has to be told where the tables are even though the
 * kernel only ever fills them in once.  Without this an application processor
 * takes its first interrupt with no IDT at all and double-faults. */
void idt_reload(void) {
    lidt(&idt.ptr);
}

/* A copy of the GDT pseudo-descriptor.  The application-processor trampoline
 * runs with paging off and has to load the table itself, and it cannot reach
 * this static in C, so the values are handed to it as plain integers. */
dt_ptr_t gdt_descriptor(void) {
    return gdt_ptr;
}

/* A copy of the IDT pseudo-descriptor, for the same reason. */
void idt_descriptor(dt_ptr_t *out) {
    *out = idt.ptr;
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