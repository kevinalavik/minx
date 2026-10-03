#include "apic.h"

#include "acpi.h"
#include "io.h"
#include "kprintf.h"
#include "pagemap.h"
#include "panic.h"
#include "types.h"

/* The redirection entry's bit layout lives in apic.h, next to the register
 * indices; it is the one thing both the routing code and the end-of-interrupt
 * path have to agree on. */

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
static uint8_t ioapic_cpu;

/* How each of the sixteen legacy lines is programmed, so masking and
 * acknowledging can find its redirection entry without recomputing anything. */
static struct {
    bool     routed;
    bool     level;
    uint8_t  vector;
    uint8_t  cpu;
    uint32_t index;   /* low half's register index in chip 0's window */
} legacy_lines[16];

/* Which IOAPIC pin each legacy ISA IRQ is actually wired to.
 *
 * The architectural expectation is the identity map -- pin n is an alias of ISA
 * IRQ n -- and on real hardware that is what the MADT's IOAPIC entry describes.
 * It is not something the architecture guarantees, and QEMU does not honour it:
 * its IOAPIC model sends ISA IRQ0 (the 8253) to *pin 2* rather than pin 0, so a
 * kernel that programs pin 0 for the timer and pin 2 for IRQ2 sees the clock
 * arrive on IRQ2's vector and never reaches its own handler.  The two things
 * are indistinguishable from the guest side -- the vector is the only thing
 * delivered -- so assuming the identity map is a silent misroute.
 *
 * The 8253 is the only legacy device whose interrupt line the kernel can raise
 * on demand, which makes it the one line that can be found rather than assumed;
 * ioapic_probe_legacy_pins() in timer.c uses it for that.  Everything else keeps
 * the identity map, which is what every machine the kernel has run on does. */
static uint8_t isa_pin[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

/* A line whose pin was taken over by the probe has none.  Pin 2 on such a
 * machine is shared by the 8253's IRQ0 and the 8259 cascade, and only one of
 * them can have the entry: the cascade is meaningless once the 8259s are masked
 * for good and the IOAPIC owns delivery, so the clock gets it. */
#define ISA_PIN_NONE 0xffu

unsigned ioapic_legacy_pin(unsigned irq) {
    return (irq < 16) ? isa_pin[irq] : irq;
}

bool ioapic_legacy_line_routable(unsigned irq) {
    return irq < 16 && isa_pin[irq] != ISA_PIN_NONE;
}

void ioapic_set_legacy_pin(unsigned irq, unsigned pin) {
    if (irq >= 16 || pin >= 16)
        return;

    /* Whatever line was on that pin has been displaced.  Two lines cannot share
     * a redirection entry, and the one programmed last would win silently --
     * which is how a 100 Hz clock ends up counted as somebody else's interrupt
     * forever. */
    for (unsigned other = 0; other < 16; other++)
        if (other != irq && isa_pin[other] == pin)
            isa_pin[other] = ISA_PIN_NONE;

    isa_pin[irq] = (uint8_t)pin;
}

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
    lapic_write(LAPIC_SVR, svr | LAPIC_SVR_ENABLE | (0xffu << 8));

    /* All interrupts are delivered on the bootstrap processor by default. */
    lapic_write(LAPIC_TPR, 0);

    /* Clear any pending EOI from a warm boot. */
    lapic_write(LAPIC_EOI, 0);

    lapic_unmask_local_vectors();

    return 0;
}

