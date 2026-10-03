#include "acpi.h"

#include "boot.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "types.h"

/* ACPI is pointed at by firmware through physical addresses, so every table
 * access goes through the higher-half direct map that Limine set up. */

#define ACPI_SIG_RSDP 0x20534452u /* "RSDP" */
#define ACPI_SIG_XSDT 0x54445358u /* "XSDT" */
#define ACPI_SIG_MADT 0x5444414du /* "MADT" */
#define ACPI_SIG_FADT 0x54444146u /* "FADT" */

/* The common SDT header, shared by the RSDT, XSDT, FADT, MADT and friends.
 * The signature is *four* bytes -- only the RSDP has an eight-byte one -- so
 * getting this wrong shifts every following field and turns a table's length
 * into its revision. */
typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    char     creator_id[4];
    uint32_t creator_revision;
} __attribute__((packed)) acpi_header_t;   /* 36 bytes */

/* The RSDP does *not* share the generic SDT header layout: the checksum, OEM id
 * and revision sit at 8, 9 and 15, and the pointer table addresses follow at 16
 * and 24.  Treating it like an SDT header reads firmware bytes as a length and a
 * revision, which is how this was first got wrong. */
typedef struct {
    char     signature[8];        /* 0x00 "RSD PTR " */
    uint8_t  checksum;            /* 0x08 */
    char     oem_id[6];           /* 0x09 */
    uint8_t  revision;            /* 0x0F */
    uint32_t rsdt_phys;           /* 0x10 */
    uint32_t length;              /* 0x14 */
    uint64_t xsdt_phys;           /* 0x18, revision >= 2 */
    uint8_t  extended_checksum;   /* 0x20, revision >= 2 */
    uint8_t  reserved[3];         /* 0x21 */
    uint64_t rsdt_phys2;          /* 0x24, revision >= 2 */
    uint64_t xsdt_phys2;          /* 0x2C, revision >= 2 */
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
    acpi_header_t header;
    uint32_t      local_apic_address;
    uint32_t      flags;
    /* Followed by the MADT's variable-length entries. */
} __attribute__((packed)) acpi_madt_t;

_Static_assert(sizeof(acpi_madt_t) == ACPI_MADT_HEADER_SIZE,
               "MADT header size drifted from ACPI_MADT_HEADER_SIZE");

static struct {
    bool          ready;
    uint32_t      version;
    acpi_madt_t  *madt;
    size_t        madt_length;
    acpi_header_t *fadt;

    acpi_cpu_t    cpus[MAX_CPU_COUNT];
    unsigned      cpu_count;

    acpi_ioapic_t ioapics[MAX_IOAPIC_COUNT];
    unsigned      ioapic_count;

    uint32_t      madt_flags;
    uint8_t       lapic_address;
    bool          have_lapic;
    uint32_t      pc_lapic_id;
    uint8_t       pc_numa_node;
} acpi;

