#!/usr/bin/env python3
"""Convert a binary PPM (P6, as written by QEMU's screendump) to PNG.

    python3 ppm2png.py [--require-content] <in.ppm> <out.png>

--require-content fails (exit 1) when the picture is (almost) entirely black,
i.e. nothing was drawn on the framebuffer.

Standard library only (zlib + struct).
"""
import struct
import sys
import zlib


def read_token(data, pos):
    # Skip whitespace and '#' comments, then read one header token.
    while True:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        break
    start = pos
    while pos < len(data) and not data[pos:pos + 1].isspace():
        pos += 1
    return data[start:pos], pos


def ppm_to_png(ppm):
    magic, pos = read_token(ppm, 0)
    if magic != b"P6":
        raise ValueError("not a binary PPM (P6) file")
    width, pos = read_token(ppm, pos)
    height, pos = read_token(ppm, pos)
    maxval, pos = read_token(ppm, pos)
    width, height, maxval = int(width), int(height), int(maxval)
    if maxval != 255:
        raise ValueError("only 8-bit PPM is supported")
    pos += 1  # the single whitespace byte after maxval
    stride = width * 3
    pixels = ppm[pos:pos + stride * height]
    if len(pixels) != stride * height:
        raise ValueError("truncated PPM pixel data")
    raw = b"".join(b"\x00" + pixels[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    lit = sum(1 for i in range(0, len(pixels), 3) if pixels[i] | pixels[i + 1] | pixels[i + 2])
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    return png, width, height, lit


def main():
    args = sys.argv[1:]
    require = "--require-content" in args
    args = [a for a in args if a != "--require-content"]
    if len(args) != 2:
        sys.stderr.write("usage: ppm2png.py [--require-content] <in.ppm> <out.png>\n")
        return 2
    with open(args[0], "rb") as f:
        png, width, height, lit = ppm_to_png(f.read())
    with open(args[1], "wb") as f:
        f.write(png)
    print("%s: %dx%d, %d non-black pixels" % (args[1], width, height, lit))
    if require and lit < width * height // 1000:
        sys.stderr.write("ppm2png: %s is blank (nothing drawn on the framebuffer)\n" % args[1])
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
