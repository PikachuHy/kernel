#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$ROOT_DIR"

echo "==> Building kernel..."
bazel build //kernel:kernel

KERNEL_ELF="$ROOT_DIR/bazel-bin/kernel/kernel"

echo "==> Kernel built at $KERNEL_ELF"
echo "==> Starting QEMU..."

qemu-system-x86_64 \
    -kernel "$KERNEL_ELF" \
    -m 512M \
    -serial stdio \
    -no-reboot \
    -d cpu_reset
