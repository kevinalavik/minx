#include "apic.h"

#include "acpi.h"
#include "io.h"
#include "kprintf.h"
#include "pagemap.h"
#include "panic.h"
#include "types.h"

/* Legacy 8259 PIC ports. */
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xa0
#define PIC2_DATA 0xa1

#define PIC_EOI   0x20

#define PIC1_OFFSET_BASE 32
#define PIC2_OFFSET_BASE 40

/* The APIC windows are either the "real" addresses below or the processor's
 * 0xFEE00000 window, which every x86 with an APIC implements. */
#define LAPIC_PHYS_DEFAULT 0xfee00000u
#define IOAPIC_PHYS_DEFAULT 0xfec00000u
#define MSR_APIC_BASE 0x1b

static bool lapic_ok;
static uint32_t lapic_phys = LAPIC_PHYS_DEFAULT;
static uint8_t *lapic_win;

static unsigned ioapic_n;
static uint8_t *ioapic_win[MAX_IOAPIC_COUNT];
static uint32_t ioapic_gsi[MAX_IOAPIC_COUNT];
static uint32_t ioapic_entries[MAX_IOAPIC_COUNT];

static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(lapic_win + reg);
}

static inline void lapic_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(lapic_win + reg) = value;
}

uint32_t lapic_read_reg(uint32_t reg) {
    return lapic_ok ? lapic_read(reg) : 0;
}

void lapic_write_reg(uint32_t reg, uint32_t value) {
    if (lapic_ok)
        lapic_write(reg, value);
}

bool lapic_available(void) { return lapic_ok; }

uint32_t lapic_id(void) {
    return lapic_ok ? (lapic_read(LAPIC_ID) >> 24) : 0;
}

int lapic_init(void) {
    /* Which physical address the APIC lives at, in order of preference. */
    uint32_t candidates[3];
    unsigned ncandidates = 0;

    uint64_t msr = rdmsr(MSR_APIC_BASE);
    uint32_t from_msr = (uint32_t)(msr & 0xfffff000u);

    if (acpi_has_lapic())
        candidates[ncandidates++] = (uint32_t)acpi_lapic_address();
    if (from_msr != 0)
        candidates[ncandidates++] = from_msr;
    candidates[ncandidates++] = LAPIC_PHYS_DEFAULT;

    /* The higher-half direct map covers RAM but deliberately excludes MMIO, so
     * the APIC may only be reachable through the identity map until the kernel
     * builds its own page tables.  Probe rather than assume: a stray access to
     * an unmapped window would be a triple fault. */
    uint32_t chosen = 0;
    uint64_t chosen_virt = 0;
    bool found = false;

    for (unsigned i = 0; i < ncandidates; i++) {
        uint64_t virt;
        if (candidates[i] != 0 && vmm_find_mapping(candidates[i], &virt)) {
            chosen = candidates[i];
            chosen_virt = virt;
            found = true;
            break;
        }
    }

    if (!found) {
        kprintf("[warn] no usable mapping for the local APIC; it will stay "
                "uninitialised\n");
        lapic_ok = false;
        return -1;
    }

    lapic_phys = chosen;
    lapic_win = (uint8_t *)chosen_virt;

    /* Reading LAPIC_ID at 0xFEE00000 without an APIC returns all ones, which is
     * how a machine with no local APIC looks. */
    uint32_t id = lapic_read(LAPIC_ID) >> 24;
    if (id == 0xff) {
        kprintf("[warn] no local APIC at %#x (via %#lx); continuing without "
                "one\n", lapic_phys, chosen_virt);
        lapic_ok = false;
        return -1;
    }

    lapic_ok = true;

    kprintf("LAPIC: physical %#x mapped at %#lx, id %u, %s\n", lapic_phys,
            chosen_virt, id,
            (msr & (1ull << 11)) ? "x2APIC" : "xAPIC");

    /* Enable the APIC and accept spurious interrupts at vector 0xff. */
    uint32_t svr = lapic_read(LAPIC_SVR);
    lapic_write(LAPIC_SVR, svr | LAPIC_SVR_ENABLE | (0xffu << 8) |
                          LAPIC_SVR_SPURIOUS);

    /* All interrupts are delivered on the bootstrap processor by default. */
    lapic_write(LAPIC_TPR, 0);

    /* Clear any pending EOI from a warm boot. */
    lapic_write(LAPIC_EOI, 0);

    return 0;
}

