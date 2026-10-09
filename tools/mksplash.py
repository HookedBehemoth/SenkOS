#!/usr/bin/env python3
"""PNG -> boot splash, drawn centred. The top-left pixel is the background; a
transparent PNG goes on the colour given (the UI's bg, ui.c), so one art makes both:

  f=yocto/meta-senkos/recipes-bsp/u-boot/files
  tools/mksplash.py $f/splash-art.png $f/splash.lz4 141418
  tools/mksplash.py $f/splash-art.png $f/splash-light.lz4 f2f2f5
"""
import struct
import subprocess
import sys

from PIL import Image, ImageChops

src, dst = sys.argv[1:3]
img = Image.open(src).convert("RGBA")
if len(sys.argv) > 3:
    under = Image.new("RGBA", (img.width + 2, img.height + 2), "#" + sys.argv[3])
    under.alpha_composite(img, (1, 1))
    img = under
img = img.convert("RGB")
bg = img.getpixel((0, 0))
box = ImageChops.difference(img, Image.new("RGB", img.size, bg)).getbbox() or (0, 0, 1, 1)
# symmetric around the centre, so centring the crop puts it back where it was drawn
l, t, r, b = box
w, h = img.size
l, r = min(l, w - r), max(r, w - l)
t, b = min(t, h - b), max(b, h - t)
crop = img.crop((l, t, r, b))

out = struct.pack("<HHI", crop.width, crop.height, bg[0] << 16 | bg[1] << 8 | bg[2])
out += crop.tobytes("raw", "BGRX")  # XRGB8888 little endian
subprocess.run(["lz4", "-12", "-f", "-q", "-", dst], input=out, check=True)
print(f"{dst}: {crop.width}x{crop.height} on #{out[6]:02x}{out[5]:02x}{out[4]:02x}, {len(out)} bytes unpacked")
