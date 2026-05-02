#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LIMINE_DIR="$(brew --prefix limine)/share/limine"

cd "$ROOT_DIR"

echo "==> Building kernel..."
bazel build //kernel:kernel

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
BOOT_SIZE=$(($(stat -f%z "$LIMINE_DIR/limine-bios-cd.bin") / 512))
xorriso -as mkisofs -b boot/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size "$BOOT_SIZE" -boot-info-table \
    --efi-boot boot/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISO_DIR" -o "$ISO_IMAGE"

limine bios-install "$ISO_IMAGE"

echo "==> Starting QEMU (graphical window + serial on terminal)..."
echo "    Kernel output appears in the QEMU window (framebuffer)"
echo "    and on this terminal (serial port)."
echo ""

qemu-system-x86_64 \
    -cdrom "$ISO_IMAGE" \
    -m 512M \
    -serial stdio \
    -no-reboot
