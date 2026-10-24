#!/usr/bin/env python3
"""Recolor the textures of Mario Party 4 HSF models, and build recolored character mods.

Only pixels whose hue falls in a chosen range are changed, so a green Yoshi can become red
while his shoes, eyes and saddle keep their colors.

    mp4recolor.py hsf in.hsf out.hsf --from 70:170 --to 0
    mp4recolor.py character yoshi yoshimdl0.bin yoshimdl1.bin out_mod_folder \\
        --color red:0 --color blue:220 --color black:0:0.15

`character` writes a mod folder (copy it into <data folder>/mods/) with one mod character per
--color NAME:HUE[:VALUE_SCALE[:SAT_SCALE]]. Hues are in degrees. Run it on packs from your own
disc only, and never commit or share the output: it contains the game's textures.
"""

import argparse
import colorsys
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp4pack  # noqa: E402

HEADER_SIZE = 8 + 21 * 8
SECTION_NAMES = ["scene", "color", "material", "attribute", "vertex", "normal", "st", "face", "object",
                 "bitmap", "palette", "motion", "cenv", "skeleton", "part", "cluster", "shape", "mapAttr",
                 "matrix", "symbol", "string"]

FMT_RGB565, FMT_RGB5A3, FMT_RGBA8, FMT_CMPR = 4, 5, 6, 7
FMT_CI_RGB565, FMT_CI_RGB5A3 = 9, 10

# Default hue range for Yoshi's green, in degrees.
DEFAULT_FROM = (70.0, 170.0)


