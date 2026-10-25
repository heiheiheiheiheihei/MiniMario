#!/usr/bin/env python3
"""Parse the RLE dumps from /tmp/display_test and rebuild PNG screenshots.

Usage: rle_to_png.py <serial_capture.txt> <outdir>

Besides the raw dumps it reconstructs, for each rotation, the "logical view" of
the physical panel framebuffer (what the user should see when holding the board
in that orientation) and compares it against the canvas that was drawn.  A
mismatch means the transpose/rotation mapping is wrong.
"""
import sys
import os

# UI palette from components/display/include/display.h (RGB565 values)
UI_PAL = [
    0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F,
    0x8410, 0x4208, 0xFD20, 0x87F0, 0x5D7F, 0xC618, 0xA145, 0x0010,
]

NAMES = ["BLACK", "WHITE", "RED", "GREEN", "BLUE", "YELLOW", "CYAN", "MAGENTA",
         "GRAY", "DGRAY", "ORANGE", "LGREEN", "LBLUE", "LGRAY", "BROWN", "NAVY"]


def rgb565_to_rgb(c):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


def parse(path):
    """Return {'canvas': {rot:(w,h,[idx...])}, 'panel': {rot:(w,h,[rgb565...])}}."""
    out = {"canvas": {}, "panel": {}}
    cur = None
    with open(path, "r", errors="replace") as f:
        for raw in f:
            line = raw.strip()
            if line.startswith("BEGIN_CANVAS"):
                parts = dict(p.split("=") for p in line.split()[1:])
                cur = {"kind": "canvas", "rot": int(parts["rot"]),
                       "w": int(parts["W"]), "h": int(parts["H"]), "runs": []}
            elif line.startswith("BEGIN_PANEL"):
                parts = dict(p.split("=") for p in line.split()[1:])
                cur = {"kind": "panel", "rot": int(parts["rot"]),
                       "w": int(parts["W"]), "h": int(parts["H"]), "runs": []}
            elif line.startswith("END_CANVAS"):
                if cur and cur["kind"] == "canvas":
                    out["canvas"][cur["rot"]] = (cur["w"], cur["h"], decode(cur["runs"], 5))
                cur = None
            elif line.startswith("END_PANEL"):
                if cur and cur["kind"] == "panel":
                    out["panel"][cur["rot"]] = (cur["w"], cur["h"], decode(cur["runs"], 8))
                cur = None
            elif cur is not None and line and all(ch in "0123456789abcdef" for ch in line):
                cur["runs"].append(line)
    return out


def decode(chunks, token_len):
    stream = "".join(chunks)
    values = []
    for i in range(0, len(stream) - token_len + 1, token_len):
        tok = stream[i:i + token_len]
        if token_len == 5:
            color, count = int(tok[0], 16), int(tok[1:], 16)
        else:
            color, count = int(tok[:4], 16), int(tok[4:], 16)
        values.extend([color] * count)
    return values


def to_image(w, h, values, palette=None):
    from PIL import Image
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            v = values[y * w + x]
            px[x, y] = palette[v] if palette else rgb565_to_rgb(v)
    return img


def logical_view(rot, img):
    """Physical portrait image -> what the user sees holding the board in `rot`.

    For landscape rotations the driver maps physical (u=column, v=row) to
    logical (x,y) as:
      rot 1 (board clockwise):        u = y,     v = 319 - x
      rot 3 (board counter-clockwise): u = 171-y, v = x
    Portrait rotations are direct mapped:
      rot 0: u = x, v = y          rot 2: u = 171-x, v = 319-y
    """
    from PIL import Image
    w, h = img.size
    if rot == 1:
        out = Image.new("RGB", (320, 172))
        for y in range(172):
            for x in range(320):
                out.putpixel((x, y), img.getpixel((y, 319 - x)))
        return out
    if rot == 3:
        out = Image.new("RGB", (320, 172))
        for y in range(172):
            for x in range(320):
                out.putpixel((x, y), img.getpixel((171 - y, x)))
        return out
    if rot == 0:
        return img.copy()
    if rot == 2:
        out = Image.new("RGB", (w, h))
        for y in range(h):
            for x in range(w):
                out.putpixel((x, y), img.getpixel((w - 1 - x, h - 1 - y)))
        return out
    raise ValueError(rot)


def main():
    from PIL import Image
    src, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    data = parse(src)

    canvas_imgs = {}
    for rot, (w, h, vals) in sorted(data["canvas"].items()):
        if w == 0:
            continue
        pal = [rgb565_to_rgb(c) for c in UI_PAL]
        img = to_image(w, h, vals, pal)
        canvas_imgs[rot] = img
        img = img.resize((w * 3, h * 3), Image.NEAREST)
        img.save(os.path.join(outdir, f"canvas_rot{rot}.png"))
        print(f"canvas rot={rot}: {w}x{h} -> canvas_rot{rot}.png")

    for rot, (w, h, vals) in sorted(data["panel"].items()):
        if w == 0:
            continue
        img = to_image(w, h, vals)
        img.save(os.path.join(outdir, f"panel_phys_rot{rot}.png"))
        view = logical_view(rot, img)
        view.resize((view.size[0] * 3, view.size[1] * 3), Image.NEAREST).save(
            os.path.join(outdir, f"view_rot{rot}.png"))

        # automated self-check: panel through rotation must equal the drawn canvas
        ref = canvas_imgs.get(rot)
        if ref is not None and ref.size == view.size:
            diff = 0
            rp, vp = ref.load(), view.load()
            for y in range(ref.size[1]):
                for x in range(ref.size[0]):
                    if rp[x, y] != vp[x, y]:
                        diff += 1
            status = "OK" if diff == 0 else f"MISMATCH {diff}px"
            print(f"panel  rot={rot}: {w}x{h} -> panel_phys_rot{rot}.png, "
                  f"view_rot{rot}.png  self-check: {status}")
        else:
            print(f"panel  rot={rot}: {w}x{h} -> panel_phys_rot{rot}.png, view_rot{rot}.png")


if __name__ == "__main__":
    main()
