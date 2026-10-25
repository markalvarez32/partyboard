#!/usr/bin/env python3
"""Read and replace the images in Mario Party 4 sprite (ANM) files.

    mp4anm.py list sprite.anm
    mp4anm.py export sprite.anm out.png [--bmp N]
    mp4anm.py replace sprite.anm picture.png out.anm [--bmp N]

`replace` scales the picture to the bitmap's size and encodes it in the bitmap's own format, so the
sprite keeps its layout and animation. Paletted bitmaps get a new palette of the same size.
"""

import argparse
import struct
import sys

from PIL import Image

FMT_RGBA8, FMT_RGB5A3, FMT_RGB5A3_DUPE, FMT_C8, FMT_C4 = 0, 1, 2, 3, 4
FMT_IA8, FMT_IA4, FMT_I8, FMT_I4, FMT_A8, FMT_CMPR = 5, 6, 7, 8, 9, 10
FMT_NAMES = ["RGBA8", "RGB5A3", "RGB5A3", "C8", "C4", "IA8", "IA4", "I8", "I4", "A8", "CMPR"]

# Block size in pixels and bits per pixel, by format.
BLOCKS = {FMT_RGBA8: (4, 4, 32), FMT_RGB5A3: (4, 4, 16), FMT_RGB5A3_DUPE: (4, 4, 16), FMT_C8: (8, 4, 8),
          FMT_C4: (8, 8, 4), FMT_IA8: (4, 4, 16), FMT_IA4: (8, 4, 8), FMT_I8: (8, 4, 8), FMT_I4: (8, 8, 4),
          FMT_A8: (8, 4, 8), FMT_CMPR: (8, 8, 4)}

BMP_SIZE = 20


class Bitmap:
    def __init__(self, data, offset):
        self.offset = offset
        (self.pix_size, self.fmt, self.pal_num, self.w, self.h, self.data_size, self.pal_ofs,
         self.data_ofs) = struct.unpack_from(">BBhhhIII", data, offset)
        self.fmt &= 0xF
        self.pal_num &= 0x7FFF

    def __str__(self):
        name = FMT_NAMES[self.fmt] if self.fmt < len(FMT_NAMES) else str(self.fmt)
        pal = f", {self.pal_num} colors" if self.fmt in (FMT_C8, FMT_C4) else ""
        return f"{self.w}x{self.h} {name}{pal}"


def bitmaps(data):
    bmp_num = struct.unpack_from(">h", data, 4)[0]
    bmp_ofs = struct.unpack_from(">I", data, 16)[0]
    return [Bitmap(data, bmp_ofs + i * BMP_SIZE) for i in range(bmp_num)]


def rgb5a3_to_rgba(c):
    if c & 0x8000:
        r, g, b = (c >> 10) & 0x1F, (c >> 5) & 0x1F, c & 0x1F
        return (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255
    a, r, g, b = (c >> 12) & 7, (c >> 8) & 0xF, (c >> 4) & 0xF, c & 0xF
    return r * 17, g * 17, b * 17, (a << 5) | (a << 2) | (a >> 1)


def rgba_to_rgb5a3(r, g, b, a):
    if a >= 0xE0:
        return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
    return ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)


def rgb565_to_rgba(c):
    r, g, b = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255


def block_order(w, h, bw, bh):
    """Pixel coordinates in the order GX stores them for a w x h texture with bw x bh blocks."""
    for by in range(0, h, bh):
        for bx in range(0, w, bw):
            for y in range(by, by + bh):
                for x in range(bx, bx + bw):
                    yield x, y


