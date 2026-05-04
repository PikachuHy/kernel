#!/bin/bash
# Wrapper for archiving that converts Bazel's ar invocation to macOS libtool.
# Bazel calls: ar -static -o <archive> <files>
# macOS ar doesn't support -static; libtool handles it natively.
set -euo pipefail

exec /usr/bin/libtool "$@"
