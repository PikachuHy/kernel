#!/bin/bash
if [ -x /usr/local/opt/llvm/bin/llvm-objcopy ]; then
    exec /usr/local/opt/llvm/bin/llvm-objcopy "$@"
else
    exec /usr/bin/llvm-objcopy "$@"
fi
