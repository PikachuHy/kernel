#include "kernel/arch/x86_64/acpi.hpp"
#include <stddef.h>

// Convert physical address to virtual via HHDM offset
inline void* p2v(uint64_t hhdm, uint64_t phys) {
    return reinterpret_cast<void*>(hhdm + phys);
}

// Validate ACPI table checksum: all bytes must sum to zero mod 256
static bool checksum_ok(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return sum == 0;
}

int acpi_find_madt(uint64_t hhdm, uint64_t rsdp_phys, const MADT** out_madt) {
    *out_madt = nullptr;

    auto* rsdp = reinterpret_cast<const RSDP*>(p2v(hhdm, rsdp_phys));

    // Validate RSDP signature ("RSD PTR ")
    const char expected_sig[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    for (int i = 0; i < 8; i++) {
        if (rsdp->signature[i] != expected_sig[i]) return -1;
    }

    // RSDP checksum: first 20 bytes must sum to zero
    if (!checksum_ok(reinterpret_cast<const uint8_t*>(rsdp), 20)) return -2;

    // Determine whether to use XSDT (ACPI 2.0+) or RSDT (ACPI 1.0)
    uint64_t dt_phys;
    bool is_xsdt;

    if (rsdp->revision >= 2 && rsdp->xsdt_addr != 0) {
        dt_phys = rsdp->xsdt_addr;
        is_xsdt = true;
    } else {
        dt_phys = rsdp->rsdt_addr;
        is_xsdt = false;
    }

    auto* dt_hdr = reinterpret_cast<const SDTHeader*>(p2v(hhdm, dt_phys));

    if (is_xsdt) {
        // XSDT entries are 64-bit physical addresses
        size_t entry_count = (dt_hdr->length - sizeof(SDTHeader)) / 8;
        auto* entries = reinterpret_cast<const uint64_t*>(dt_hdr + 1);
        for (size_t i = 0; i < entry_count; i++) {
            auto* hdr = reinterpret_cast<const SDTHeader*>(p2v(hhdm, entries[i]));
            // Check for "APIC" signature
            if (hdr->signature[0] == 'A' && hdr->signature[1] == 'P' &&
                hdr->signature[2] == 'I' && hdr->signature[3] == 'C') {
                if (checksum_ok(reinterpret_cast<const uint8_t*>(hdr), hdr->length)) {
                    *out_madt = reinterpret_cast<const MADT*>(hdr);
                    return 0;
                }
            }
        }
    } else {
        // RSDT entries are 32-bit physical addresses
        size_t entry_count = (dt_hdr->length - sizeof(SDTHeader)) / 4;
        auto* entries = reinterpret_cast<const uint32_t*>(dt_hdr + 1);
        for (size_t i = 0; i < entry_count; i++) {
            auto* hdr = reinterpret_cast<const SDTHeader*>(p2v(hhdm, entries[i]));
            if (hdr->signature[0] == 'A' && hdr->signature[1] == 'P' &&
                hdr->signature[2] == 'I' && hdr->signature[3] == 'C') {
                if (checksum_ok(reinterpret_cast<const uint8_t*>(hdr), hdr->length)) {
                    *out_madt = reinterpret_cast<const MADT*>(hdr);
                    return 0;
                }
            }
        }
    }

    return -3;  // MADT not found
}

int acpi_parse_cpus(const MADT* madt, CpuInfo* cpus, int max_cpus) {
    int count = 0;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(madt) + sizeof(MADT);
    const uint8_t* end = reinterpret_cast<const uint8_t*>(madt) + madt->header.length;

    while (ptr < end) {
        auto* entry = reinterpret_cast<const MADTEntry*>(ptr);
        if (entry->type == MADT_LAPIC && count < max_cpus) {
            auto* lapic = reinterpret_cast<const MADTLapic*>(ptr);
            cpus[count].acpi_cpu_id = lapic->acpi_cpu_id;
            cpus[count].lapic_id = lapic->apic_id;
            cpus[count].enabled = (lapic->flags & 1) != 0;
            count++;
        }
        ptr += entry->length;
    }

    return count;
}
