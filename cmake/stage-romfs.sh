#!/bin/bash
# stage-romfs.sh --- Assemble a romfs staging directory
#
# Shared between ARM and m68k targets.  Creates the full directory
# structure, installs user ELFs, busybox symlinks, rogue, headers,
# and /etc files with target-specific overrides.
#
# Usage:
#   stage-romfs.sh STAGING_DIR PROJECT_ROOT \
#       --user-elfs ELF1 ELF2 ... \
#       --bb-dir DIR \
#       --bb-applets APP1 APP2 ... \
#       --bb-shell-applets APP1 APP2 ... \
#       --bb-sbin-applets APP1 APP2 ... \
#       --rogue PATH \
#       --etc-dir DIR \
#       --fstab PATH \
#       --inittab PATH

set -e

STAGING="$1"; shift
PROJECT_ROOT="$1"; shift

USER_ELFS=()
BB_DIR=""
BB_APPLETS=()
BB_SHELL_APPLETS=()
BB_SBIN_APPLETS=()
ROGUE=""
ETC_DIR=""
FSTAB=""
INITTAB=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --user-elfs)
            shift
            while [[ $# -gt 0 && ! "$1" == --* ]]; do
                USER_ELFS+=("$1"); shift
            done
            ;;
        --bb-dir)
            BB_DIR="$2"; shift 2
            ;;
        --bb-applets)
            shift
            while [[ $# -gt 0 && ! "$1" == --* ]]; do
                BB_APPLETS+=("$1"); shift
            done
            ;;
        --bb-shell-applets)
            shift
            while [[ $# -gt 0 && ! "$1" == --* ]]; do
                BB_SHELL_APPLETS+=("$1"); shift
            done
            ;;
        --bb-sbin-applets)
            shift
            while [[ $# -gt 0 && ! "$1" == --* ]]; do
                BB_SBIN_APPLETS+=("$1"); shift
            done
            ;;
        --rogue)
            ROGUE="$2"; shift 2
            ;;
        --etc-dir)
            ETC_DIR="$2"; shift 2
            ;;
        --fstab)
            FSTAB="$2"; shift 2
            ;;
        --inittab)
            INITTAB="$2"; shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2; exit 1
            ;;
    esac
done

# --- Create directory structure ---
rm -rf "$STAGING"
mkdir -p "$STAGING/bin" "$STAGING/sbin" "$STAGING/etc" "$STAGING/dev"
mkdir -p "$STAGING/proc" "$STAGING/tmp" "$STAGING/usr/bin" "$STAGING/usr/include"
mkdir -p "$STAGING/mnt/sd"

# --- Install user programs ---
for elf in "${USER_ELFS[@]}"; do
    name=$(basename "$elf" .elf)
    case "$name" in
        init)   cp "$elf" "$STAGING/sbin/$name" ;;
        ttyctl) cp "$elf" "$STAGING/usr/bin/$name" ;;
        *)      cp "$elf" "$STAGING/bin/$name" ;;
    esac
done

# --- Install busybox (if available) ---
if [[ -n "$BB_DIR" && -f "$BB_DIR/busybox" ]]; then
    cp "$BB_DIR/busybox" "$STAGING/bin/busybox"
    cp "$BB_DIR/busybox.sh" "$STAGING/bin/busybox.sh"
    for a in "${BB_APPLETS[@]}"; do ln -sf busybox "$STAGING/bin/$a"; done
    for a in "${BB_SHELL_APPLETS[@]}"; do ln -sf busybox.sh "$STAGING/bin/$a"; done
    for a in "${BB_SBIN_APPLETS[@]}"; do ln -sf ../bin/busybox "$STAGING/sbin/$a"; done
fi

# --- Install rogue (if available) ---
if [[ -n "$ROGUE" && -f "$ROGUE" ]]; then
    cp "$ROGUE" "$STAGING/bin/rogue"
fi

# --- Install shared ABI headers ---
cp "$PROJECT_ROOT/src/common/"*.h "$STAGING/usr/include/"

# --- Copy /etc files ---
if [[ -n "$ETC_DIR" && -d "$ETC_DIR" ]]; then
    cp -a "$ETC_DIR/." "$STAGING/etc/"
fi

# --- Apply target-specific overrides ---
if [[ -n "$FSTAB" && -f "$FSTAB" ]]; then
    cp "$FSTAB" "$STAGING/etc/fstab"
fi
if [[ -n "$INITTAB" && -f "$INITTAB" ]]; then
    cp "$INITTAB" "$STAGING/etc/inittab"
fi
