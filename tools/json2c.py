#!/usr/bin/env python3
"""
json2c.py — Convert IchigoJam font JSON to a C array source file.

Reads a JSON array of 256 hex strings (each 16 hex chars = 8 bytes)
and emits a C source file with a const uint8_t[256][8] array.

Usage:
    python3 json2c.py --input font.json --output font8x8.c --name font8x8
"""

import argparse
import json
import sys


def main():
    parser = argparse.ArgumentParser(
        description='Convert IchigoJam font JSON to C array')
    parser.add_argument('--input', required=True, help='Input JSON file')
    parser.add_argument('--output', required=True, help='Output C file')
    parser.add_argument('--name', required=True, help='Array name')
    args = parser.parse_args()

    with open(args.input) as f:
        data = json.load(f)

    if not isinstance(data, list) or len(data) != 256:
        print(f'Error: expected JSON array of 256 entries, got {len(data)}',
              file=sys.stderr)
        sys.exit(1)

    with open(args.output, 'w') as f:
        f.write('/* Auto-generated from IchigoJam font (CC BY / MIT) '
                '— do not edit */\n')
        f.write('#include <stdint.h>\n\n')
        f.write(f'const uint8_t {args.name}[256][8] = {{\n')

        for cp in range(256):
            s = data[cp]
            rows = [int(s[i:i+2], 16) for i in range(0, 16, 2)]
            hex_str = ', '.join(f'0x{b:02X}' for b in rows)
            label = ''
            if 0x20 <= cp <= 0x7E:
                ch = chr(cp)
                if ch == '\\':
                    label = "  /* '\\\\' */"
                elif ch == "'":
                    label = "  /* '\\'' */"
                else:
                    label = f"  /* '{ch}' */"
            elif cp < 0x20:
                label = f'  /* 0x{cp:02X} */'
            f.write(f'    {{ {hex_str} }},{label}\n')

        f.write('};\n')

    print(f'json2c: 256 glyphs from {args.input} -> {args.output}')


if __name__ == '__main__':
    main()