static uint8_t madt_checksum_ok(const acpi_madt_t *m) {
    const uint8_t *p = (const uint8_t *)m;
    uint8_t sum = 0;
    for (size_t i = 0; i < m->header.length; i++)
        sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

static void parse_madt(void *madt_virt, bool verbose) {
    acpi_madt_t *m = (acpi_madt_t *)madt_virt;

    if (memcmp(m->header.signature, "APIC", 4) != 0) {
        kprintf("[warn] MADT has signature '%c%c%c%c', expected APIC\n",
                m->header.signature[0], m->header.signature[1],
                m->header.signature[2], m->header.signature[3]);
        return;
    }
    if (m->header.length < sizeof(acpi_madt_t) ||
        m->header.length > 0x10000) {
        kprintf("[warn] MADT length %u is implausible\n", m->header.length);
        return;
    }
    if (!madt_checksum_ok(m)) {
        kprintf("[warn] MADT checksum is wrong; parsing anyway\n");
    }

    acpi.madt = m;
    acpi.madt_length = m->header.length;
    acpi.madt_flags = m->flags;
    acpi.lapic_address = (uint8_t)m->local_apic_address;
    acpi.have_lapic = true;

    kprintf("MADT: LAPIC at 0x%02x, flags 0x%08x, %u bytes\n",
            m->local_apic_address, m->flags, m->header.length);

    /* A table header is a sequence of length-prefixed entries, so when one looks
     * wrong the raw bytes are the only way to find out which field is off. */
    if (verbose) {
        kprintf("MADT: header bytes:");
        for (size_t i = 0; i < sizeof(acpi_madt_t); i++)
            kprintf(" %02x", ((const uint8_t *)m)[i]);
        kprintf("\nMADT: entries:");
        for (size_t i = sizeof(acpi_madt_t); i < m->header.length; i++)
            kprintf(" %02x", ((const uint8_t *)m)[i]);
        kprintf("\n");
    }

    /* Walk the variable-length entries that follow the fixed header. */
    const uint8_t *p = (const uint8_t *)m + sizeof(acpi_madt_t);
    const uint8_t *end = (const uint8_t *)m + m->header.length;
    unsigned entries = 0;

    while (p + 2 <= end) {
        uint8_t type = p[0];
        uint8_t len = p[1];

        if (len < 2 || p + len > end) {
            kprintf("[warn] MADT entry type %u has a bad length %u; "
                    "stopping at offset %zu\n", type, len,
                    (size_t)(p - (const uint8_t *)m));
            break;
        }

        switch (type) {
        case ACPI_MADT_TYPE_LAPIC:
        case ACPI_MADT_TYPE_LAPIC_OVERRIDE:
        case ACPI_MADT_TYPE_LAPIC_CVM: {
            /* 8-byte header: type, len, acpi_id, lapic_id, flags */
            if (len >= 8) {
                uint8_t apic_id = p[2];
                uint8_t lapic_id = p[3];
                uint32_t flags = p[4] | ((uint32_t)p[5] << 8) |
                                 ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);

                if (type == ACPI_MADT_TYPE_LAPIC_OVERRIDE) {
                    /* An override renames an existing logical CPU. */
                    for (unsigned i = 0; i < acpi.cpu_count; i++) {
                        if (acpi.cpus[i].id == apic_id) {
                            acpi.cpus[i].lapic_id = lapic_id;
                            break;
                        }
                    }
                    kprintf("  MADT: LAPIC override: id %u -> LAPIC %u\n",
                            apic_id, lapic_id);
                    break;
                }

                bool enabled = (flags & ACPI_MADT_ENABLED) != 0;
                bool online = (flags & ACPI_MADT_ONLINE) != 0;

                if (acpi.cpu_count < MAX_CPU_COUNT) {
                    acpi_cpu_t *c = &acpi.cpus[acpi.cpu_count++];
                    c->id = apic_id;
                    c->lapic_id = lapic_id;
                    c->flags = (uint8_t)flags;
                    c->enabled = enabled;
                    c->online_capable = online;
                    kprintf("  MADT: CPU %u (ACPI id %u, LAPIC id %u)%s\n",
                            acpi.cpu_count - 1, apic_id, lapic_id,
                            enabled ? "" : " [disabled]");
                } else {
                    kprintf("  MADT: more CPUs than MAX_CPU_COUNT (%u); "
                            "ignoring the rest\n", MAX_CPU_COUNT);
                }
            }
            break;
        }

        case ACPI_MADT_TYPE_X2APIC: {
            /* 16-byte header: type, len, flags, x2apic_id(32) */
            if (len >= 16) {
                uint32_t flags = p[2] | ((uint32_t)p[3] << 8) |
                                 ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24);
                uint32_t id = p[6] | ((uint32_t)p[7] << 8) |
                              ((uint32_t)p[8] << 16) | ((uint32_t)p[9] << 24);
                if (acpi.cpu_count < MAX_CPU_COUNT) {
                    acpi_cpu_t *c = &acpi.cpus[acpi.cpu_count++];
                    c->id = id;
                    c->lapic_id = (uint8_t)(id & 0xff);
                    c->flags = (uint8_t)flags;
                    c->enabled = (flags & ACPI_MADT_ENABLED) != 0;
                    c->online_capable = (flags & ACPI_MADT_ONLINE) != 0;
                    kprintf("  MADT: CPU %u (x2APIC id %u)%s\n",
                            acpi.cpu_count - 1, id,
                            c->enabled ? "" : " [disabled]");
                }
            }
            break;
        }

        case ACPI_MADT_TYPE_IOAPIC:
        case ACPI_MADT_TYPE_IOAPIC_NS:
        case ACPI_MADT_TYPE_IOAPIC_ISA_ONLY: {
            /* 12-byte entry: type, len, id, reserved, mmio_base(32),
             * gsi_base(32).  Both addresses are 32-bit here; the 64-bit form
             * only appears in a type 5 override structure. */
            if (len >= 12 && acpi.ioapic_count < MAX_IOAPIC_COUNT) {
                acpi_ioapic_t *io = &acpi.ioapics[acpi.ioapic_count];
                io->id = p[2];
                io->base = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                           ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
                io->gsi_base = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                               ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
                acpi.ioapic_count++;
                kprintf("  MADT: IOAPIC %u at 0x%08x, GSI base %u\n",
                        io->id, io->base, io->gsi_base);
            }
            break;
        }

        case ACPI_MADT_TYPE_LAPIC_ADDRESS: {
            if (len >= 12) {
                uint64_t range = (uint64_t)p[4] | ((uint64_t)p[5] << 8) |
                                 ((uint64_t)p[6] << 16) |
                                 ((uint64_t)p[7] << 24) |
                                 ((uint64_t)p[8] << 32) |
                                 ((uint64_t)p[9] << 40) |
                                 ((uint64_t)p[10] << 48);
                uint64_t addr = (uint64_t)p[11] | ((uint64_t)p[12] << 8) |
                                ((uint64_t)p[13] << 16) |
                                ((uint64_t)p[14] << 24) |
                                ((uint64_t)p[15] << 32) |
                                ((uint64_t)p[16] << 40);
                kprintf("  MADT: LAPIC address range at physical %#lx "
                        "length %#lx\n", addr, range + 1);
            }
            break;
        }

        case ACPI_MADT_TYPE_LAPIC_NMI: {
            if (len >= 12) {
                uint8_t lapic_id = p[2];
                uint16_t flags = (uint16_t)(p[3] | ((uint16_t)p[4] << 8));
                uint8_t nmi_type = p[9];
                kprintf("  MADT: LAPIC NMI on LAPIC %u, flags 0x%04x, "
                        "type %u\n", lapic_id, flags, nmi_type);
            }
            break;
        }

        case ACPI_MADT_TYPE_GIC:
        case ACPI_MADT_TYPE_GICD:
            /* ARM interrupt controllers: irrelevant on x86, but skip cleanly. */
            break;

        default:
            break;
        }

        entries++;
        p += len;
    }

    kprintf("MADT: %u entries, %u CPU(s), %u IOAPIC(s)\n",
            entries, acpi.cpu_count, acpi.ioapic_count);
}

