#!/usr/bin/env python3
"""Print a Foundation 0x5506 resize request, and optionally drop resize.txt.

The live control stream is AES-GCM (protocol 13). A raw 0x5506 datagram is
dropped. This script prints the plaintext Rig must encrypt with the session
key, the same way it sends every other control message. `--write` is the
host-side harness: Eddyshine consumes `%ProgramData%\\Rig\\resize.txt` once
and runs the same resize path.

`--write` opts that session into 0x5507. A stock Moonlight client can freeze
when it receives 0x5507. Use it with a client that understands the message.
"""

from __future__ import annotations

import argparse
import os
import sys


def _u16(value: int) -> bytes:
    return int(value).to_bytes(2, "little")


def _u32(value: int) -> bytes:
    return int(value).to_bytes(4, "little", signed=False)


def _i32(value: int) -> bytes:
    return int(value).to_bytes(4, "little", signed=True)


def encode_resolution_request(width: int, height: int) -> bytes:
    """16-byte 0x5506 plaintext, header included."""
    return _u16(0x5506) + _u16(12) + _i32(0) + _i32(width) + _i32(height)


def encode_resolution_notify(width: int, height: int) -> bytes:
    """12-byte 0x5507 plaintext, header included."""
    return _u16(0x5507) + _u16(8) + _u32(width) + _u32(height)


def _hex(blob: bytes) -> str:
    return blob.hex()


def rig_dir(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    env = os.environ.get("RIG_DATA_DIR")
    if env:
        return env
    program_data = os.environ.get("ProgramData")
    if program_data:
        return os.path.join(program_data, "Rig")
    if os.name == "nt":
        return os.path.join(os.environ.get("SystemDrive", "C:") + "\\", "ProgramData", "Rig")
    return None


def self_test() -> None:
    request = encode_resolution_request(1280, 720)
    notify = encode_resolution_notify(1280, 720)
    if request != bytes.fromhex("06550c000000000000050000d0020000"):
        raise SystemExit(f"request vector mismatch: {_hex(request)}")
    if notify != bytes.fromhex("0755080000050000d0020000"):
        raise SystemExit(f"notify vector mismatch: {_hex(notify)}")
    print("ok")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--width", type=int)
    parser.add_argument("--height", type=int)
    parser.add_argument("--write", action="store_true", help="write resize.txt and delete any previous one")
    parser.add_argument("--dir", help="directory for resize.txt (default: RIG_DATA_DIR or %%ProgramData%%\\Rig)")
    parser.add_argument("--self-test", action="store_true", help="check the plaintext vectors and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        self_test()
        return 0

    if args.width is None or args.height is None:
        parser.error("--width and --height are required unless --self-test")

    request = encode_resolution_request(args.width, args.height)
    notify = encode_resolution_notify(args.width, args.height)
    print(f"0x5506 plaintext ({len(request)} bytes): {_hex(request)}")
    print(f"0x5507 plaintext ({len(notify)} bytes): {_hex(notify)}")
    print("Encrypt the 0x5506 plaintext on the control stream. Do not send it in the clear.")

    if not args.write:
        return 0

    directory = rig_dir(args.dir)
    if not directory:
        print("No Rig directory. Pass --dir or set RIG_DATA_DIR / ProgramData.", file=sys.stderr)
        return 1
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, "resize.txt")
    with open(path, "w", encoding="ascii", newline="\n") as handle:
        handle.write(f"width={args.width}\nheight={args.height}\n")
    print(f"wrote {path}")
    print("The next RUNNING session deletes this file and applies it.")
    print("That opts the session into 0x5507. Stock Moonlight can freeze.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
