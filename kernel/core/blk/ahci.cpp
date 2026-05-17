#include "kernel/core/blk/ahci.hpp"
#include "kernel/core/blk/blkdev.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/arch/x86_64/pci.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/lib/klog.hpp"

// ── AHCI Register Definitions ─────────────────────────────────────────────

// HBA registers (offsets from ABAR)
static constexpr uint32_t GHC       = 0x04; // Global HBA Control
static constexpr uint32_t GHC_AE    = (1U << 31); // AHCI Enable
static constexpr uint32_t GHC_HR    = (1U << 0);  // HBA Reset
static constexpr uint32_t PI        = 0x0C; // Ports Implemented

// Port registers (offset from port base = ABAR + 0x100 + port * 0x80)
static constexpr uint32_t PXCLB     = 0x00; // Command List Base (low)
static constexpr uint32_t PXCLBU    = 0x04; // Command List Base (high)
static constexpr uint32_t PXFB      = 0x08; // FIS Base (low)
static constexpr uint32_t PXFBU     = 0x0C; // FIS Base (high)
static constexpr uint32_t PXCMD     = 0x18; // Command
static constexpr uint32_t PXCMD_ST  = (1U << 0);  // Start (DMA engine)
static constexpr uint32_t PXCMD_FRE = (1U << 4);  // FIS Receive Enable
static constexpr uint32_t PXCMD_FR  = (1U << 14); // FIS Receive Running
static constexpr uint32_t PXCMD_CR  = (1U << 15); // Command List Running
static constexpr uint32_t PXSIG     = 0x24; // Signature
static constexpr uint32_t PXSSTS    = 0x28; // SATA Status
static constexpr uint32_t PXSERR    = 0x30; // SATA Error
static constexpr uint32_t PXCI      = 0x38; // Command Issue

// FIS types
static constexpr uint8_t FIS_TYPE_REG_H2D = 0x27;

// ATA commands
static constexpr uint8_t ATA_IDENTIFY       = 0xEC;
static constexpr uint8_t ATA_READ_DMA_EXT   = 0x25;
static constexpr uint8_t ATA_WRITE_DMA_EXT  = 0x35;

// ── AHCI Data Structures ──────────────────────────────────────────────────

struct __attribute__((packed)) HbaCmdHeader {
    uint16_t options;      // C(0), W(4), P(5), CFL(8-15)
    uint16_t prdtl;        // PRDT length (entries)
    uint32_t prdbc;        // PRD byte count transferred
    uint32_t ctba;         // Command Table Base Address (low)
    uint32_t ctbau;        // Command Table Base Address (high)
    uint32_t reserved[4];
};

struct __attribute__((packed)) HbaCmdTable {
    uint8_t  cfis[64];     // Command FIS (first 20 bytes used)
    uint8_t  acmd[16];     // ATAPI command
    uint8_t  reserved[48];
    // PRDT entries follow; at least 1 entry fits before page boundary
    struct __attribute__((packed)) {
        uint32_t dba;      // Data Base Address (low)
        uint32_t dbau;     // Data Base Address (high)
        uint32_t reserved;
        uint32_t dbc;      // Byte count (bit 31 = IOC)
    } prdt[1];
};

struct __attribute__((packed)) HbaFis {
    uint8_t dsfis[0x1C];   // DMA Setup FIS
    uint8_t reserved1[4];
    uint8_t psfis[0x14];   // PIO Setup FIS
    uint8_t reserved2[0x0C];
    uint8_t rfis[0x14];    // D2H Register FIS
    uint8_t reserved3[4];
    uint8_t sdbfis[8];     // Set Device Bits FIS
    uint8_t ufis[0x40];
    uint8_t reserved4[0x18];
};

// ── Per-port driver state ─────────────────────────────────────────────────

struct AhciPort {
    uint8_t*  mmio_base;   // virtual address of port MMIO registers
    uint64_t  clb_phys;    // command list (phys, 1KB-aligned)
    uint8_t*  clb_virt;    // command list (virt)
    uint64_t  fb_phys;     // received FIS area (phys, 256B-aligned)
    uint8_t*  fb_virt;     // received FIS area (virt)
    uint64_t  ct_phys;     // command table (phys, 128B-aligned)
    uint8_t*  ct_virt;     // command table (virt)
    int       port_num;
};

