#pragma once

#include <stdint.h>

// ACPI structures for MADT CPU discovery

struct [[gnu::packed]] RSDP {
    char     signature[8];   // "RSD PTR "
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    // ACPI 2.0+:
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
};

struct [[gnu::packed]] SDTHeader {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct [[gnu::packed]] MADT {
    SDTHeader header;
    uint32_t  lapic_addr;
    uint32_t  flags;
    // Variable-length entries follow
};

constexpr uint8_t MADT_LAPIC  = 0x00;
constexpr uint8_t MADT_IOAPIC = 0x01;
constexpr uint8_t MADT_ISO    = 0x02;
constexpr uint8_t MADT_NMI    = 0x04;

struct [[gnu::packed]] MADTEntry {
    uint8_t type;
    uint8_t length;
};

struct [[gnu::packed]] MADTLapic {
    uint8_t  type;       // 0x00
    uint8_t  length;     // 8
    uint8_t  acpi_cpu_id;
    uint8_t  apic_id;
    uint32_t flags;      // bit 0 = enabled
};

constexpr int ACPI_MAX_CPUS = 64;

struct CpuInfo {
    uint8_t acpi_cpu_id;
    uint8_t lapic_id;
    bool    enabled;
};

// Walk RSDP -> XSDT(or RSDT) -> find MADT.
// Returns 0 on success, <0 on error.
// out_madt points to the MADT (accessible via HHDM).
int acpi_find_madt(uint64_t hhdm, uint64_t rsdp_phys, const MADT** out_madt);

// Parse MADT entries to enumerate Local APICs.
// Returns number of CPUs found.
int acpi_parse_cpus(const MADT* madt, CpuInfo* cpus, int max_cpus);