/* Configure the local vector table.
 *
 * Limine masks every LVT entry before handing over and leaves LINT0 in ExtINT
 * mode, and both have to be dealt with before a single device interrupt can
 * arrive:
 *
 *   - the mask bit on LINT0 is what actually stops delivery, and
 *   - ExtINT delivery makes the local APIC take the vector from the LVT entry
 *     and ignore the one the IOAPIC put in the message, so every device line
 *     would arrive on the same vector and the dispatcher could not tell a
 *     keystroke from a clock tick.
 *
 * So LINT0 is unmasked and put into fixed delivery, which is what makes the
 * per-line IOAPIC redirection table meaningful; its own vector field is only a
 * fallback for a machine that delivers something with no vector in the message.
 * LINT1 is unused and stays masked.  The error entry is pointed at the kernel's
 * error vector so an unexpected APIC error is reportable, but it stays masked.
 * Thermal, performance-monitoring and CMCI stay masked because nothing has
 * claimed them.
 */
void lapic_unmask_local_vectors(void) {
    if (!lapic_ok)
        return;

    uint32_t lint0 = lapic_read(LAPIC_LVT_LINT0);

    lint0 &= ~(LAPIC_LVT_MASKED | (0x7u << 8) | 0xffu);
    lint0 |= (uint32_t)(VECTOR_IRQ0 & 0xff);
    lapic_write(LAPIC_LVT_LINT0, lint0);

    lapic_write(LAPIC_LVT_LINT1, lapic_read(LAPIC_LVT_LINT1) | LAPIC_LVT_MASKED);

    lapic_write(LAPIC_LVT_ERROR,
                (lapic_read(LAPIC_LVT_ERROR) & ~0x1ffu) |
                    (uint32_t)(VECTOR_LAPIC_ERROR & 0xff) | LAPIC_LVT_MASKED);

    lapic_write(LAPIC_LVT_CMCI, lapic_read(LAPIC_LVT_CMCI) | LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, lapic_read(LAPIC_LVT_THERMAL) | LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PMC, lapic_read(LAPIC_LVT_PMC) | LAPIC_LVT_MASKED);

    kprintf("LAPIC: LVTs configured (LINT0 vector %u, fixed delivery; error "
            "vector %u)\n", (unsigned)(lapic_read(LAPIC_LVT_LINT0) & 0xff),
            (unsigned)VECTOR_LAPIC_ERROR);
}

void lapic_eoi(void) {
    if (lapic_ok)
        lapic_write(LAPIC_EOI, 0);
}

/* Spin until the ICR's delivery-status bit clears, which is the hardware's own
 * confirmation that the interrupt has been sent. */
static void ipi_wait(void) {
    unsigned spins = 0;
    while ((lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_DELIVS) != 0) {
        if (++spins > 1000000u) {
            kprintf("[warn] LAPIC IPI delivery status never cleared\n");
            return;
        }
        pause_cpu();
    }
}

void lapic_ipi(uint8_t target_lapic, uint8_t vector) {
    if (!lapic_ok)
        return;
    /* Fixed delivery (bits 8-10 = 000), physical destination (bit 11 clear),
     * assert (bit 14), edge triggered (bit 15 clear). */
    lapic_write(LAPIC_ICR_HI, (uint32_t)target_lapic << 24);
    lapic_write(LAPIC_ICR_LO, LAPIC_ICR_ASSERT | (vector & 0xffu));
    ipi_wait();
}

void lapic_ipi_all_cpus(uint8_t vector) {
    if (!lapic_ok)
        return;
    /* Destination shorthand 11 (bits 18-19): all including self.  This is
     * shorthand rather than a broadcast, so it reaches application processors
     * that have their own local APIC address, which a literal 0xff would. */
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO,
                LAPIC_ICR_ALL_CPUS | LAPIC_ICR_ASSERT | (vector & 0xffu));
    ipi_wait();
}

void lapic_ipi_all_but_self(uint8_t vector) {
    if (!lapic_ok)
        return;
    /* Shorthand 10 (bits 18-19): all excluding self. */
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO,
                LAPIC_ICR_ALL_BUT_SELF | LAPIC_ICR_ASSERT | (vector & 0xffu));
    ipi_wait();
}

