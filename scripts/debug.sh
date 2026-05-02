#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LIMINE_DIR="$(brew --prefix limine)/share/limine"

cd "$ROOT_DIR"

echo "==> Building kernel (debug)..."
bazel build -c dbg //kernel:kernel

KERNEL_ELF="$ROOT_DIR/bazel-bin/kernel/kernel"
ISO_DIR="$ROOT_DIR/build/iso"
ISO_IMAGE="$ROOT_DIR/build/kernel.iso"

rm -rf "$ISO_DIR"
mkdir -p "$ISO_DIR/boot"
mkdir -p "$ISO_DIR/EFI/BOOT"

cp "$KERNEL_ELF" "$ISO_DIR/boot/kernel.elf"
cp "$ROOT_DIR/limine.cfg" "$ISO_DIR/boot/limine.cfg"
cp "$LIMINE_DIR/limine-bios.sys" "$LIMINE_DIR/limine-bios-cd.bin" \
   "$LIMINE_DIR/limine-uefi-cd.bin" "$ISO_DIR/boot/"
cp "$LIMINE_DIR/BOOTX64.EFI" "$ISO_DIR/EFI/BOOT/"
cp "$LIMINE_DIR/BOOTIA32.EFI" "$ISO_DIR/EFI/BOOT/"

echo "==> Creating bootable ISO..."
xorriso -as mkisofs -b boot/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISO_DIR" -o "$ISO_IMAGE" 2>/dev/null

limine bios-install "$ISO_IMAGE"

echo "==> Starting QEMU with GDB stub (port 1234)..."
echo "    Connect with: lldb -o 'gdb-remote 1234' $KERNEL_ELF"
echo "    Or: gdb -ex 'target remote :1234' $KERNEL_ELF"

qemu-system-x86_64 \
    -cdrom "$ISO_IMAGE" \
    -m 512M \
    -serial stdio \
    -no-reboot \
    -s -S
