LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=b234ee4d69f5fce4486a80fdaf4a4263"
LIBRETRO_REPO = "gpsp"
LIBRETRO_REV = "5819380c2ffb0900219d700a382ee68c464ebb99"
SRC_URI[sha256sum] = "1dfc919966cd58ce1d8f2580041d9bffe2e313d9442260c713ed0286a048a733"
require libretro-core.inc
SUMMARY = "Game Boy Advance (gpSP, ARM64 dynarec) libretro core"
# platform=unix picks the dynarec from the build host's uname; the Makefile resets LDFLAGS
LIBRETRO_MAKE_ARGS = "HAVE_DYNAREC=1 CPU_ARCH=arm64 LDFLAGS='${LDFLAGS} -Wl,--no-undefined'"