void lapic_eoi(void) {
    if (lapic_ok)
        lapic_write(LAPIC_EOI, 0);
}

void lapic_set_priority(uint8_t priority) {
    if (lapic_ok)
        lapic_write(LAPIC_TPR, priority);
}

void lapic_ipi(uint8_t target_lapic, uint8_t vector) {
    if (!lapic_ok)
        return;
    lapic_write(LAPIC_ICR_HI, (uint32_t)target_lapic << 24);
    /* Fixed delivery, physical destination, assert, edge triggered. */
    lapic_write(LAPIC_ICR_LO, 0x00004500u | vector);

    /* Wait for the delivery status bit to clear. */
    unsigned spins = 0;
    while ((lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_DELIVS) != 0) {
        if (++spins > 100000u) {
            kprintf("[warn] LAPIC IPI to %u (vector %u) never completed\n",
                    target_lapic, vector);
            break;
        }
        pause_cpu();
    }
}

void lapic_ipi_all_cpus(uint8_t vector) {
    if (!lapic_ok)
        return;
    /* Delivery mode "send to all including self". */
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO, 0x00004800u | vector);

    unsigned spins = 0;
    while ((lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_DELIVS) != 0) {
        if (++spins > 100000u)
            break;
        pause_cpu();
    }
}

void lapic_ipi_all_but_self(uint8_t vector) {
    if (!lapic_ok)
        return;
    /* Shorthand for "all excluding self": x2APIC-only on some parts, so it is
     * only used where the destination id has been left at 0. */
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO, 0x00004000u | vector);

    unsigned spins = 0;
    while ((lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_DELIVS) != 0) {
        if (++spins > 100000u)
            break;
        pause_cpu();
    }
}

/* --- IOAPIC -------------------------------------------------------------- */

void ioapic_write(uint32_t index, uint32_t value) {
    outb(IOAPIC_REGSEL, (uint8_t)index);
    outl(IOAPIC_WINDOW, value);
}

uint32_t ioapic_read(uint32_t index) {
    outb(IOAPIC_REGSEL, (uint8_t)index);
    return inl(IOAPIC_WINDOW);
}

unsigned ioapic_count(void) {
    return ioapic_n;
}

uint32_t ioapic_max_entries(unsigned index) {
    if (index >= ioapic_n)
        return 0;
    return (ioapic_read(1) >> 16) & 0xff;
}

int ioapic_init(void) {
    unsigned count = 0;
    const acpi_ioapic_t *list = acpi_ioapic_table(&count);

    if (count == 0) {
        kprintf("[warn] ACPI listed no IOAPIC\n");
        return -1;
    }

    for (unsigned i = 0; i < count && i < MAX_IOAPIC_COUNT; i++) {
        uint32_t phys = list[i].base != 0 ? list[i].base
                                          : IOAPIC_PHYS_DEFAULT + i * 0x1000;

        uint64_t virt;
        if (!vmm_find_mapping(phys, &virt)) {
            kprintf("IOAPIC %u at %#x is not mapped; skipping it\n", i, phys);
            continue;
        }

        ioapic_win[i] = (uint8_t *)virt;
        ioapic_gsi[i] = list[i].gsi_base;
        ioapic_entries[i] = (ioapic_read(ioapic_n * 2 + 1) >> 16) & 0xff;
        ioapic_n++;
        acpi_note_ioapic_entries(i, ioapic_entries[i]);

        kprintf("IOAPIC %u: physical %#x at %#lx, GSI base %u, %u entries\n", i,
                phys, virt, ioapic_gsi[i], ioapic_entries[i]);

        if (ioapic_entries[i] == 0) {
            kprintf("[warn] IOAPIC %u reports no entries; disabling it\n", i);
            ioapic_n--;
        }
    }

    if (ioapic_n == 0)
        return -1;

    /* Mask everything until a driver routes the lines it owns. */
    for (unsigned i = 0; i < ioapic_n; i++) {
        for (uint32_t e = 0; e < 24 && e < ioapic_entries[i]; e++)
            ioapic_write(i * 2 + 0x10 + e * 2, 1u << 16);
    }

    return 0;
}

