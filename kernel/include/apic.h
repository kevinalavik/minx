/* minx - local and I/O APIC plus the legacy 8259 PICs.
 *
 * The legacy PICs are remapped away and fully masked once the IOAPIC is up, so
 * all interrupt delivery goes through the local APIC.
 */
#ifndef MINX_APIC_H
#define MINX_APIC_H

#include <stdbool.h>
#include <stdint.h>

#include "vectors.h"

/* IOAPIC indirect register file: byte offsets inside the chip's MMIO window.
 * The window is mapped uncacheable by the virtual memory manager.
 *
 * A register is selected by writing its index to IOAPIC_REGSEL and then reading
 * or writing IOAPIC_WINDOW.  The redirection table starts at IOAPIC_REDIR_BASE
 * and each entry is *two* registers wide, so entry n's low half is at
 * IOAPIC_REDIR_BASE + 2*n -- omitting the base addresses a register that does not
 * exist, which reads as zero and looks exactly like a chip that ignores every
 * mask change. */
#define IOAPIC_REGSEL     0x00
#define IOAPIC_WINDOW     0x10
#define IOAPIC_EOI        0x40   /* per-pin EOI, chips of version 0x20 or later */
#define IOAPIC_REDIR_BASE 0x10

#define IOAPIC_REG_ID  0x00
#define IOAPIC_REG_VER 0x01

/* Redirection entry bit layout.  Note that bit 8 is *not* the level flag: bits
 * 10..8 are the delivery mode, and the trigger mode is bit 15.  Writing "level"
 * into bit 8 silently asks for lowest-priority delivery on an edge-triggered
 * line, which for a device that holds its interrupt line asserted delivers it
 * exactly once and then never again.  Bits 12 (delivery status) and 14 (remote
 * IRR) are read-only.
 *
 *   bits 7..0    vector
 *   bits 10..8   delivery mode (0 = fixed)
 *   bit  11      destination mode (0 = physical)
 *   bit  13      polarity (0 = active high)
 *   bit  15      trigger mode (1 = level)
 *   bit  16      mask: the line is blocked while this is set
 *   bits 55..32  destination
 */
#define IOAPIC_REDIR_VECTOR_MASK 0xffu
#define IOAPIC_REDIR_MASKED      (1u << 16)
#define IOAPIC_REDIR_LEVEL       (1u << 15)
#define IOAPIC_REDIR_REMOTE_IRR  (1u << 14)

/* LAPIC registers.  The MMIO window is 4 KiB and every register is 32 bits
 * wide; the offsets below are the architectural ones (Intel SDM Vol. 3,
 * "Local APIC Register Offset Address Map"), cross-checked against QEMU's
 * hw/intc/apic.c.  The timer *count* registers are at 0x380/0x390, distinct
 * from the LVT Timer at 0x320 that selects the mode and the vector. */
#define LAPIC_ID      0x020
#define LAPIC_VERSION 0x030
#define LAPIC_TPR     0x080
#define LAPIC_DFR     0x0d0
#define LAPIC_LDR     0x0e0
#define LAPIC_SVR     0x0f0
#define LAPIC_ISR0    0x100   /* 8 x 64 bits: in-service register */
#define LAPIC_IRR0    0x200   /* 8 x 64 bits: request register      */
#define LAPIC_ESR     0x280   /* error status                        */
#define LAPIC_ICR_LO  0x300   /* inter-processor interrupt, low      */
#define LAPIC_ICR_HI  0x310   /* inter-processor interrupt, high     */
#define LAPIC_LVT_CMCI    0x2f0
#define LAPIC_TIMER   0x320
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_LVT_THERMAL 0x330
#define LAPIC_LVT_PMC     0x340
#define LAPIC_LVT_LINT0   0x350
#define LAPIC_LVT_LINT1   0x360
#define LAPIC_LVT_ERROR   0x370
#define LAPIC_EOI     0x0b0
#define LAPIC_COUNT    0x380   /* timer initial count                */
#define LAPIC_CURRENT_COUNT 0x390
#define LAPIC_TDIV    0x3e0   /* timer divide configuration         */

#define LAPIC_SVR_ENABLE     (1u << 8)
#define LAPIC_LVT_MASKED      (1u << 16)
#define LAPIC_LVT_PERIODIC    (1u << 17)
#define LAPIC_TDIV_ENABLE     (1u << 0)
#define LAPIC_TDIV_PERIODIC   (1u << 1)

/* ICR delivery mode, bits 8..10. */
#define LAPIC_DM_FIXED      0x0u
#define LAPIC_DM_INIT       0x4u
#define LAPIC_DM_SIPI       0x5u

