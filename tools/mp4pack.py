#!/usr/bin/env python3
"""List, extract and inspect Mario Party 4 .bin data packs.

Use this on packs from your own disc (extract them with Dolphin: right-click the game,
Properties > Filesystem > data > Extract File) to find which file index to replace in a mod.

    mp4pack.py list mariomdl0.bin
    mp4pack.py extract mariomdl0.bin out/mariomdl0
    mp4pack.py extract mariomdl0.bin out/mariomdl0 --index 0

Extracted files are decompressed, so they can be edited and dropped straight into a mod at
overlay/data/<pack>.bin/<index>.<ext>. Never commit or share files extracted from the game.
"""

import argparse
import os
import struct
import sys

DECODE_NAMES = {0: "none", 1: "lz", 2: "slide", 3: "fslide_alt", 4: "fslide", 5: "rle"}


def decode_none(src, size):
    return bytes(src[:size])


def decode_lz(src, size):
    text = bytearray(1024)
    pos = 958
    out = bytearray()
    flag = 0
    s = 0
    while len(out) < size:
        flag >>= 1
        if not flag & 0x100:
            flag = src[s] | 0xFF00
            s += 1
        if flag & 1:
            b = src[s]
            s += 1
            out.append(b)
            text[pos] = b
            pos = (pos + 1) & 0x3FF
        else:
            i = src[s]
            copy_len = src[s + 1]
            s += 2
            i |= (copy_len & ~0x3F) << 2
            copy_len = (copy_len & 0x3F) + 3
            for j in range(copy_len):
                b = text[(i + j) & 0x3FF]
                out.append(b)
                text[pos] = b
                pos = (pos + 1) & 0x3FF
    return bytes(out[:size])


def decode_slide(src, size, zero_fill_before_start):
    s = 4  # big-endian size header, unused
    out = bytearray()
    flag = 0
    flag_len = 0
    while len(out) < size:
        if flag_len == 0:
            flag = struct.unpack_from(">I", src, s)[0]
            s += 4
            flag_len = 32
        if flag >> 31:
            out.append(src[s])
            s += 1
        else:
            dist = (src[s] << 8) | src[s + 1]
            s += 2
            length = (dist >> 12) & 0xF
            dist &= 0xFFF
            if length == 0:
                length = src[s] + 18
                s += 1
            else:
                length += 2
            start = len(out) - dist - 1
            for k in range(length):
                p = start + k
                if p < 0:
                    if not zero_fill_before_start:
                        raise ValueError("slide reference before start of data")
                    out.append(0)
                else:
                    out.append(out[p])
        flag = (flag << 1) & 0xFFFFFFFF
        flag_len -= 1
    return bytes(out[:size])


def decode_rle(src, size):
    out = bytearray()
    s = 0
    while len(out) < size:
        n = src[s]
        s += 1
        if n < 128:
            out += bytes([src[s]]) * n
            s += 1
        else:
            n -= 128
            out += src[s:s + n]
            s += n
    return bytes(out[:size])


def decode(src, size, decode_type):
    if decode_type == 0:
        return decode_none(src, size)
    if decode_type == 1:
        return decode_lz(src, size)
    if decode_type == 2:
        return decode_slide(src, size, True)
    if decode_type in (3, 4):
        return decode_slide(src, size, False)
    if decode_type == 5:
        return decode_rle(src, size)
    raise ValueError(f"unknown decode type {decode_type}")


def read_pack(path):
    with open(path, "rb") as f:
        data = f.read()
    (count,) = struct.unpack_from(">I", data, 0)
    if count == 0 or 4 + count * 4 > len(data):
        raise ValueError(f"{path} does not look like a data pack")
    offsets = list(struct.unpack_from(f">{count}I", data, 4))
    entries = []
    for i, ofs in enumerate(offsets):
        raw_len, decode_type = struct.unpack_from(">II", data, ofs)
        ends = [o for o in offsets if o > ofs] + [len(data)]
        entries.append((i, ofs, raw_len, decode_type, data[ofs + 8:min(ends)]))
    return entries


def guess_extension(data):
    if data.startswith(b"HSFV"):
        return "hsf"
    if data.startswith(b"ANIM") or data[:4] == b"\x00\x00\x00\x00" and data[4:8] == b"ANIM":
        return "anm"
    return "dat"


def cmd_list(args):
    for i, ofs, raw_len, decode_type, payload in read_pack(args.pack):
        try:
            ext = guess_extension(decode(payload, min(raw_len, 64), decode_type))
        except (IndexError, ValueError):
            ext = "?"
        name = DECODE_NAMES.get(decode_type, str(decode_type))
        print(f"{i:4d}  0x{i:04X}  {raw_len:9d} bytes  {name:10s}  {ext}")


def cmd_extract(args):
    os.makedirs(args.out, exist_ok=True)
    for i, ofs, raw_len, decode_type, payload in read_pack(args.pack):
        if args.index is not None and i != args.index:
            continue
        data = decode(payload, raw_len, decode_type)
        path = os.path.join(args.out, f"{i}.{guess_extension(data)}")
        with open(path, "wb") as f:
            f.write(data)
        print(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("list", help="list the files in a pack")
    p.add_argument("pack")
    p.set_defaults(func=cmd_list)
    p = sub.add_parser("extract", help="decompress files from a pack")
    p.add_argument("pack")
    p.add_argument("out")
    p.add_argument("--index", type=lambda v: int(v, 0))
    p.set_defaults(func=cmd_extract)
    args = parser.parse_args()
    try:
        args.func(args)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