/* FADT lives in the XSDT but is only needed for power management; record where
 * it is so the power-off path can find it later. */
static void find_fadt(acpi_header_t *xsdt) {
    uint32_t count = (xsdt->length - sizeof(acpi_header_t)) / 8;
    const uint64_t *entries = (const uint64_t *)((const uint8_t *)xsdt +
                                                 sizeof(acpi_header_t));
    for (uint32_t i = 0; i < count; i++) {
        void *table = (void *)phys_to_virt(entries[i]);
        if (table == NULL)
            continue;
        if (memcmp(((acpi_header_t *)table)->signature, "FACP", 4) == 0) {
            acpi.fadt = (acpi_header_t *)table;
            kprintf("ACPI: FADT at %p\n", table);
            return;
        }
    }
}

static bool rsdp_checksum_ok(const acpi_rsdp_t *rsdp) {
    /* Revision 0 checksums the first 20 bytes, revision 2 the whole 108. */
    size_t span = rsdp->revision >= 2 ? 108 : 20;
    const uint8_t *p = (const uint8_t *)rsdp;
    uint8_t sum = 0;
    for (size_t i = 0; i < span; i++)
        sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

int acpi_init(void *rsdp_virt, bool verbose) {
    memzero(&acpi, sizeof(acpi));

    if (rsdp_virt == NULL) {
        kprintf("[warn] ACPI: no RSDP from the bootloader\n");
        return -1;
    }

    acpi_rsdp_t *rsdp = (acpi_rsdp_t *)rsdp_virt;
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) {
        kprintf("[warn] ACPI: RSDP signature is wrong\n");
        return -1;
    }

    acpi.version = rsdp->revision;
    kprintf("ACPI: RSDP at %p, revision %u, OEM \"%c%c%c%c%c%c\"\n",
            rsdp_virt, rsdp->revision,
            rsdp->oem_id[0], rsdp->oem_id[1], rsdp->oem_id[2],
            rsdp->oem_id[3], rsdp->oem_id[4], rsdp->oem_id[5]);

    if (!rsdp_checksum_ok(rsdp))
        kprintf("[warn] ACPI: RSDP checksum is wrong\n");

    /* Only revision 2 adds the extended (64-bit) pointers. */
    bool have_ext = rsdp->revision >= 2;

    uint64_t xsdt = have_ext ? rsdp->xsdt_phys2 : rsdp->xsdt_phys;
    uint64_t rsdt = have_ext ? rsdp->rsdt_phys2 : rsdp->rsdt_phys;

    if (verbose)
        kprintf("ACPI: RSDT pointer %#lx, XSDT pointer %#lx\n", rsdt, xsdt);

    /* ACPI tables below 1 MiB live in firmware memory the bootloader may have
     * reclaimed, so read them from the HHDT-mapped alias, but report the
     * physical address so a wrong pointer is easy to spot. */
    acpi_header_t *root = NULL;
    bool from_xsdt = false;

    if (xsdt != 0) {
        root = (acpi_header_t *)phys_to_virt(xsdt);
        from_xsdt = true;
    } else if (rsdt != 0) {
        root = (acpi_header_t *)phys_to_virt(rsdt);
    } else {
        kprintf("[warn] ACPI: neither an XSDT nor an RSDT is present\n");
        return -1;
    }

    kprintf("ACPI: %s at %p, length %u\n", from_xsdt ? "XSDT" : "RSDT", root,
            root->length);

    if (verbose) {
        kprintf("ACPI: root bytes:");
        for (int i = 0; i < 16; i++)
            kprintf(" %02x", ((const uint8_t *)root)[i]);
        kprintf("\n");
    }

    bool ok = false;
    if (memcmp(root->signature, "XSDT", 4) == 0) {
        ok = true;
        from_xsdt = true;
    } else if (memcmp(root->signature, "RSDT", 4) == 0) {
        ok = true;
        from_xsdt = false;
    }

    if (!ok) {
        kprintf("[warn] ACPI: the root pointer does not name an RSDT or XSDT\n");
        return -1;
    }

    if (root->length < sizeof(acpi_header_t) || root->length > 0x100000) {
        kprintf("[warn] ACPI: root table length %u is implausible\n",
                root->length);
        return -1;
    }

    if (from_xsdt)
        find_fadt(root);

    /* Both tables are arrays of 32-bit (RSDT) or 64-bit (XSDT) pointers. */
    const uint8_t *base = (const uint8_t *)root + sizeof(acpi_header_t);
    uint32_t count = from_xsdt
        ? (root->length - sizeof(acpi_header_t)) / 8
        : (root->length - sizeof(acpi_header_t)) / 4;
    if (count > 4096) {
        kprintf("[warn] ACPI: root table claims %u entries; clamping\n", count);
        count = 4096;
    }

    unsigned found = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t phys = from_xsdt ? ((const uint64_t *)base)[i]
                                  : (uint64_t)((const uint32_t *)base)[i];
        if (phys == 0)
            continue;

        void *table = (void *)phys_to_virt(phys);

        if (memcmp(((acpi_header_t *)table)->signature, "APIC", 4) == 0) {
            parse_madt(table, verbose);
            found++;
        }
    }

    if (found == 0) {
        kprintf("[warn] ACPI: no MADT found among %u tables\n", count);
        return -1;
    }

    acpi.ready = true;
    return 0;
}

const acpi_cpu_t *acpi_cpu_table(unsigned *count) {
    if (count)
        *count = acpi.cpu_count;
    return acpi.cpus;
}

const acpi_ioapic_t *acpi_ioapic_table(unsigned *count) {
    if (count)
        *count = acpi.ioapic_count;
    return acpi.ioapics;
}

bool acpi_ready(void) { return acpi.ready; }
bool acpi_has_lapic(void) { return acpi.have_lapic; }
uint8_t acpi_lapic_address(void) { return acpi.lapic_address; }
uint32_t acpi_lapic_flags(void) { return acpi.madt_flags; }
void *acpi_madt(void) { return acpi.madt; }
size_t acpi_madt_length(void) { return acpi.madt_length; }
void *acpi_fadt(void) { return acpi.fadt; }
uint32_t acpi_version(void) { return acpi.version; }

void acpi_note_ioapic_entries(unsigned index, uint32_t entries) {
    if (index >= acpi.ioapic_count)
        return;
    acpi.ioapics[index].irq_sources = entries;
}