// ── MMIO Helpers ──────────────────────────────────────────────────────────

static inline uint32_t ahci_read32(const uint8_t* base, uint32_t reg) {
    return *(const volatile uint32_t*)(base + reg);
}

static inline void ahci_write32(uint8_t* base, uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(base + reg) = val;
}

// Small delay loop with compiler barrier.
static inline void ahci_udelay() {
    for (int j = 0; j < 10; j++) {
        __asm__ volatile("pause" ::: "memory");
    }
}

// Spin-wait until (reg & mask) == 0.  Returns 0 on success, -1 on timeout.
static int ahci_wait_clear(uint8_t* mmio, uint32_t reg, uint32_t mask,
                           int timeout_loops) {
    for (int i = 0; i < timeout_loops; i++) {
        if (!(ahci_read32(mmio, reg) & mask))
            return 0;
        ahci_udelay();
    }
    return -1;
}

// Spin-wait until (reg & mask) != 0.  Returns 0 on success, -1 on timeout.
static int ahci_wait_set(uint8_t* mmio, uint32_t reg, uint32_t mask,
                         int timeout_loops) {
    for (int i = 0; i < timeout_loops; i++) {
        if (ahci_read32(mmio, reg) & mask)
            return 0;
        ahci_udelay();
    }
    return -1;
}

// Swap byte pairs in a buffer (for ATA model string conversion).
static void byte_swap_pairs(uint8_t* buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint8_t t = buf[i];
        buf[i]     = buf[i + 1];
        buf[i + 1] = t;
    }
}

// ── Port Lifecycle ────────────────────────────────────────────────────────

// Stop port DMA engine.  Returns 0 on success.
static int ahci_port_stop(uint8_t* mmio) {
    uint32_t cmd = ahci_read32(mmio, PXCMD);
    ahci_write32(mmio, PXCMD, cmd & ~(PXCMD_ST | PXCMD_FRE));
    return ahci_wait_clear(mmio, PXCMD, PXCMD_CR | PXCMD_FR, 100000);
}

// Allocate and initialise per-port DMA structures, start port.
// Returns 0 on success.
static int ahci_port_init(AhciPort* port) {
    uint8_t* mmio = port->mmio_base;
    uint32_t cmd;

    // Ensure port is stopped.
    ahci_port_stop(mmio);

    // Allocate command list (4KB, 1KB-aligned is satisfied by buddy).
    {
        void* p = buddy_alloc_pages(0);
        if (!p) return -1;
        port->clb_phys = reinterpret_cast<uint64_t>(p);
        port->clb_virt = static_cast<uint8_t*>(phys_to_virt(reinterpret_cast<uint64_t>(p)));
    }
    // Allocate FIS region.
    {
        void* p = buddy_alloc_pages(0);
        if (!p) goto err_clb;
        port->fb_phys = reinterpret_cast<uint64_t>(p);
        port->fb_virt = static_cast<uint8_t*>(phys_to_virt(reinterpret_cast<uint64_t>(p)));
    }
    // Allocate command table region.
    {
        void* p = buddy_alloc_pages(0);
        if (!p) goto err_fb;
        port->ct_phys = reinterpret_cast<uint64_t>(p);
        port->ct_virt = static_cast<uint8_t*>(phys_to_virt(reinterpret_cast<uint64_t>(p)));
    }

    // Zero all allocated DMA memory.
    for (int i = 0; i < 4096; i++) {
        port->clb_virt[i] = 0;
        port->fb_virt[i]  = 0;
        port->ct_virt[i]  = 0;
    }

    // Point port registers at our DMA buffers.
    ahci_write32(mmio, PXCLB,  port->clb_phys & 0xFFFFFFFFU);
    ahci_write32(mmio, PXCLBU, (port->clb_phys >> 32) & 0xFFFFFFFFU);
    ahci_write32(mmio, PXFB,   port->fb_phys & 0xFFFFFFFFU);
    ahci_write32(mmio, PXFBU,  (port->fb_phys >> 32) & 0xFFFFFFFFU);

    // Enable FIS receive.
    cmd = ahci_read32(mmio, PXCMD);
    ahci_write32(mmio, PXCMD, cmd | PXCMD_FRE);
    if (ahci_wait_set(mmio, PXCMD, PXCMD_FR, 100000) != 0)
        goto err_ct;

    // Start port DMA engine.
    cmd = ahci_read32(mmio, PXCMD);
    ahci_write32(mmio, PXCMD, cmd | PXCMD_ST);
    if (ahci_wait_set(mmio, PXCMD, PXCMD_CR, 100000) != 0)
        goto err_ct;

    return 0;

err_ct:
    buddy_free_pages(reinterpret_cast<void*>(port->ct_phys), 0);
    port->ct_phys = 0; port->ct_virt = nullptr;
err_fb:
    buddy_free_pages(reinterpret_cast<void*>(port->fb_phys), 0);
    port->fb_phys = 0; port->fb_virt = nullptr;
err_clb:
    buddy_free_pages(reinterpret_cast<void*>(port->clb_phys), 0);
    port->clb_phys = 0; port->clb_virt = nullptr;
    return -1;
}

