#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$ROOT_DIR"

echo "==> Building kernel (debug)..."
bazel build -c dbg //kernel:kernel

KERNEL_ELF="$ROOT_DIR/bazel-bin/kernel/kernel"

echo "==> Starting QEMU with GDB stub (port 1234)..."
echo "    Connect with: lldb -o 'gdb-remote 1234' $KERNEL_ELF"
echo "    Or: gdb -ex 'target remote :1234' $KERNEL_ELF"

qemu-system-x86_64 \
    -kernel "$KERNEL_ELF" \
    -m 512M \
    -serial stdio \
    -no-reboot \
    -s -S
