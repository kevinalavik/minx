/* minx - local and I/O APIC plus the legacy 8259 PICs.
 *
 * The legacy PICs are remapped away and fully masked once the IOAPIC is up, so
 * all interrupt delivery goes through the local APIC.
 */
#ifndef MINX_APIC_H
#define MINX_APIC_H

#include <stdbool.h>
#include <stdint.h>

/* IOAPIC indirect register file, reached through these two ports. */
#define IOAPIC_REGSEL 0x00
#define IOAPIC_WINDOW 0x10

/* LAPIC registers. */
#define LAPIC_ID      0x020
#define LAPIC_TPR     0x080
#define LAPIC_EOI     0x0b0
#define LAPIC_SVR     0x0f0
#define LAPIC_ICR_LO  0x300
#define LAPIC_ICR_HI  0x310
#define LAPIC_TIMER   0x320
#define LAPIC_TDIV    0x3e0

#define LAPIC_SVR_ENABLE     (1u << 8)
#define LAPIC_SVR_SPURIOUS   (1u << 10)
#define LAPIC_ICR_DELIVS     0x700   /* delivery status */
#define LAPIC_ICR_VECTOR     0x0ff

int  lapic_init(void);
bool lapic_available(void);
uint32_t lapic_id(void);
void lapic_eoi(void);

/* Direct access to the LAPIC register file, for the timer and for tests. */
uint32_t lapic_read_reg(uint32_t reg);
void     lapic_write_reg(uint32_t reg, uint32_t value);

/* Priority: 0 lets the LAPIC decide, anything else overrides it. */
void lapic_set_priority(uint8_t priority);
void lapic_ipi(uint8_t target_lapic, uint8_t vector);
void lapic_ipi_all_but_self(uint8_t vector);
void lapic_ipi_all_cpus(uint8_t vector);

/* IOAPIC redirection table. */
int  ioapic_init(void);
unsigned ioapic_count(void);
void ioapic_write(uint32_t index, uint32_t value);
uint32_t ioapic_read(uint32_t index);

/* Route `vector` to `cpu` through IOAPIC `ioapic`'s GSI, optionally in
 * level-triggered mode, and unmask it. */
int  ioapic_route(unsigned ioapic, uint8_t vector, uint8_t cpu, bool level);

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