void lapic_ipi_init(uint8_t target_lapic) {
    if (!lapic_ok)
        return;
    /* Delivery mode INIT (bits 8-10 = 100).  The INIT/SIPI sequence is the only
     * way an application processor can be started once it is running. */
    lapic_write(LAPIC_ICR_HI, (uint32_t)target_lapic << 24);
    lapic_write(LAPIC_ICR_LO,
                LAPIC_ICR_ASSERT | ((uint32_t)LAPIC_DM_INIT << 8));
    ipi_wait();
}

void lapic_ipi_startup(uint8_t target_lapic, uint8_t vector) {
    if (!lapic_ok)
        return;
    /* Delivery mode Startup (bits 8-10 = 101).  Only bits 7..0 of the ICR are the
     * vector for INIT/SIPI; everything above is ignored. */
    lapic_write(LAPIC_ICR_HI, (uint32_t)target_lapic << 24);
    lapic_write(LAPIC_ICR_LO,
                LAPIC_ICR_ASSERT | ((uint32_t)LAPIC_DM_SIPI << 8) |
                    (vector & 0xffu));
    ipi_wait();
}

/* --- IOAPIC -------------------------------------------------------------- */

/* The IOAPIC indirect register file: one 32-bit register selects the register and
 * the next holds its value.  Both live in the chip's own MMIO window, at offsets
 * 0x00 and 0x10, so they are reached through the mapping the virtual memory
 * manager installed -- not through I/O ports.  Reading the index register through
 * a port writes that port's device instead, and the chip then appears to ignore
 * every mask change, which is exactly the symptom that made the lines look
 * permanently blocked.
 *
 * The window is uncacheable (mapped that way for exactly this reason), so no
 * explicit barrier is needed around the pair of accesses. */
void ioapic_write(uint32_t index, uint32_t value) {
    volatile uint32_t *regs = (volatile uint32_t *)ioapic_win[0];

    regs[IOAPIC_REGSEL / 4] = index;
    regs[IOAPIC_WINDOW / 4] = value;
}

uint32_t ioapic_read(uint32_t index) {
    volatile uint32_t *regs = (volatile uint32_t *)ioapic_win[0];

    regs[IOAPIC_REGSEL / 4] = index;
    return regs[IOAPIC_WINDOW / 4];
}

unsigned ioapic_count(void) {
    return ioapic_n;
}

/* Number of redirection entries this chip has.
 *
 * The version register reports the highest valid entry *index*, so the count is
 * one more than that.  The check is on the mapped window rather than on how many
 * chips have been counted so far, because ioapic_init() asks this question while
 * it is still programming the first chip.
 */
uint32_t ioapic_max_entries(unsigned index) {
    if (index >= MAX_IOAPIC_COUNT || ioapic_win[index] == NULL)
        return 0;
    return ((ioapic_read(IOAPIC_REG_VER) >> 16) & 0xff) + 1;
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
        ioapic_entries[i] = ioapic_max_entries(i);
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
            ioapic_write(i * 2 + IOAPIC_REDIR_BASE + e * 2, IOAPIC_REDIR_MASKED);
    }

    return 0;
}

/* Program one redirection entry.
 *
 * `line` is a chip pin (a GSI, or an ISA IRQ number for the pins that alias the
 * ISA lines) and `vector` is the vector the CPU should take when it fires.  They
 * are separate arguments because they are separate things: the pin selects the
 * entry, and the vector only fills in the low eight bits of it.  Conflating the
 * two writes the entry at gsi_base + vector, which for a vector in the device
 * range lands past the end of the table and is silently dropped by the chip --
 * the kernel then believes every line is routed and nothing arrives. */