/* ICR bit fields. */
#define LAPIC_ICR_DELIVS     0x1000  /* delivery status                  */
#define LAPIC_ICR_ASSERT     0x4000  /* delivery mode: assert            */
#define LAPIC_ICR_LEVEL      0x4000  /* trigger mode: level              */
#define LAPIC_ICR_SELF       0x40000 /* destination shorthand: self      */
#define LAPIC_ICR_ALL_BUT_SELF 0x80000
#define LAPIC_ICR_ALL_CPUS   0xc0000

#define LAPIC_ICR_DELIVS_MASK 0x7000

int  lapic_init(void);
bool lapic_available(void);
uint32_t lapic_id(void);
void lapic_eoi(void);

/* Unmask the local vectors the kernel owns (the LINT pair for the IOAPIC, the
 * error and spurious vectors).  Limine masks every LVT before handing over, so
 * without this nothing at all reaches the CPU. */
void lapic_unmask_local_vectors(void);
bool lapic_spurious_handler_installed(void);

/* Direct access to the LAPIC register file, for the timer and for tests. */
uint32_t lapic_read_reg(uint32_t reg);
void     lapic_write_reg(uint32_t reg, uint32_t value);

/* Priority: 0 lets the LAPIC decide, anything else overrides it. */
void lapic_set_priority(uint8_t priority);
void lapic_ipi(uint8_t target_lapic, uint8_t vector);
void lapic_ipi_all_but_self(uint8_t vector);
void lapic_ipi_all_cpus(uint8_t vector);

/* INIT/SIPI sequence used to start an application processor. */
void lapic_ipi_init(uint8_t target_lapic);
void lapic_ipi_startup(uint8_t target_lapic, uint8_t vector);

/* IOAPIC redirection table. */
int  ioapic_init(void);
unsigned ioapic_count(void);
void ioapic_write(uint32_t index, uint32_t value);
uint32_t ioapic_read(uint32_t index);

/* The local APIC id of the CPU that ran ioapic_route_legacy_lines(), so a later
 * re-route can move a line without guessing which CPU it was aimed at. */
uint8_t ioapic_primary_cpu(void);

/* Give the IOAPIC ownership of the sixteen legacy lines, which is what lets the
 * 8259s be masked for good. */
int ioapic_route_legacy_lines(uint8_t vector0, uint8_t cpu, bool level);

/* Which IOAPIC pin carries a given legacy ISA IRQ, and the mapping itself.
 *
 * The identity map is the architectural expectation and what every machine the
 * kernel has run on does, but it is not guaranteed -- QEMU's IOAPIC model puts
 * ISA IRQ0 (the 8253) on pin 2 -- and a wrong pin is indistinguishable from a
 * line that never fires.  timer.c's pin probe uses these two to record what it
 * found; ioapic_route_legacy_lines() uses them to program the table. */
unsigned ioapic_legacy_pin(unsigned irq);
bool     ioapic_legacy_line_routable(unsigned irq);
void     ioapic_set_legacy_pin(unsigned irq, unsigned pin);

/* Pin-probe primitives.  ioapic_mask_all_legacy_pins() silences every ISA-alias
 * pin so that a window with one pin live attributes an interrupt to that pin;
 * ioapic_route_pin_to_vector() then points that pin at a vector. */
void ioapic_mask_all_legacy_pins(void);
int  ioapic_route_pin_to_vector(unsigned pin, uint8_t vector, uint8_t cpu);

/* Mask and unmask one of the sixteen legacy lines by its IOAPIC GSI. */
void ioapic_mask_line(unsigned ioapic, unsigned gsi);
void ioapic_unmask_line(unsigned ioapic, unsigned gsi);
bool ioapic_line_masked(unsigned ioapic, unsigned gsi);

/* End of interrupt for a line the IOAPIC delivered.  For a level-triggered
 * entry this also has to release the Remote IRR bit, which the chip keeps set
 * until it sees the line go inactive: a handler that reads its device and finds
 * nothing would otherwise never be called again. */
void ioapic_eoi(unsigned line);

/* Route `line` (a chip pin: a GSI, or an ISA IRQ number for the pins that alias
 * the ISA lines) to `vector` on `cpu` through IOAPIC `ioapic`, optionally
 * level-triggered, and unmask it.  The pin and the vector are separate because
 * they select different things: the pin chooses the entry, the vector fills in
 * the low eight bits of it. */
int  ioapic_route(unsigned ioapic, unsigned line, uint8_t vector, uint8_t cpu,
                  bool level);

/* Number of IOAPIC entries this chip drives (usually 24). */
uint32_t ioapic_max_entries(unsigned ioapic);

/* Legacy 8259 PICs. */
int  pic_init(void);
void pic_mask_all(void);
void pic_unmask_all(void);
void pic_remap(uint8_t master_offset, uint8_t slave_offset);
void pic_disable(void);
uint16_t pic_irq_mask(void);
void pic_set_mask(uint16_t mask);

#endif /* MINX_APIC_H */