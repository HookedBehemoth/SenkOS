LICENSE = "Snes9x"
LIC_FILES_CHKSUM = "file://LICENSE;md5=6691f801143624b98c1f074d27d27ba7"
LIBRETRO_REPO = "snes9x"
LIBRETRO_REV = "fae2fea08f74180759ef540ee94259213f503480"
SRC_URI[sha256sum] = "0d4b0c4181d66668ec0040eb17f90a559f7b7fa6cbd9198593c3bdbe79acd029"
require libretro-core.inc
SUMMARY = "SNES (Snes9x) libretro core"
# Non-commercial license
NO_GENERIC_LICENSE[Snes9x] = "LICENSE"
LIBRETRO_DIR = "${S}/libretro"
