#!/usr/bin/env python3
"""xtensa_cc_test_monitor.py — Capture xtensa_cc serial output for `--test`.

Reads from the device's USB Serial JTAG endpoint, mirrors everything to
stdout for visibility, and stops as soon as runtests prints either
``ALL TESTS PASSED`` or ``SOME TESTS FAILED`` (or after a hard timeout).

Designed to run inside the ppap/xtensa Docker container, which provides
pyserial via ESP-IDF's virtualenv.  Invoked from scripts/run.sh's
xtensa_cc test-mode branch — not a standalone tool.
"""
from __future__ import annotations

import argparse
import sys
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=180.0,
                        help="overall capture timeout in seconds")
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=1)
    deadline = time.time() + args.timeout
    buf = bytearray()
    pass_marker = b"ALL TESTS PASSED"
    fail_marker = b"SOME TESTS FAILED"

    while time.time() < deadline:
        chunk = ser.read(4096)
        if not chunk:
            continue
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        buf.extend(chunk)
        if pass_marker in buf or fail_marker in buf:
            return 0  # exit normally; run.sh greps the captured output
    return 0


if __name__ == "__main__":
    sys.exit(main())