int ioapic_route(unsigned ioapic, unsigned line, uint8_t vector, uint8_t cpu,
                 bool level) {
    if (ioapic >= ioapic_n)
        return -1;

    uint32_t entry = ioapic_gsi[ioapic] + line;
    if (entry >= ioapic_entries[ioapic])
        return -1;

    uint32_t index = ioapic * 2 + IOAPIC_REDIR_BASE + 2 * entry;

    uint32_t low = vector & IOAPIC_REDIR_VECTOR_MASK;
    if (level)
        low |= IOAPIC_REDIR_LEVEL;

    /* Destination: physical destination mode with the APIC id in the high half,
     * and a destination-enabled bit so the interrupt is actually delivered. */
    uint32_t high = ((uint32_t)cpu & 0xff) << 24;

    ioapic_write(index, low);
    ioapic_write(index + 1, high);
    return 0;
}

/* Route the sixteen legacy lines to `vector0 .. vector0+15` on CPU `cpu`.
 *
 * This is what actually hands interrupt delivery over from the 8259s to the
 * IOAPIC: after it, every line has a redirection entry, so the 8259s can be
 * masked permanently and nothing can arrive twice.  The GSI numbering is the
 * one the ISA bus uses (line n is GSI n), which is what QEMU's i8042 and 8253
 * drive. */
int ioapic_route_legacy_lines(uint8_t vector0, uint8_t cpu, bool level) {
    if (ioapic_n == 0)
        return -1;

    /* The ISA lines are edge-triggered, and the caller asking for level here
     * would be a mistake worth refusing.
     *
     * An ISA interrupt is a pulse: the device raises the line, the CPU takes it,
     * and the line drops again.  Programming the redirection entry as
     * level-triggered instead makes the chip latch the interrupt in its remote
     * IRR bit and re-assert it for as long as the line is still high -- so a
     * device that leaves one byte in its output buffer, or an 8253 that holds
     * its output pin high until the channel is reloaded, produces an endless
     * stream of interrupts the handler never asked for.  Level-triggered
     * entries are for PCI devices with an interrupt-status register, not for
     * this bus. */
    if (level)
        kprintf("[warn] IOAPIC: legacy ISA lines are edge-triggered; "
                "ignoring the level request\n");
    level = false;

    for (unsigned line = 0; line < 16; line++) {
        /* A line the probe took away has no entry, so it stays masked and
         * claiming it fails rather than being silently accepted. */
        if (!ioapic_legacy_line_routable(line)) {
            legacy_lines[line].routed = false;
            continue;
        }

        if (ioapic_route(0, isa_pin[line], (uint8_t)(vector0 + line), cpu,
                         level) != 0) {
            kprintf("IOAPIC: cannot route line %u (pin %u)\n", line,
                    isa_pin[line]);
            return -1;
        }

        legacy_lines[line].routed = true;
        legacy_lines[line].level = level;
        legacy_lines[line].vector = (uint8_t)(vector0 + line);
        legacy_lines[line].cpu = cpu;
        /* The register index, not the pin: each redirection entry is two 32-bit
         * registers, so the low half lives at twice the entry number.  Storing
         * the GSI here instead silently addresses the wrong register, which
         * looks exactly like a chip that ignores the unmask. */
        legacy_lines[line].index =
            IOAPIC_REDIR_BASE + 2 * (ioapic_gsi[0] + isa_pin[line]);
    }

    ioapic_cpu = cpu;
    kprintf("IOAPIC: 16 lines routed to vectors %u..%u on LAPIC id %u "
            "(IRQ0 on pin %u)\n", vector0, (unsigned)(vector0 + 15), cpu,
            isa_pin[0]);
    for (unsigned line = 0; line < 16; line++)
        if (!ioapic_legacy_line_routable(line))
            kprintf("IOAPIC: IRQ%u shares pin %u with IRQ0 and has no entry "
                    "of its own; it stays masked\n", line, isa_pin[0]);
    return 0;
}

/* Mask every one of the sixteen ISA-alias pins on chip 0.
 *
 * Used to give the pin probe a quiet machine: a window in which exactly one pin
 * is live is the only way to attribute an interrupt to a pin, since the chip
 * delivers a vector and not the pin it came from. */
