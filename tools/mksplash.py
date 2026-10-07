#!/usr/bin/env python3
"""PNG -> splash.bin for the kernel's boot splash (drm_splash.c in kernel patch 0052).

The top-left pixel is the background; only the box around everything else is stored,
centred on that colour at boot. Keep the PNG at the panel's size (640x480) so the
position matches what you drew.

  tools/mksplash.py splash.png yocto/meta-anbernic/recipes-kernel/linux/files/firmware/splash.bin
"""
import struct
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
open(dst, "wb").write(out)
print(f"{dst}: {crop.width}x{crop.height} on #{out[6]:02x}{out[5]:02x}{out[4]:02x}, {len(out)} bytes")
