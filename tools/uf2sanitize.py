#!/usr/bin/env python3
"""
uf2sanitize.py — Post-process a UF2 file for the PicoCalc UF2 bootloader.

The pelrun/uf2loader requires contiguous, sequential UF2 blocks with no
address gaps.  The Pico SDK's elf2uf2 may produce non-contiguous blocks
(e.g., boot2 region + kernel region with a gap between them).

This script:
  1. Reads the input UF2 file.
  2. Filters out blocks below a configurable start address (default:
     0x10004000 to skip the bootloader's reserved 16KB).
  3. Fills any internal address gaps with 0xFF padding blocks.
  4. Renumbers all blocks sequentially.
  5. Writes the sanitized UF2.

Usage:
  python3 uf2sanitize.py INPUT.uf2 OUTPUT.uf2 [--start 0x10004000]
"""

import argparse
import struct
import sys

UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56
BLOCK_SIZE = 512
PAYLOAD_SIZE = 256


def parse_args():
    p = argparse.ArgumentParser(description="Sanitize UF2 for PicoCalc bootloader")
    p.add_argument("input", help="Input UF2 file")
    p.add_argument("output", help="Output UF2 file")
    p.add_argument("--start", type=lambda x: int(x, 0), default=0x10004000,
                    help="Minimum target address to keep (default: 0x10004000)")
    return p.parse_args()


def read_blocks(path):
    """Read UF2 file and return list of (target_addr, flags, family_id, payload)."""
    with open(path, "rb") as f:
        data = f.read()
    blocks = []
    for off in range(0, len(data), BLOCK_SIZE):
        hdr = struct.unpack_from("<IIIIIIII", data, off)
        magic0, magic1, flags, target_addr, payload_size, block_no, num_blocks, family_id = hdr
        if magic0 != UF2_MAGIC0 or magic1 != UF2_MAGIC1:
            print(f"WARNING: bad magic at offset {off}, skipping", file=sys.stderr)
            continue
        payload = data[off + 32 : off + 32 + PAYLOAD_SIZE]
        blocks.append((target_addr, flags, family_id, payload))
    return blocks


def write_uf2(path, blocks):
    """Write list of (target_addr, flags, family_id, payload) as UF2."""
    num_blocks = len(blocks)
    with open(path, "wb") as f:
        for i, (addr, flags, family_id, payload) in enumerate(blocks):
            hdr = struct.pack("<IIIIIIII",
                              UF2_MAGIC0, UF2_MAGIC1, flags,
                              addr, PAYLOAD_SIZE, i, num_blocks, family_id)
            # Pad payload to 476 bytes (512 - 32 header - 4 final magic)
            padded = payload + b"\x00" * (BLOCK_SIZE - 32 - 4 - len(payload))
            f.write(hdr + padded + struct.pack("<I", UF2_MAGIC_END))


def main():
    args = parse_args()
    blocks = read_blocks(args.input)
    if not blocks:
        print("ERROR: no valid UF2 blocks found", file=sys.stderr)
        sys.exit(1)

    # Filter blocks below start address
    kept = [(addr, fl, fam, pay) for addr, fl, fam, pay in blocks if addr >= args.start]
    removed = len(blocks) - len(kept)
    if removed:
        print(f"Removed {removed} blocks below 0x{args.start:08X}")

    if not kept:
        print("ERROR: no blocks remaining after filtering", file=sys.stderr)
        sys.exit(1)

    # Sort by address
    kept.sort(key=lambda b: b[0])

    # Use flags/family from first block as template
    tmpl_flags = kept[0][1]
    tmpl_family = kept[0][2]

    # Fill gaps with 0xFF padding blocks
    filled = [kept[0]]
    for i in range(1, len(kept)):
        prev_addr = filled[-1][0]
        cur_addr = kept[i][0]
        expected = prev_addr + PAYLOAD_SIZE
        while expected < cur_addr:
            filled.append((expected, tmpl_flags, tmpl_family, b"\xFF" * PAYLOAD_SIZE))
            expected += PAYLOAD_SIZE
        filled.append(kept[i])

    gap_blocks = len(filled) - len(kept)
    if gap_blocks:
        print(f"Added {gap_blocks} padding blocks to fill gaps")

    write_uf2(args.output, filled)
    print(f"Wrote {len(filled)} blocks to {args.output} "
          f"(0x{filled[0][0]:08X}–0x{filled[-1][0] + PAYLOAD_SIZE:08X})")


if __name__ == "__main__":
    main()