static void ahci_port_free(AhciPort* port) {
    ahci_port_stop(port->mmio_base);
    if (port->clb_phys) buddy_free_pages(reinterpret_cast<void*>(port->clb_phys), 0);
    if (port->fb_phys)  buddy_free_pages(reinterpret_cast<void*>(port->fb_phys), 0);
    if (port->ct_phys)  buddy_free_pages(reinterpret_cast<void*>(port->ct_phys), 0);
}

// ── ATA IDENTIFY ──────────────────────────────────────────────────────────

static int ahci_port_identify(AhciPort* port, char* model_out,
                              size_t model_max, uint64_t* sectors_out) {
    uint8_t* mmio   = port->mmio_base;
    uint8_t* ct     = port->ct_virt;

    // Allocate DMA data buffer (512 bytes minimum; we use a full page).
    void* buf_phys = buddy_alloc_pages(0);
    if (!buf_phys) return -1;
    uint8_t* buf_virt = static_cast<uint8_t*>(phys_to_virt(reinterpret_cast<uint64_t>(buf_phys)));
    for (int i = 0; i < 4096; i++) buf_virt[i] = 0;

    // Zero command table for a fresh command.
    for (int i = 0; i < 256; i++) ct[i] = 0;

    // Build command header (slot 0).
    HbaCmdHeader* hdr = reinterpret_cast<HbaCmdHeader*>(port->clb_virt);
    hdr->options = (1U << 0) | (5U << 8);   // C=1, CFL=5 (20-byte FIS)
    hdr->prdtl   = 1;                        // one PRDT entry
    hdr->prdbc   = 0;
    hdr->ctba    = port->ct_phys & 0xFFFFFFFFU;
    hdr->ctbau   = (port->ct_phys >> 32) & 0xFFFFFFFFU;

    // Build Register H2D FIS inside command table.
    HbaCmdTable* ctbl = reinterpret_cast<HbaCmdTable*>(ct);
    ctbl->cfis[0]  = FIS_TYPE_REG_H2D;
    ctbl->cfis[1]  = 0x80;                  // C=1, PM=0
    ctbl->cfis[2]  = ATA_IDENTIFY;
    ctbl->cfis[3]  = 0x00;
    ctbl->cfis[4]  = 0x00;
    ctbl->cfis[5]  = 0x00;
    ctbl->cfis[6]  = 0x00;
    ctbl->cfis[7]  = 0x40;                  // Device (LBA=1)
    ctbl->cfis[8]  = 0x00;
    ctbl->cfis[9]  = 0x00;
    ctbl->cfis[10] = 0x00;
    ctbl->cfis[11] = 0x00;
    ctbl->cfis[12] = 0x01;                  // count low
    ctbl->cfis[13] = 0x00;                  // count high
    ctbl->cfis[14] = 0x00;                  // ICC
    ctbl->cfis[15] = 0x00;                  // Control

    // PRDT entry pointing at our data buffer.
    ctbl->prdt[0].dba      = reinterpret_cast<uint64_t>(buf_phys) & 0xFFFFFFFFU;
    ctbl->prdt[0].dbau     = (reinterpret_cast<uint64_t>(buf_phys) >> 32) & 0xFFFFFFFFU;
    ctbl->prdt[0].dbc      = 512 - 1;       // byte count (minus 1)
    ctbl->prdt[0].reserved = 0;

    // Clear pending errors.
    ahci_write32(mmio, PXSERR, ahci_read32(mmio, PXSERR));

    // Issue command via slot 0.
    ahci_write32(mmio, PXCI, 1U);

    // Poll for completion (CI bit 0 clears when done).
    if (ahci_wait_clear(mmio, PXCI, 1U, 5000000) != 0) {
        klog("AHCI: IDENTIFY timeout port ");
        klog_dec(port->port_num);
        uint32_t serr = ahci_read32(mmio, PXSERR);
        if (serr) {
            klog(" SERR=0x"); klog_hex(serr);
            ahci_write32(mmio, PXSERR, serr);
        }
        klog("\n");
        buddy_free_pages(buf_phys, 0);
        return -1;
    }

    // Parse model string: words 27-46, each 2 chars byte-swapped.
    // Word 27 starts at byte offset 54 in the 512-byte buffer.
    size_t model_len = (model_max < 41) ? model_max - 1 : 40;
    for (size_t i = 0; i < model_len; i++)
        model_out[i] = static_cast<char>(buf_virt[54 + i]);
    byte_swap_pairs(reinterpret_cast<uint8_t*>(model_out), model_len);
    model_out[model_len] = '\0';

    // Trim trailing spaces.
    {
        int last = static_cast<int>(model_len) - 1;
        while (last >= 0 && (model_out[last] == ' ' || model_out[last] == '\0')) {
            model_out[last] = '\0';
            last--;
        }
        // Ensure at least empty string.
        if (last < 0) model_out[0] = '\0';
    }

    // Parse total sectors:
    //   LBA48: words 100 (byte 200) + 102 (byte 204) as 64-bit
    //   Fallback LBA28: words 60-61 (byte 120) as 32-bit
    uint64_t lba48_lo = *reinterpret_cast<const uint32_t*>(buf_virt + 200);
    uint64_t lba48_hi = *reinterpret_cast<const uint32_t*>(buf_virt + 204);
    uint64_t lba48    = lba48_lo | (lba48_hi << 32);
    if (lba48 > 0)
        *sectors_out = lba48;
    else
        *sectors_out = *reinterpret_cast<const uint32_t*>(buf_virt + 120);

    buddy_free_pages(buf_phys, 0);
    return 0;
}

