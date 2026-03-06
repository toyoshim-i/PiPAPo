#!/bin/bash
# Install busybox variants and applet symlinks into romfs/
#
# Three variants:
#   busybox       — full binary (transient commands: ls, cat, grep, ...)
#   busybox.init  — init-only binary (PID 1 resident)
#   busybox.sh    — shell + builtins binary (interactive shell, resident)
#
# Symlink layout:
#   /sbin/init       → busybox.init    (dedicated init)
#   /bin/sh          → busybox.sh      (dedicated shell)
#   /bin/hush        → busybox.sh      (dedicated shell)
#   /bin/ls, cat, …  → busybox         (full binary for transient commands)
#   /sbin/mount, …   → ../bin/busybox  (full binary)
#
# Usage: ./third_party/install-busybox.sh [--clean]
#   --clean   Remove busybox and applet symlinks from romfs, then exit

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BB_BUILD="$PROJECT_ROOT/build/busybox"
ROMFS_BIN="$PROJECT_ROOT/romfs/bin"
ROMFS_SBIN="$PROJECT_ROOT/romfs/sbin"

# Applets that link to full busybox (transient commands)
APPLETS=(
    cat chmod cp df echo grep head kill ln ls mkdir mv
    printf ps rm rmdir sed sleep sort tail top uname vi wc
)

# Shell applets — link to busybox.sh (dedicated shell binary)
SHELL_APPLETS=(sh hush)

# Sbin applets — link to full busybox via ../bin/busybox
SBIN_APPLETS=(mount umount getty)

# --- Handle --clean ---
if [[ "${1:-}" == "--clean" ]]; then
    echo "busybox: cleaning romfs installation..."
    rm -f "$ROMFS_BIN/busybox" "$ROMFS_BIN/busybox.sh"
    rm -f "$ROMFS_SBIN/busybox.init"
    for applet in "${APPLETS[@]}"; do
        rm -f "$ROMFS_BIN/$applet"
    done
    for applet in "${SHELL_APPLETS[@]}"; do
        rm -f "$ROMFS_BIN/$applet"
    done
    for applet in "${SBIN_APPLETS[@]}"; do
        rm -f "$ROMFS_SBIN/$applet"
    done
    rm -f "$ROMFS_SBIN/init"
    echo "busybox: clean done."
    exit 0
fi

# --- Check prerequisites ---
for bin in busybox busybox.init busybox.sh; do
    if [[ ! -f "$BB_BUILD/$bin" ]]; then
        echo "ERROR: $bin not found at $BB_BUILD/$bin" >&2
        echo "  Run: ./third_party/build-busybox.sh" >&2
        exit 1
    fi
done

# --- Skip if already installed ---
if [[ -f "$ROMFS_BIN/busybox" && -f "$ROMFS_BIN/busybox.sh" && \
      -f "$ROMFS_SBIN/busybox.init" ]]; then
    # Check if all binaries are up to date
    up_to_date=true
    for pair in "busybox:$ROMFS_BIN" "busybox.sh:$ROMFS_BIN" "busybox.init:$ROMFS_SBIN"; do
        bin="${pair%%:*}"
        dest="${pair#*:}"
        if ! cmp -s "$BB_BUILD/$bin" "$dest/$bin"; then
            up_to_date=false
            break
        fi
    done
    if $up_to_date; then
        echo "busybox: already installed in romfs — skipping."
        echo "busybox: run '$0 --clean' to force reinstall."
        exit 0
    fi
fi

# --- Install ---
echo "busybox: installing into romfs..."

mkdir -p "$ROMFS_BIN"
mkdir -p "$ROMFS_SBIN"

# Copy all three binaries
cp "$BB_BUILD/busybox" "$ROMFS_BIN/busybox"
echo "  installed: $ROMFS_BIN/busybox ($(stat -c%s "$ROMFS_BIN/busybox") bytes)"

cp "$BB_BUILD/busybox.sh" "$ROMFS_BIN/busybox.sh"
echo "  installed: $ROMFS_BIN/busybox.sh ($(stat -c%s "$ROMFS_BIN/busybox.sh") bytes)"

cp "$BB_BUILD/busybox.init" "$ROMFS_SBIN/busybox.init"
echo "  installed: $ROMFS_SBIN/busybox.init ($(stat -c%s "$ROMFS_SBIN/busybox.init") bytes)"

# Create applet symlinks → full busybox (transient commands)
for applet in "${APPLETS[@]}"; do
    rm -f "$ROMFS_BIN/$applet"
    ln -s busybox "$ROMFS_BIN/$applet"
done
echo "  symlinks: ${#APPLETS[@]} applets → busybox"

# Create shell symlinks → busybox.sh (dedicated shell)
for applet in "${SHELL_APPLETS[@]}"; do
    rm -f "$ROMFS_BIN/$applet"
    ln -s busybox.sh "$ROMFS_BIN/$applet"
done
echo "  symlinks: ${SHELL_APPLETS[*]} → busybox.sh"

# Create sbin applet symlinks → full busybox
for applet in "${SBIN_APPLETS[@]}"; do
    rm -f "$ROMFS_SBIN/$applet"
    ln -s ../bin/busybox "$ROMFS_SBIN/$applet"
done
echo "  symlinks: ${SBIN_APPLETS[*]} → ../bin/busybox"

# Create /sbin/init → busybox.init (dedicated init)
rm -f "$ROMFS_SBIN/init"
ln -s busybox.init "$ROMFS_SBIN/init"
echo "  symlinks: init → busybox.init"

# --- Summary ---
echo "busybox: installation complete."
echo "  binaries: busybox + busybox.init + busybox.sh"
total=$((${#APPLETS[@]} + ${#SHELL_APPLETS[@]} + ${#SBIN_APPLETS[@]} + 1))
echo "  symlinks: $total total (${#APPLETS[@]} applets + ${#SHELL_APPLETS[@]} shell + ${#SBIN_APPLETS[@]} sbin + 1 init)"
