#!/usr/bin/env python3
"""Capture UART test output until a suite result marker or timeout."""

import argparse
import os
import select
import sys
import termios
import time


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument("--port", required=True)
  parser.add_argument("--timeout", type=float, default=180.0)
  args = parser.parse_args()

  fd = os.open(args.port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
  old = termios.tcgetattr(fd)
  attrs = termios.tcgetattr(fd)
  attrs[0] = 0
  attrs[1] = 0
  attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
  attrs[3] = 0
  attrs[4] = termios.B115200
  attrs[5] = termios.B115200
  termios.tcsetattr(fd, termios.TCSANOW, attrs)

  deadline = time.monotonic() + args.timeout
  output = bytearray()
  try:
    while time.monotonic() < deadline:
      ready, _, _ = select.select([fd], [], [], 1.0)
      if not ready:
        continue
      chunk = os.read(fd, 4096)
      if not chunk:
        continue
      sys.stdout.buffer.write(chunk)
      sys.stdout.buffer.flush()
      output.extend(chunk)
      if (b"ALL TESTS PASSED" in output or
          b"SOME TESTS FAILED" in output or
          b"KERNEL TESTS FAILED" in output):
        return 0
  finally:
    termios.tcsetattr(fd, termios.TCSANOW, old)
    os.close(fd)
  return 0


if __name__ == "__main__":
  sys.exit(main())
