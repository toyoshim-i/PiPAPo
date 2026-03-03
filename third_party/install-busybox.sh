#!/bin/bash
# Install busybox binary and applet symlinks into romfs/bin/
#
# Usage: ./third_party/install-busybox.sh [--clean]
#   --clean   Remove busybox and applet symlinks from romfs, then exit

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUSYBOX_BIN="$PROJECT_ROOT/build/busybox/busybox"
ROMFS_BIN="$PROJECT_ROOT/romfs/bin"
ROMFS_SBIN="$PROJECT_ROOT/romfs/sbin"

# Applet list — must match busybox_ppap.fragment
APPLETS=(
    cat chmod cp df echo grep head kill ln ls mkdir mv
    printf ps rm rmdir sed sleep sort tail top uname vi wc
)

# Shell applets — these link to busybox as shell interpreters
SHELL_APPLETS=(sh hush)

# Sbin applets — linked in /sbin/
SBIN_APPLETS=(mount umount)

# --- Handle --clean ---
if [[ "${1:-}" == "--clean" ]]; then
    echo "busybox: cleaning romfs installation..."
    rm -f "$ROMFS_BIN/busybox"
    for applet in "${APPLETS[@]}" "${SHELL_APPLETS[@]}"; do
        rm -f "$ROMFS_BIN/$applet"
    done
    for applet in "${SBIN_APPLETS[@]}"; do
        rm -f "$ROMFS_SBIN/$applet"
    done
    rm -f "$ROMFS_SBIN/init"
    echo "busybox: clean done."
    exit 0
fi

# --- Check prerequisite ---
if [[ ! -f "$BUSYBOX_BIN" ]]; then
    echo "ERROR: busybox binary not found at $BUSYBOX_BIN" >&2
    echo "  Run: ./third_party/build-busybox.sh" >&2
    exit 1
fi

# --- Skip if already installed ---
if [[ -f "$ROMFS_BIN/busybox" ]]; then
    # Check if binary is up to date
    if [[ "$ROMFS_BIN/busybox" -nt "$BUSYBOX_BIN" ]] || \
       cmp -s "$BUSYBOX_BIN" "$ROMFS_BIN/busybox"; then
        echo "busybox: already installed in romfs — skipping."
        echo "busybox: run '$0 --clean' to force reinstall."
        exit 0
    fi
fi

# --- Install ---
echo "busybox: installing into romfs..."

mkdir -p "$ROMFS_BIN"
mkdir -p "$ROMFS_SBIN"

# Copy busybox binary
cp -v "$BUSYBOX_BIN" "$ROMFS_BIN/busybox" | sed 's/^/  /'

# Create applet symlinks
for applet in "${APPLETS[@]}" "${SHELL_APPLETS[@]}"; do
    # Remove existing file/symlink (might be a standalone test binary)
    rm -f "$ROMFS_BIN/$applet"
    ln -sv busybox "$ROMFS_BIN/$applet" | sed 's/^/  /'
done

# Create sbin applet symlinks
for applet in "${SBIN_APPLETS[@]}"; do
    rm -f "$ROMFS_SBIN/$applet"
    ln -sv ../bin/busybox "$ROMFS_SBIN/$applet" | sed 's/^/  /'
done

# Create /sbin/init -> ../bin/busybox
rm -f "$ROMFS_SBIN/init"
ln -sv ../bin/busybox "$ROMFS_SBIN/init" | sed 's/^/  /'

# --- Verify ---
echo "busybox: installation complete."
echo "  binary: $ROMFS_BIN/busybox ($(stat -c%s "$ROMFS_BIN/busybox") bytes)"
echo "  applets: ${#APPLETS[@]} + ${#SHELL_APPLETS[@]} shell + ${#SBIN_APPLETS[@]} sbin = $((${#APPLETS[@]} + ${#SHELL_APPLETS[@]} + ${#SBIN_APPLETS[@]})) symlinks"
