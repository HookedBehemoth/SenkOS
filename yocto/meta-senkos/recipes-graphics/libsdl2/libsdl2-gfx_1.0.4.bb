SUMMARY = "SDL2 graphics primitives, rotozoomer, framerate control and image filters"
DESCRIPTION = "Part of the ports' platform (docs/ports.md): binaries built against Debian's libSDL2_gfx-1.0.so.0."
HOMEPAGE = "https://www.ferzkopp.net/wordpress/2016/01/02/sdl_gfx-sdl2_gfx/"
SECTION = "libs"
LICENSE = "Zlib"
LIC_FILES_CHKSUM = "file://COPYING;md5=e04e1feb331457e9da15e0c88d0e8097"

DEPENDS = "virtual/libsdl2"

SRC_URI = "https://www.ferzkopp.net/Software/SDL2_gfx/SDL2_gfx-${PV}.tar.gz"
SRC_URI[sha256sum] = "63e0e01addedc9df2f85b93a248f06e8a04affa014a835c2ea34bfe34e576262"

S = "${UNPACKDIR}/SDL2_gfx-${PV}"

inherit autotools pkgconfig

# x86 assembly
EXTRA_OECONF = "--disable-mmx"