class Recolor:
    def __init__(self, hue_from, hue_to, value_scale=1.0, sat_scale=1.0):
        self.lo, self.hi = hue_from
        self.to = hue_to
        self.value_scale = value_scale
        self.sat_scale = sat_scale
        self.center = (self.lo + self.hi) / 2
        self.changed = 0

    def rgb(self, r, g, b):
        h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        hd = h * 360
        if s < 0.15 or not (self.lo <= hd <= self.hi):
            return r, g, b
        self.changed += 1
        # Keep the shading variation inside the range by shifting relative to its center.
        nh = ((self.to + (hd - self.center)) % 360) / 360
        s = min(1.0, s * self.sat_scale)
        v = min(1.0, v * self.value_scale)
        nr, ng, nb = colorsys.hsv_to_rgb(nh, s, v)
        return round(nr * 255), round(ng * 255), round(nb * 255)

    def rgb565(self, c):
        r = (c >> 11) & 0x1F
        g = (c >> 5) & 0x3F
        b = c & 0x1F
        r, g, b = self.rgb((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

    def rgb5a3(self, c):
        if c & 0x8000:
            r = (c >> 10) & 0x1F
            g = (c >> 5) & 0x1F
            b = c & 0x1F
            r, g, b = self.rgb((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2))
            return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
        a = c & 0x7000
        r = (c >> 8) & 0xF
        g = (c >> 4) & 0xF
        b = c & 0xF
        r, g, b = self.rgb(r * 17, g * 17, b * 17)
        return a | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)


def tex_size(fmt, w, h):
    bpp = {0: 4, 1: 8, 2: 8, 3: 16, 4: 16, 5: 16, 6: 32, 7: 4}.get(fmt)
    if bpp is None:
        return None
    bw, bh = {4: (8, 8), 8: (8, 4), 16: (4, 4), 32: (4, 4)}[bpp]
    return ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bw * bh * bpp // 8


def recolor_u16s(buf, start, count, fn):
    for i in range(count):
        o = start + i * 2
        (c,) = struct.unpack_from(">H", buf, o)
        struct.pack_into(">H", buf, o, fn(c))


def recolor_rgba8(buf, start, size, rc):
    for blk in range(start, start + size, 64):
        for p in range(16):
            r, g = buf[blk + p * 2 + 1], buf[blk + 32 + p * 2]
            b = buf[blk + 32 + p * 2 + 1]
            r, g, b = rc.rgb(r, g, b)
            buf[blk + p * 2 + 1] = r
            buf[blk + 32 + p * 2] = g
            buf[blk + 32 + p * 2 + 1] = b


def recolor_cmpr(buf, start, size, rc):
    for blk in range(start, start + size, 8):
        c0, c1, idx = struct.unpack_from(">HHI", buf, blk)
        n0, n1 = rc.rgb565(c0), rc.rgb565(c1)
        # Keep each block's mode (c0 > c1: four colors, otherwise three colors and transparent) by
        # swapping the endpoints back into order and remapping the indices.
        if c0 > c1:
            if n0 < n1:
                n0, n1 = n1, n0
                idx = swap_indices(idx, {0: 1, 1: 0, 2: 3, 3: 2})
            elif n0 == n1:
                idx = 0
        elif n0 > n1:
            n0, n1 = n1, n0
            idx = swap_indices(idx, {0: 1, 1: 0, 2: 2, 3: 3})
        struct.pack_into(">HHI", buf, blk, n0, n1, idx)


def swap_indices(idx, mapping):
    out = 0
    for i in range(16):
        shift = 30 - i * 2
        out |= mapping[(idx >> shift) & 3] << shift
    return out


def recolor_hsf(data, rc):
    buf = bytearray(data)
    if not buf.startswith(b"HSFV"):
        raise ValueError("not an HSF model")
    sections = {}
    for i, name in enumerate(SECTION_NAMES):
        sections[name] = struct.unpack_from(">ii", buf, 8 + i * 8)

    pal_ofs, pal_count = sections["palette"]
    pal_data_base = pal_ofs + pal_count * 16
    palettes = []
    for i in range(pal_count):
        _name, _unk, pal_size, data_ofs = struct.unpack_from(">IiII", buf, pal_ofs + i * 16)
        palettes.append((pal_size, pal_data_base + data_ofs))

    bmp_ofs, bmp_count = sections["bitmap"]
    bmp_data_base = bmp_ofs + bmp_count * 32
    done_palettes = set()
    for i in range(bmp_count):
        e = bmp_ofs + i * 32
        _name, max_lod, fmt, _pix, w, h, _pal_size = struct.unpack_from(">IIBBhhh", buf, e)
        pal_index, _unk, data_ofs = struct.unpack_from(">iII", buf, e + 20)
        start = bmp_data_base + data_ofs
        if fmt in (FMT_CI_RGB565, FMT_CI_RGB5A3):
            if 0 <= pal_index < len(palettes) and pal_index not in done_palettes:
                done_palettes.add(pal_index)
                size, pstart = palettes[pal_index]
                recolor_u16s(buf, pstart, size, rc.rgb565 if fmt == FMT_CI_RGB565 else rc.rgb5a3)
            continue
        levels = max(1, max_lod)
        for _ in range(levels):
            size = tex_size(fmt, w, h)
            if size is None or start + size > len(buf):
                break
            if fmt == FMT_RGB565:
                recolor_u16s(buf, start, size // 2, rc.rgb565)
            elif fmt == FMT_RGB5A3:
                recolor_u16s(buf, start, size // 2, rc.rgb5a3)
            elif fmt == FMT_RGBA8:
                recolor_rgba8(buf, start, size, rc)
            elif fmt == FMT_CMPR:
                recolor_cmpr(buf, start, size, rc)
            else:
                break
            start += size
            w, h = max(1, w // 2), max(1, h // 2)

    # Material colors (some parts are colored without a texture).
    mat_ofs, mat_count = sections["material"]
    mat_size = 0x3C
    for i in range(mat_count):
        e = mat_ofs + i * mat_size
        for field in (11, 14, 17):  # litColor, color, shadowColor
            r, g, b = buf[e + field:e + field + 3]
            buf[e + field:e + field + 3] = bytes(rc.rgb(r, g, b))

    return bytes(buf)


def parse_range(text):
    lo, hi = text.split(":")
    return float(lo), float(hi)


def cmd_hsf(args):
    rc = Recolor(parse_range(args.hue_from), args.to, args.value, args.sat)
    with open(args.input, "rb") as f:
        data = f.read()
    with open(args.output, "wb") as f:
        f.write(recolor_hsf(data, rc))
    print(f"{rc.changed} colors changed")


def cmd_character(args):
    base = args.base
    packs = {"mdl0": args.mdl0, "mdl1": args.mdl1}
    models = {}
    for kind, path in packs.items():
        models[kind] = [(i, mp4pack.decode(payload, raw_len, dt))
                        for i, _ofs, raw_len, dt, payload in mp4pack.read_pack(path)]
    characters = []
    for spec in args.color:
        parts = spec.split(":")
        name, hue = parts[0], float(parts[1])
        value = float(parts[2]) if len(parts) > 2 else 1.0
        sat = float(parts[3]) if len(parts) > 3 else 1.0
        char_id = f"{name.lower()}{base}"
        total = 0
        for kind, files in models.items():
            for index, data in files:
                if not data.startswith(b"HSFV"):
                    continue
                rc = Recolor(parse_range(args.hue_from), hue, value, sat)
                out = recolor_hsf(data, rc)
                if rc.changed == 0:
                    continue
                total += rc.changed
                path = os.path.join(args.out, "characters", char_id, kind, f"{index}.hsf")
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(out)
        print(f"{char_id}: {total} colors changed")
        characters.append({"id": char_id, "name": f"{name.capitalize()} {base.capitalize()}", "base": base, "costume": True})
    mod = {
        "id": f"local.{base}_colors",
        "name": f"{base.capitalize()} Colors",
        "version": "1.0.0",
        "description": f"Recolored {base.capitalize()}s, made from your own disc with tools/mp4recolor.py.",
        "characters": characters,
    }
    with open(os.path.join(args.out, "mod.json"), "w") as f:
        json.dump(mod, f, indent=2)
    print(os.path.join(args.out, "mod.json"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("hsf", help="recolor one HSF model")
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--from", dest="hue_from", default=f"{DEFAULT_FROM[0]:g}:{DEFAULT_FROM[1]:g}")
    p.add_argument("--to", type=float, required=True)
    p.add_argument("--value", type=float, default=1.0)
    p.add_argument("--sat", type=float, default=1.0)
    p.set_defaults(func=cmd_hsf)
    p = sub.add_parser("character", help="build a mod of recolored characters from a character's packs")
    p.add_argument("base", choices=["mario", "luigi", "peach", "yoshi", "wario", "donkey", "daisy", "waluigi"])
    p.add_argument("mdl0")
    p.add_argument("mdl1")
    p.add_argument("out")
    p.add_argument("--from", dest="hue_from", default=f"{DEFAULT_FROM[0]:g}:{DEFAULT_FROM[1]:g}")
    p.add_argument("--color", action="append", required=True, help="NAME:HUE[:VALUE_SCALE[:SAT_SCALE]]")
    p.set_defaults(func=cmd_character)
    args = parser.parse_args()
    try:
        args.func(args)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