// ── BlockDevice callbacks ─────────────────────────────────────────────────

static int ahci_read(BlockDevice* dev, uint64_t lba, void* buf, size_t count) {
    AhciPort* port = static_cast<AhciPort*>(dev->driver_data);
    uint8_t* mmio  = port->mmio_base;
    size_t   bps   = dev->sector_size;  // 512
    (void)bps;

    for (size_t s = 0; s < count; s++) {
        // Allocate DMA buffer per sector.
        void* dma_phys = buddy_alloc_pages(0);
        if (!dma_phys) return -1;
        uint8_t* dma_virt = static_cast<uint8_t*>(phys_to_virt(reinterpret_cast<uint64_t>(dma_phys)));

        // Zero command table for fresh command.
        for (int i = 0; i < 256; i++) port->ct_virt[i] = 0;

        // Command header (slot 0).
        HbaCmdHeader* hdr = reinterpret_cast<HbaCmdHeader*>(port->clb_virt);
        hdr->options = (1U << 0) | (5U << 8);
        hdr->prdtl   = 1;
        hdr->prdbc   = 0;
        hdr->ctba    = port->ct_phys & 0xFFFFFFFFU;
        hdr->ctbau   = (port->ct_phys >> 32) & 0xFFFFFFFFU;

        // Build READ DMA EXT FIS.
        HbaCmdTable* ctbl = reinterpret_cast<HbaCmdTable*>(port->ct_virt);
        uint64_t cur_lba = lba + s;
        ctbl->cfis[0]  = FIS_TYPE_REG_H2D;
        ctbl->cfis[1]  = 0x80;
        ctbl->cfis[2]  = ATA_READ_DMA_EXT;
        ctbl->cfis[3]  = 0x00;
        ctbl->cfis[4]  = static_cast<uint8_t>(cur_lba);
        ctbl->cfis[5]  = static_cast<uint8_t>(cur_lba >> 8);
        ctbl->cfis[6]  = static_cast<uint8_t>(cur_lba >> 16);
        ctbl->cfis[7]  = 0x40;              // LBA=1
        ctbl->cfis[8]  = static_cast<uint8_t>(cur_lba >> 24);
        ctbl->cfis[9]  = static_cast<uint8_t>(cur_lba >> 32);
        ctbl->cfis[10] = static_cast<uint8_t>(cur_lba >> 40);
        ctbl->cfis[11] = 0x00;
        ctbl->cfis[12] = 0x01;              // 1 sector
        ctbl->cfis[13] = 0x00;
        ctbl->cfis[14] = 0x00;
        ctbl->cfis[15] = 0x00;

        // PRDT.
        ctbl->prdt[0].dba      = reinterpret_cast<uint64_t>(dma_phys) & 0xFFFFFFFFU;
        ctbl->prdt[0].dbau     = (reinterpret_cast<uint64_t>(dma_phys) >> 32) & 0xFFFFFFFFU;
        ctbl->prdt[0].dbc      = 512 - 1;
        ctbl->prdt[0].reserved = 0;

        // Clear pending errors.
        ahci_write32(mmio, PXSERR, ahci_read32(mmio, PXSERR));

        // Issue command.
        ahci_write32(mmio, PXCI, 1U);
        if (ahci_wait_clear(mmio, PXCI, 1U, 5000000) != 0) {
            uint32_t serr = ahci_read32(mmio, PXSERR);
            if (serr) {
                klog("AHCI: rd err port "); klog_dec(port->port_num);
                klog(" SERR=0x"); klog_hex(serr); klog("\n");
                ahci_write32(mmio, PXSERR, serr);
            }
            buddy_free_pages(dma_phys, 0);
            return -1;
        }

        // Copy from DMA buffer to caller buffer.
        uint8_t* dst = static_cast<uint8_t*>(buf) + s * 512;
        for (int i = 0; i < 512; i++)
            dst[i] = dma_virt[i];

        buddy_free_pages(dma_phys, 0);
    }

    return 0;
}

