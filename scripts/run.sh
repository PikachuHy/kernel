#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LIMINE_DIR="$(brew --prefix limine)/share/limine"

cd "$ROOT_DIR"

echo "==> Building kernel..."
bazel build //kernel:kernel

KERNEL_ELF="$ROOT_DIR/bazel-bin/kernel/kernel"
DISK_IMG="$ROOT_DIR/build/kernel.img"

echo "==> Creating bootable disk image..."
rm -f "$DISK_IMG"
dd if=/dev/zero of="$DISK_IMG" bs=1M count=64 2>/dev/null

# Create FAT32 partition with MBR
DISK_DEV=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$DISK_IMG" 2>/dev/null | awk '{print $1}')
diskutil partitionDisk "$DISK_DEV" 1 MBR "MS-DOS FAT32" "KERNEL" 100% 2>/dev/null

# Copy kernel, config, and Limine files
MOUNT_POINT="/Volumes/KERNEL"
cp "$KERNEL_ELF" "$MOUNT_POINT/kernel.elf"
cp "$ROOT_DIR/limine.conf" "$MOUNT_POINT/limine.conf"
cp "$LIMINE_DIR/limine-bios.sys" "$MOUNT_POINT/"

hdiutil detach "$DISK_DEV" 2>/dev/null

# Install Limine BIOS bootloader
limine bios-install "$DISK_IMG"

echo "==> Starting QEMU (serial only, no GUI)..."
echo "    To exit: press Ctrl+A, then X"
echo ""

qemu-system-x86_64 \
    -drive file="$DISK_IMG",format=raw,if=none,id=disk \
    -device ahci,id=ahci \
    -device ide-hd,drive=disk,bus=ahci.0 \
    -m 512M \
    -smp 2 \
    -nographic \
    -no-reboot
