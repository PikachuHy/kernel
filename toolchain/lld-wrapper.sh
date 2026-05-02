#!/bin/bash
# Wrapper for ld.lld that filters macOS-specific flags Apple Clang passes
# to the linker even when targeting non-macOS platforms.
set -euo pipefail

SKIP_NEXT=false
ARGS=()
for arg in "$@"; do
    if $SKIP_NEXT; then
        SKIP_NEXT=false
        continue
    fi
    case "$arg" in
        -dynamic|-arch|-platform_version|-syslibroot|-lto_library|-no_deduplicate)
            # macOS-specific linker flags; skip
            ;;
        -object_path_lto|-export_dynamic|-no_fixup_chains)
            # macOS-specific; skip
            ;;
        -e|--entry|-o|--output|-L|-rpath)
            ARGS+=("$arg")
            ;;
        /Library/Developer/CommandLineTools/usr/lib/libLTO.dylib|\
        /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk)
            # macOS-specific paths; skip
            ;;
        x86_64|macos|15.0.0|26.0)
            # These are arguments to -arch, -platform_version, etc.
            # which we already skipped
            ;;
        --sysroot=/dev/null)
            ARGS+=("--sysroot=/dev/null")
            ;;
        *)
            ARGS+=("$arg")
            ;;
    esac
done

exec /usr/local/bin/ld.lld "${ARGS[@]}"
