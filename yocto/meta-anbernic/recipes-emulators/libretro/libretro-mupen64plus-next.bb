LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"
LIBRETRO_REPO = "mupen64plus-libretro-nx"
LIBRETRO_REV = "6752836de8b224febfd5708444755b77712ac939"
SRC_URI[sha256sum] = "78509e9787729aa0051915e646200ce21ab2d7e809318c9b1dfdaabcab2f067d"
require libretro-core.inc
SUMMARY = "Nintendo 64 (Mupen64Plus-Next) libretro core"
DEPENDS = "virtual/libgles2 virtual/egl zlib libpng"
# GLideN64 on GLES3 by default; paraLLEl-RDP/RSP (Vulkan compute) selectable in the core options
LIBRETRO_MAKE_ARGS = "ARCH=aarch64 WITH_DYNAREC=aarch64 FORCE_GLES3=1 HAVE_PARALLEL_RDP=1 HAVE_PARALLEL_RSP=1 HAVE_THR_AL=1"
