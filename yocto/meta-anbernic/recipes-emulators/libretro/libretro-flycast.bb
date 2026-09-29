SUMMARY = "Dreamcast / NAOMI (Flycast) libretro core"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"
# libretro's flycast: dependencies vendored (upstream flyinghead/flycast needs ~20 submodules)
LIBRETRO_REPO = "flycast"
LIBRETRO_REV = "45bd2f4e59708a7c16a5bb1cb90a94d1b39e330d"
SRC_URI[sha256sum] = "5ebf68c4548e5ba0521478f5172abd7f2ca5a3e15060cb78da011e3b03f3fb10"
require libretro-core.inc
DEPENDS = "virtual/libgles2 virtual/egl zlib"
# Generic arm64: GLES3 + arm64 dynarec; the Vulkan renderer too (loader comes from RetroArch)
# The Makefile resets CFLAGS and LDFLAGS; vendored libzip calls close() without <unistd.h> (an error since GCC 14).
# The dynarec's .S needs the preprocessor: assemble through gcc, not as.
LIBRETRO_MAKE_ARGS = "platform=arm64 HAVE_VULKAN=1 CC='${CC} -Wno-error=implicit-function-declaration' CC_AS='${CC}' LDFLAGS='${LDFLAGS} -Wl,--no-undefined -fopenmp'"