static int ahci_write(BlockDevice* dev, uint64_t lba, const void* buf,
                      size_t count) {
    AhciPort* port = static_cast<AhciPort*>(dev->driver_data);
    uint8_t* mmio  = port->mmio_base;

    for (size_t s = 0; s < count; s++) {
        void* dma_phys = buddy_alloc_pages(0);
        if (!dma_phys) return -1;
        uint8_t* dma_virt = static_cast<uint8_t*>(phys_to_virt(reinterpret_cast<uint64_t>(dma_phys)));

        // Copy caller data to DMA buffer.
        const uint8_t* src = static_cast<const uint8_t*>(buf) + s * 512;
        for (int i = 0; i < 512; i++)
            dma_virt[i] = src[i];

        // Zero command table.
        for (int i = 0; i < 256; i++) port->ct_virt[i] = 0;

        // Command header (slot 0) – W=1 for write.
        HbaCmdHeader* hdr = reinterpret_cast<HbaCmdHeader*>(port->clb_virt);
        hdr->options = (1U << 0) | (1U << 4) | (5U << 8);  // C=1, W=1, CFL=5
        hdr->prdtl   = 1;
        hdr->prdbc   = 0;
        hdr->ctba    = port->ct_phys & 0xFFFFFFFFU;
        hdr->ctbau   = (port->ct_phys >> 32) & 0xFFFFFFFFU;

        // Build WRITE DMA EXT FIS.
        HbaCmdTable* ctbl = reinterpret_cast<HbaCmdTable*>(port->ct_virt);
        uint64_t cur_lba = lba + s;
        ctbl->cfis[0]  = FIS_TYPE_REG_H2D;
        ctbl->cfis[1]  = 0x80;
        ctbl->cfis[2]  = ATA_WRITE_DMA_EXT;
        ctbl->cfis[3]  = 0x00;
        ctbl->cfis[4]  = static_cast<uint8_t>(cur_lba);
        ctbl->cfis[5]  = static_cast<uint8_t>(cur_lba >> 8);
        ctbl->cfis[6]  = static_cast<uint8_t>(cur_lba >> 16);
        ctbl->cfis[7]  = 0x40;
        ctbl->cfis[8]  = static_cast<uint8_t>(cur_lba >> 24);
        ctbl->cfis[9]  = static_cast<uint8_t>(cur_lba >> 32);
        ctbl->cfis[10] = static_cast<uint8_t>(cur_lba >> 40);
        ctbl->cfis[11] = 0x00;
        ctbl->cfis[12] = 0x01;
        ctbl->cfis[13] = 0x00;
        ctbl->cfis[14] = 0x00;
        ctbl->cfis[15] = 0x00;

        // PRDT.
        ctbl->prdt[0].dba      = reinterpret_cast<uint64_t>(dma_phys) & 0xFFFFFFFFU;
        ctbl->prdt[0].dbau     = (reinterpret_cast<uint64_t>(dma_phys) >> 32) & 0xFFFFFFFFU;
        ctbl->prdt[0].dbc      = 512 - 1;
        ctbl->prdt[0].reserved = 0;

        // Clear pending errors.
        ahci_write32(mmio, PXSERR, ahci_read32(mmio, PXSERR));

        // Issue command.
        ahci_write32(mmio, PXCI, 1U);
        if (ahci_wait_clear(mmio, PXCI, 1U, 5000000) != 0) {
            uint32_t serr = ahci_read32(mmio, PXSERR);
            if (serr) {
                klog("AHCI: wr err port "); klog_dec(port->port_num);
                klog(" SERR=0x"); klog_hex(serr); klog("\n");
                ahci_write32(mmio, PXSERR, serr);
            }
            buddy_free_pages(dma_phys, 0);
            return -1;
        }

        buddy_free_pages(dma_phys, 0);
    }

    return 0;
}

