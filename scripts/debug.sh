#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LIMINE_DIR="$(brew --prefix limine)/share/limine"

cd "$ROOT_DIR"

echo "==> Building kernel (debug)..."
bazel build -c dbg //kernel:kernel

KERNEL_ELF="$ROOT_DIR/bazel-bin/kernel/kernel"
DISK_IMG="$ROOT_DIR/build/kernel.img"

echo "==> Creating bootable disk image..."
rm -f "$DISK_IMG"
dd if=/dev/zero of="$DISK_IMG" bs=1M count=64 2>/dev/null

DISK_DEV=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$DISK_IMG" 2>/dev/null | awk '{print $1}')
diskutil partitionDisk "$DISK_DEV" 1 MBR "MS-DOS FAT32" "KERNEL" 100% 2>/dev/null

MOUNT_POINT="/Volumes/KERNEL"
cp "$KERNEL_ELF" "$MOUNT_POINT/kernel.elf"
cp "$ROOT_DIR/limine.conf" "$MOUNT_POINT/limine.conf"
cp "$LIMINE_DIR/limine-bios.sys" "$MOUNT_POINT/"

hdiutil detach "$DISK_DEV" 2>/dev/null

limine bios-install "$DISK_IMG"

echo "==> Starting QEMU with GDB stub (port 1234)..."
echo "    Connect with: lldb -o 'gdb-remote 1234' $KERNEL_ELF"
echo "    Or: gdb -ex 'target remote :1234' $KERNEL_ELF"

qemu-system-x86_64 \
    -drive file="$DISK_IMG",format=raw,if=ide \
    -m 512M \
    -serial stdio \
    -no-reboot \
    -s -S
