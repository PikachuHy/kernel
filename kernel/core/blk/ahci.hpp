#pragma once

// Initialize the AHCI driver: find controller via PCI, reset HBA,
// probe ports, IDENTIFY devices, register BlockDevices.
void ahci_init();
