/* minx - ACPI discovery.
 *
 * Finds the RSDP handed over by the bootloader, walks the XSDT to the MADT, and
 * exposes the LAPIC/IOAPIC topology.  Only the tables the kernel actually needs
 * are decoded; anything unexpected is reported and skipped rather than trusted.
 */
#ifndef MINX_ACPI_H
#define MINX_ACPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MADT interrupt-controller types (the first byte of each entry). */
#define ACPI_MADT_TYPE_LAPIC            0
#define ACPI_MADT_TYPE_IOAPIC           1
#define ACPI_MADT_TYPE_IOAPIC_NS        2
#define ACPI_MADT_TYPE_IOAPIC_ISA_ONLY  3
#define ACPI_MADT_TYPE_LAPIC_OVERRIDE   4
#define ACPI_MADT_TYPE_LAPIC_NMI        5
#define ACPI_MADT_TYPE_LAPIC_ADDRESS    6
#define ACPI_MADT_TYPE_ITS              7
#define ACPI_MADT_TYPE_GIC              8
#define ACPI_MADT_TYPE_GICD             9
#define ACPI_MADT_TYPE_LAPIC_CVM        10
#define ACPI_MADT_TYPE_LAPIC_CVM_REG    11
#define ACPI_MADT_TYPE_LAPIC_SYSV       12
#define ACPI_MADT_TYPE_SAPIC            13
#define ACPI_MADT_TYPE_X2APIC           14

#define ACPI_MADT_ENABLED       (1u << 0)
#define ACPI_MADT_ONLINE        (1u << 1)
#define ACPI_MADT_RUNTIME       (1u << 2)
#define ACPI_MADT_GS           (1u << 15)
#define ACPI_MADT_GS_MASK       (1u << 16)

#define MAX_IOAPIC_COUNT 8

typedef struct {
    uint32_t id;          /* LAPIC id, already extended for x2APIC entries */
    uint8_t  flags;       /* ACPI_MADT_* flags */
    uint8_t  lapic_id;    /* 8-bit id, i.e. the legacy encoding */
    bool     enabled;
    bool     online_capable;
} acpi_cpu_t;

typedef struct {
    uint32_t id;
    uint32_t base;        /* MMIO physical address of the register window */
    uint32_t gsi_base;
    uint32_t irq_sources; /* filled in once the chip has been programmed */
} acpi_ioapic_t;

/* Size of the MADT's fixed part: the 36-byte SDT header plus the local APIC
 * address and the interrupt controller flags.  Everything after this is a
 * sequence of length-prefixed entries. */
#define ACPI_MADT_HEADER_SIZE 44

/* Parse the RSDP the bootloader found and everything reachable from it.
 * `verbose` additionally dumps the raw header bytes, which is what makes a
 * misread firmware table diagnosable instead of mysterious. */
int  acpi_init(void *rsdp_virt, bool verbose);

/* Called once per LAPIC entry so its address can be reported in the log. */
const acpi_cpu_t *acpi_cpu_table(unsigned *count);
const acpi_ioapic_t *acpi_ioapic_table(unsigned *count);

/* True once a RSDP was found and a MADT was parsed out of it.  Note that this is
 * not the same as acpi_version(): a revision-0 RSDP is valid firmware and
 * still parses, so the version can legitimately be 0 on a working machine. */
bool     acpi_ready(void);

/* Where the LAPIC is and which revision it is; 0 when there is none. */
bool     acpi_has_lapic(void);
uint8_t  acpi_lapic_address(void);
uint32_t acpi_lapic_flags(void);
uint32_t acpi_madt_flags(void);
void    *acpi_madt(void);
void    *acpi_fadt(void);
uint32_t acpi_version(void);

/* Total length in bytes of the MADT, so later phases can map its region. */
size_t acpi_madt_length(void);

/* Keep an ACPI-defined region out of the physical allocator.
 *
 * The RSDP and the tables it points at are read from firmware memory long after
 * the firmware has formally stopped using it: ACPI tables are typically in a
 * region Limine reports as ACPI reclaimable.  Handing those frames to the page
 * allocator would corrupt the MADT while the kernel is still parsing it, and the
 * failure would look like a random bad length rather than a use-after-free. */
int  acpi_reserve_tables(void);

/* Record how many redirection entries a programmed IOAPIC actually has. */
void acpi_note_ioapic_entries(unsigned index, uint32_t entries);

#endif /* MINX_ACPI_H */