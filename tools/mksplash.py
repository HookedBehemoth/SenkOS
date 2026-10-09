#!/usr/bin/env python3
"""PNG -> boot splash, drawn centred. The top-left pixel is the background.

  tools/mksplash.py splash.png yocto/meta-senkos/recipes-bsp/u-boot/files/splash.lz4
"""
import struct
import subprocess
import sys

from PIL import Image, ImageChops

src, dst = sys.argv[1:3]
img = Image.open(src).convert("RGB")
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