void ioapic_mask_all_legacy_pins(void) {
    if (ioapic_n == 0 || ioapic_win[0] == NULL)
        return;

    for (unsigned pin = 0; pin < 16 && pin < ioapic_entries[0]; pin++) {
        uint32_t index = IOAPIC_REDIR_BASE + 2 * (ioapic_gsi[0] + pin);
        ioapic_write(index, ioapic_read(index) | IOAPIC_REDIR_MASKED);
    }
}

/* Point one pin at one vector on one CPU, unmasked, with no regard for which
 * legacy line is supposed to own it.  Only the pin probe uses this; normal
 * routing goes through ioapic_route_legacy_lines(). */
int ioapic_route_pin_to_vector(unsigned pin, uint8_t vector, uint8_t cpu) {
    return ioapic_route(0, pin, vector, cpu, false);
}

/* Set or clear the mask bit of a line's redirection entry. */
static void legacy_line_mask(unsigned line, bool masked) {
    if (line >= 16 || !legacy_lines[line].routed)
        return;

    uint32_t low = ioapic_read(legacy_lines[line].index);
    if (masked)
        low |= IOAPIC_REDIR_MASKED;
    else
        low &= ~IOAPIC_REDIR_MASKED;
    ioapic_write(legacy_lines[line].index, low);
}

void ioapic_mask_line(unsigned ioapic, unsigned gsi) {
    (void)ioapic;
    legacy_line_mask(gsi, true);
}

void ioapic_unmask_line(unsigned ioapic, unsigned gsi) {
    (void)ioapic;
    legacy_line_mask(gsi, false);
}

bool ioapic_line_masked(unsigned ioapic, unsigned gsi) {
    (void)ioapic;
    if (gsi >= 16 || !legacy_lines[gsi].routed)
        return true;
    return (ioapic_read(legacy_lines[gsi].index) & IOAPIC_REDIR_MASKED) != 0;
}

/* Release a level-triggered line.
 *
 * An IOAPIC latches Remote IRR when it accepts an edge on a level-triggered
 * entry, and clears it only when the line goes inactive again -- which for the
 * PS/2 controller's "data available" status bit means reading the data
 * register.  Handlers here do exactly that, but the chip cannot see the read
 * land, so it has to be told: EOI for a version 0x20-or-later chip, and for
 * older ones momentarily dropping the entry to edge-triggered, which is the
 * documented way to clear the latch. */
/* End of interrupt for a level-triggered line.
 *
 * Only level-triggered entries need this.  A level-triggered chip latches the
 * interrupt in the entry's remote-IRR bit when it delivers, and will not deliver
 * that pin again until the bit is cleared; clearing it is the kernel's job.  The
 * chip's own EOI register is keyed by *vector*, not by entry index -- writing
 * the entry number instead silently clears nothing and the line goes quiet for
 * good after its first interrupt.
 *
 * Chips older than version 0x20 have no EOI register, and their remote IRR is
 * cleared by rewriting the entry with the level bit momentarily dropped, which
 * is why the routine writes the entry twice either way. */
void ioapic_eoi(unsigned line) {
    if (ioapic_n == 0 || ioapic_win[0] == NULL)
        return;
    if (line >= 16 || !legacy_lines[line].routed || !legacy_lines[line].level)
        return;

    uint32_t index = legacy_lines[line].index;
    uint32_t entry = ioapic_read(index);

    if ((ioapic_read(IOAPIC_REG_VER) & 0xff) >= 0x20) {
        ioapic_write(index, entry | IOAPIC_REDIR_LEVEL);
        *(volatile uint32_t *)(ioapic_win[0] + IOAPIC_EOI) =
            entry & IOAPIC_REDIR_VECTOR_MASK;
        ioapic_write(index, entry);
    } else {
        ioapic_write(index, entry & ~IOAPIC_REDIR_LEVEL);
        ioapic_write(index, entry);
    }
}

uint8_t ioapic_primary_cpu(void) {
    return ioapic_cpu;
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