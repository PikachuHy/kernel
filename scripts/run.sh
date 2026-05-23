#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Detect platform
case "$(uname)" in
    Darwin) PLATFORM="macos" ;;
    Linux)  PLATFORM="linux" ;;
    *)
        echo "Unsupported platform: $(uname)" >&2
        exit 1
        ;;
esac

source "${SCRIPT_DIR}/lib/disk_${PLATFORM}.sh"

cd "$ROOT_DIR"

echo "==> Building kernel..."
bazel build //kernel:kernel

KERNEL_ELF="$ROOT_DIR/bazel-bin/kernel/kernel"
DISK_IMG="$ROOT_DIR/build/kernel.img"

disk_create "$DISK_IMG" "$KERNEL_ELF" "$ROOT_DIR/limine.conf"

echo "==> Starting QEMU (serial only, no GUI)..."
echo "    To exit: Ctrl+A, then X"
echo ""

qemu-system-x86_64 \
    -drive file="$DISK_IMG",format=raw,if=none,id=disk \
    -device ahci,id=ahci \
    -device ide-hd,drive=disk,bus=ahci.0 \
    -m 256M \
    -smp 1 \
    -display none \
    -serial stdio \
    -monitor none \
    -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