// ── Initialisation ────────────────────────────────────────────────────────

void ahci_init() {
    klog("AHCI: probing for SATA controller...\n");

    PciDevice* pci = pci_find_by_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_SATA);
    if (!pci) {
        klog("AHCI: no SATA controller found, skipping\n");
        return;
    }

    klog("AHCI: found at PCI ");
    klog_hex(pci->bus); klog(":"); klog_hex(pci->dev);
    klog("."); klog_hex(pci->func); klog("\n");
    klog("  vendor=0x"); klog_hex(pci->vendor_id);
    klog(" device=0x"); klog_hex(pci->device_id);
    klog(" BAR5=0x"); klog_hex(pci->bar[5]); klog("\n");

    // BAR5 = ABAR (AHCI Base Address Register).  Mask off lower 4 bits.
    uint64_t abar_phys = pci->bar[5] & ~0xFULL;
    uint8_t* abar = static_cast<uint8_t*>(phys_to_virt(abar_phys));

    klog("  ABAR phys=0x"); klog_hex(abar_phys);
    klog(" virt=0x"); klog_hex(reinterpret_cast<uint64_t>(abar));
    klog("\n");

    // Reset HBA.
    klog("AHCI: resetting HBA...\n");
    ahci_write32(abar, GHC, GHC_HR);
    if (ahci_wait_clear(abar, GHC, GHC_HR, 1000000) != 0) {
        klog("AHCI: HBA reset timeout\n");
        return;
    }
    klog("  HBA reset OK\n");

    // Enable AHCI.
    ahci_write32(abar, GHC, GHC_AE);

    // Read Ports Implemented bitmap.
    uint32_t ports_impl = ahci_read32(abar, PI);
    klog("  ports implemented: 0x"); klog_hex(ports_impl); klog("\n");

    int dev_index = 0;
    for (int port_num = 0; port_num < 32; port_num++) {
        if (!(ports_impl & (1U << port_num))) continue;

        uint8_t* pm = abar + 0x100 + port_num * 0x80;
        uint32_t ssts = ahci_read32(pm, PXSSTS);

        klog("  port "); klog_dec(port_num);
        klog(": SSTS=0x"); klog_hex(ssts); klog("\n");

        // DET field (bits 0-3): 3 = device present and phy established.
        uint32_t det = ssts & 0x0F;
        if (det != 3) {
            klog("    -> DET="); klog_dec(det); klog(" (no device)\n");
            continue;
        }

        klog("    -> device detected, initialising port...\n");

        // Allocate port state.
        AhciPort* port = static_cast<AhciPort*>(kmalloc(sizeof(AhciPort)));
        if (!port) continue;
        port->mmio_base = pm;
        port->port_num  = port_num;
        port->clb_phys = 0; port->clb_virt = nullptr;
        port->fb_phys  = 0; port->fb_virt  = nullptr;
        port->ct_phys  = 0; port->ct_virt  = nullptr;

        if (ahci_port_init(port) != 0) {
            klog("    port init failed\n");
            kfree(port);
            continue;
        }

        // After port start, wait briefly for device signature to be latched.
        for (int i = 0; i < 10000; i++) {
            __asm__ volatile("pause" ::: "memory");
        }

        uint32_t sig = ahci_read32(pm, PXSIG);
        klog("    SIG=0x"); klog_hex(sig); klog("\n");

        // Signature 0x00000101 = ATA.  ATAPI and PM have different sigs.
        if (sig != 0x00000101) {
            klog("    -> non-ATA signature (0x"); klog_hex(sig);
            klog("), skipping\n");
            ahci_port_free(port);
            kfree(port);
            continue;
        }

        klog("    -> ATA device\n");

        // Send IDENTIFY.
        char model[41];
        uint64_t total_sectors = 0;
        if (ahci_port_identify(port, model, sizeof(model), &total_sectors) != 0) {
            klog("    IDENTIFY failed\n");
            ahci_port_free(port);
            kfree(port);
            continue;
        }

        uint64_t mb = total_sectors * 512 / (1024 * 1024);
        klog("    model: \"");
        klog(model);
        klog("\" (");
        klog_dec(mb); klog(" MB, ");
        klog_dec(total_sectors); klog(" sectors)\n");

        // Register BlockDevice.
        BlockDevice* bdev = static_cast<BlockDevice*>(kmalloc(sizeof(BlockDevice)));
        if (!bdev) {
            ahci_port_free(port);
            kfree(port);
            continue;
        }

        // Name: "ahci0", "ahci1", ...
        bdev->name[0] = 'a'; bdev->name[1] = 'h';
        bdev->name[2] = 'c'; bdev->name[3] = 'i';
        bdev->name[4] = static_cast<char>('0' + dev_index);
        bdev->name[5] = '\0';
        for (int i = 6; i < 32; i++) bdev->name[i] = '\0';

        bdev->total_sectors = total_sectors;
        bdev->sector_size   = 512;
        bdev->read          = ahci_read;
        bdev->write         = ahci_write;
        bdev->driver_data   = port;
        bdev->next          = nullptr;

        blkdev_register(bdev);
        klog("    registered as "); klog(bdev->name); klog("\n");

        dev_index++;
    }

    // Boot-time verification: read MBR from first device.
    if (dev_index > 0) {
        char dn[8];
        dn[0] = 'a'; dn[1] = 'h'; dn[2] = 'c'; dn[3] = 'i';
        dn[4] = '0'; dn[5] = '\0';
        BlockDevice* d = blkdev_find(dn);
        if (d) {
            uint8_t mbr[512];
            int rc = d->read(d, 0, mbr, 1);
            if (rc == 0 && mbr[0x1FE] == 0x55 && mbr[0x1FF] == 0xAA) {
                klog("AHCI: sector 0: MBR signature present (0x55AA)\n");
            } else if (rc == 0) {
                klog("AHCI: sector 0: no MBR signature\n");
            } else {
                klog("AHCI: sector 0 read failed\n");
            }
        }
    }

    klog("AHCI: init complete\n");
}