/* Redirection table entry layout: 64 bits, two 32-bit halves. */
#define IOAPIC_REDIR_MASK_PRESENT   (1u << 0)
#define IOAPIC_REDIR_MASK_LEVEL     (1u << 8)
#define IOAPIC_REDIR_MASK_ACTIVE_HI (1u << 9)
#define IOAPIC_REDIR_MASK_ACTIVE_LO (1u << 10)

int ioapic_route(unsigned ioapic, uint8_t vector, uint8_t cpu, bool level) {
    if (ioapic >= ioapic_n)
        return -1;

    uint32_t entry = ioapic_gsi[ioapic] + vector;
    uint32_t index = ioapic * 2 + 2 * (entry & 0x7f);

    uint32_t low = IOAPIC_REDIR_MASK_PRESENT | vector;
    if (level)
        low |= IOAPIC_REDIR_MASK_LEVEL;

    /* Destination: physical destination mode with the APIC id in the high half,
     * and a destination-enabled bit so the interrupt is actually delivered. */
    uint32_t high = ((uint32_t)cpu & 0xff) << 24;

    ioapic_write(index, low);
    ioapic_write(index + 1, high);
    return 0;
}

/* --- legacy 8259 PICs ---------------------------------------------------- */

static uint16_t pic_mask = 0xffff;

int pic_init(void) {
    /* Mask both chips before touching them: an unexpected IRQ7 during init
     * would land on a vector we have not set up. */
    outb(PIC1_DATA, 0xff);
    outb(PIC2_DATA, 0xff);

    /* ICW1: expect ICW4, start initialisation. */
    outb(PIC1_CMD, 0x11);
    io_wait();
    outb(PIC2_CMD, 0x11);

    /* ICW2: vector offsets, chosen so no PIC line collides with the CPU's own
     * exception vectors or with the legacy IRQ stubs at 32..47. */
    outb(PIC1_DATA, PIC1_OFFSET_BASE);
    io_wait();
    outb(PIC2_DATA, PIC2_OFFSET_BASE);

    /* ICW3: cascade wiring -- the slave is on IRQ2 of the master. */
    outb(PIC1_DATA, 1 << 2);
    io_wait();
    outb(PIC2_DATA, 2);

    /* ICW4: 8086 mode, normal (non-automatic) end of interrupt. */
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);

    pic_mask = 0xffff;
    kprintf("PIC: remapped to vectors %u..%u and %u..%u\n",
            PIC1_OFFSET_BASE, PIC1_OFFSET_BASE + 7,
            PIC2_OFFSET_BASE, PIC2_OFFSET_BASE + 7);
    return 0;
}

void pic_set_mask(uint16_t mask) {
    pic_mask = mask;
    outb(PIC1_DATA, (uint8_t)(mask & 0xff));
    outb(PIC2_DATA, (uint8_t)((mask >> 8) & 0xff));
}

/* Read the mask back from the chips rather than from the cached copy: a write
 * that the hardware ignored must be visible here, because otherwise the rest of
 * the kernel would believe an IRQ is unmasked when it is not. */
uint16_t pic_irq_mask(void) {
    pic_mask = (uint16_t)inb(PIC1_DATA) | ((uint16_t)inb(PIC2_DATA) << 8);
    return pic_mask;
}

void pic_mask_all(void) {
    pic_set_mask(0xffff);
}

void pic_unmask_all(void) {
    pic_set_mask(0x0000);
}

void pic_remap(uint8_t master_offset, uint8_t slave_offset) {
    outb(PIC1_CMD, 0x11);
    io_wait();
    outb(PIC2_CMD, 0x11);
    outb(PIC1_DATA, master_offset);
    io_wait();
    outb(PIC2_DATA, slave_offset);
    outb(PIC1_DATA, 1 << 2);
    io_wait();
    outb(PIC2_DATA, 2);
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
}

void pic_disable(void) {
    /* Once the IOAPIC owns the lines, the 8259s can never be unmasked again. */
    pic_mask_all();
    kprintf("PIC: fully masked; the IOAPIC owns every interrupt line\n");
}