def decode_cmpr(data, start, w, h):
    img = Image.new("RGBA", (w, h))
    px = img.load()
    o = start
    for by in range(0, h, 8):
        for bx in range(0, w, 8):
            for sub in range(4):
                c0, c1, idx = struct.unpack_from(">HHI", data, o)
                o += 8
                a, b = rgb565_to_rgba(c0), rgb565_to_rgba(c1)
                if c0 > c1:
                    pal = [a, b, tuple((2 * a[i] + b[i]) // 3 for i in range(3)) + (255,),
                           tuple((a[i] + 2 * b[i]) // 3 for i in range(3)) + (255,)]
                else:
                    pal = [a, b, tuple((a[i] + b[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]
                sx, sy = bx + (sub % 2) * 4, by + (sub // 2) * 4
                for p in range(16):
                    x, y = sx + p % 4, sy + p // 4
                    if x < w and y < h:
                        px[x, y] = pal[(idx >> (30 - 2 * p)) & 3]
    return img


def decode(data, bmp):
    w, h, fmt = bmp.w, bmp.h, bmp.fmt
    if fmt == FMT_CMPR:
        return decode_cmpr(data, bmp.data_ofs, w, h)
    bw, bh, bpp = BLOCKS[fmt]
    img = Image.new("RGBA", (w, h))
    px = img.load()
    palette = []
    if fmt in (FMT_C8, FMT_C4):
        palette = [rgb5a3_to_rgba(struct.unpack_from(">H", data, bmp.pal_ofs + i * 2)[0]) for i in range(bmp.pal_num)]
    o = bmp.data_ofs
    coords = list(block_order(w, h, bw, bh))
    if fmt == FMT_RGBA8:
        # Each 4x4 block holds 16 AR pairs, then 16 GB pairs.
        for blk in range(0, len(coords), 16):
            for p in range(16):
                x, y = coords[blk + p]
                a, r = data[o + p * 2], data[o + p * 2 + 1]
                g, b = data[o + 32 + p * 2], data[o + 32 + p * 2 + 1]
                if x < w and y < h:
                    px[x, y] = (r, g, b, a)
            o += 64
        return img
    for i, (x, y) in enumerate(coords):
        if bpp == 4:
            v = (data[o + i // 2] >> (4 if i % 2 == 0 else 0)) & 0xF
        elif bpp == 8:
            v = data[o + i]
        else:
            v = struct.unpack_from(">H", data, o + i * 2)[0]
        if x >= w or y >= h:
            continue
        if fmt in (FMT_C8, FMT_C4):
            px[x, y] = palette[v] if v < len(palette) else (0, 0, 0, 0)
        elif fmt in (FMT_RGB5A3, FMT_RGB5A3_DUPE):
            px[x, y] = rgb5a3_to_rgba(v)
        elif fmt == FMT_IA8:
            px[x, y] = (v & 0xFF,) * 3 + (v >> 8,)
        elif fmt == FMT_IA4:
            px[x, y] = ((v & 0xF) * 17,) * 3 + ((v >> 4) * 17,)
        elif fmt == FMT_I8:
            px[x, y] = (v, v, v, v)
        elif fmt == FMT_I4:
            px[x, y] = (v * 17,) * 4
        elif fmt == FMT_A8:
            px[x, y] = (255, 255, 255, v)
    return img


def fit(picture, w, h):
    """Scale the picture to fit inside w x h, keeping its shape, centered on a transparent background."""
    picture = picture.convert("RGBA")
    scale = min(w / picture.width, h / picture.height)
    size = (max(1, round(picture.width * scale)), max(1, round(picture.height * scale)))
    picture = picture.resize(size, Image.LANCZOS)
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(picture, ((w - size[0]) // 2, (h - size[1]) // 2), picture)
    return out


def quantize(img, colors):
    """Reduce an RGBA image to at most `colors` RGB5A3 colors; returns (palette, indices)."""
    # Keep fully transparent pixels in one palette entry so edges stay clean.
    rgb5a3 = [rgba_to_rgb5a3(*p) if p[3] >= 16 else 0 for p in img.getdata()]
    unique = sorted(set(rgb5a3))
    if len(unique) <= colors:
        lookup = {c: i for i, c in enumerate(unique)}
        return unique, [lookup[c] for c in rgb5a3]
    q = img.quantize(colors - 1, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    pal = q.getpalette("RGBA")[:(colors - 1) * 4]
    palette = [0] + [rgba_to_rgb5a3(*pal[i * 4:i * 4 + 4]) for i in range(colors - 1)]
    indices = [0 if a < 16 else q.getpixel((i % img.width, i // img.width)) + 1
               for i, (_, _, _, a) in enumerate(img.getdata())]
    return palette, indices


def rgba_to_rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def encode_cmpr_block(pixels):
    """Encode 16 RGBA pixels as one DXT1 block (big-endian, as GX stores it)."""
    opaque = [p for p in pixels if p[3] >= 128]
    if not opaque:
        return struct.pack(">HHI", 0, 0xFFFF, 0xFFFFFFFF)
    # Endpoints: the two opaque pixels furthest apart.
    best = (-1, opaque[0], opaque[0])
    for i, a in enumerate(opaque):
        for b in opaque[i + 1:]:
            d = sum((a[c] - b[c]) ** 2 for c in range(3))
            if d > best[0]:
                best = (d, a, b)
    c0, c1 = rgba_to_rgb565(*best[1][:3]), rgba_to_rgb565(*best[2][:3])
    transparent = len(opaque) < 16
    if transparent:
        # Three colors and transparency need c0 <= c1.
        c0, c1 = min(c0, c1), max(c0, c1)
    elif c0 < c1:
        c0, c1 = c1, c0
    elif c0 == c1:
        transparent = False
        c1 = c1 - 1 if c1 > 0 else c1
        if c0 == c1:
            c0 = c1 + 1
    a, b = rgb565_to_rgba(c0), rgb565_to_rgba(c1)
    if c0 > c1:
        pal = [a, b, tuple((2 * a[i] + b[i]) // 3 for i in range(3)), tuple((a[i] + 2 * b[i]) // 3 for i in range(3))]
    else:
        pal = [a, b, tuple((a[i] + b[i]) // 2 for i in range(3)), None]
    idx = 0
    for p, px in enumerate(pixels):
        if px[3] < 128 and c0 <= c1:
            v = 3
        else:
            v = min((i for i in range(4) if pal[i] is not None),
                    key=lambda i: sum((px[c] - pal[i][c]) ** 2 for c in range(3)))
        idx |= v << (30 - 2 * p)
    return struct.pack(">HHI", c0, c1, idx)


def encode_cmpr(data, start, img):
    w, h = img.size
    px = img.load()
    o = start
    for by in range(0, h, 8):
        for bx in range(0, w, 8):
            for sub in range(4):
                sx, sy = bx + (sub % 2) * 4, by + (sub // 2) * 4
                pixels = [px[sx + p % 4, sy + p // 4] if sx + p % 4 < w and sy + p // 4 < h else (0, 0, 0, 0)
                          for p in range(16)]
                data[o:o + 8] = encode_cmpr_block(pixels)
                o += 8


def encode_into(data, bmp, img):
    w, h, fmt = bmp.w, bmp.h, bmp.fmt
    if fmt == FMT_CMPR:
        encode_cmpr(data, bmp.data_ofs, img)
        return
    if fmt not in (FMT_C8, FMT_C4, FMT_RGB5A3, FMT_RGB5A3_DUPE, FMT_RGBA8):
        raise ValueError(f"replacing {FMT_NAMES[fmt]} bitmaps is not supported")
    bw, bh, bpp = BLOCKS[fmt]
    coords = list(block_order(w, h, bw, bh))
    px = img.load()
    pixel = lambda x, y: px[x, y] if x < w and y < h else (0, 0, 0, 0)
    o = bmp.data_ofs
    if fmt == FMT_RGBA8:
        for blk in range(0, len(coords), 16):
            for p in range(16):
                r, g, b, a = pixel(*coords[blk + p])
                data[o + p * 2:o + p * 2 + 2] = bytes((a, r))
                data[o + 32 + p * 2:o + 32 + p * 2 + 2] = bytes((g, b))
            o += 64
        return
    if fmt in (FMT_RGB5A3, FMT_RGB5A3_DUPE):
        for i, (x, y) in enumerate(coords):
            struct.pack_into(">H", data, o + i * 2, rgba_to_rgb5a3(*pixel(x, y)))
        return
    palette, indices = quantize(img, bmp.pal_num)
    for i, c in enumerate(palette):
        struct.pack_into(">H", data, bmp.pal_ofs + i * 2, c)
    for i in range(len(palette), bmp.pal_num):
        struct.pack_into(">H", data, bmp.pal_ofs + i * 2, 0)
    for i, (x, y) in enumerate(coords):
        v = indices[y * w + x] if x < w and y < h else 0
        if bpp == 8:
            data[o + i] = v
        elif i % 2 == 0:
            data[o + i // 2] = (data[o + i // 2] & 0x0F) | (v << 4)
        else:
            data[o + i // 2] = (data[o + i // 2] & 0xF0) | v


def replace(data, picture, index=None, mode="fit"):
    """Return a copy of an ANM file with its bitmaps (or just bitmap `index`) replaced by `picture`."""
    out = bytearray(data)
    targets = bitmaps(out) if index is None else [bitmaps(out)[index]]
    for bmp in targets:
        img = fit(picture, bmp.w, bmp.h) if mode == "fit" else picture.convert("RGBA").resize((bmp.w, bmp.h), Image.LANCZOS)
        encode_into(out, bmp, img)
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("list")
    p.add_argument("anm")
    p = sub.add_parser("export")
    p.add_argument("anm")
    p.add_argument("png")
    p.add_argument("--bmp", type=int, default=0)
    p = sub.add_parser("replace")
    p.add_argument("anm")
    p.add_argument("png")
    p.add_argument("out")
    p.add_argument("--bmp", type=int)
    p.add_argument("--stretch", action="store_true", help="fill the bitmap instead of keeping the picture's shape")
    args = parser.parse_args()
    with open(args.anm, "rb") as f:
        data = f.read()
    if args.command == "list":
        for i, bmp in enumerate(bitmaps(data)):
            print(i, bmp)
    elif args.command == "export":
        decode(data, bitmaps(data)[args.bmp]).save(args.png)
    else:
        out = replace(data, Image.open(args.png), args.bmp, "stretch" if args.stretch else "fit")
        with open(args.out, "wb") as f:
            f.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
