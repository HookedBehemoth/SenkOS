SUMMARY = "Game Boy Advance (mGBA) libretro core"
LICENSE = "MPL-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=815ca599c9df247a0c7f619bab123dad"
SRC_URI = "https://github.com/mgba-emu/mgba/archive/refs/tags/${PV}.tar.gz;downloadfilename=mgba-${PV}.tar.gz"
SRC_URI[sha256sum] = "91d6fbd32abcbdf030d58d3f562de25ebbc9d56040d513ff8e5c19bee9dacf14"
# No source tarball among the release assets; the tag archive is pinned by the checksum above
ERROR_QA:remove = "src-uri-bad"
WARN_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/mgba-${PV}"

inherit cmake pkgconfig
DEPENDS = "zlib libpng"

# The core, with its bundled minizip/lzma for zipped ROMs
EXTRA_OECMAKE = "-DBUILD_QT=OFF -DBUILD_SDL=OFF -DBUILD_STATIC=OFF -DBUILD_SHARED=OFF \
    -DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF -DUSE_EPOXY=OFF \
    -DUSE_FFMPEG=OFF -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_SQLITE3=OFF -DUSE_LUA=OFF \
    -DENABLE_SCRIPTING=OFF -DUSE_ELF=OFF -DUSE_EDITLINE=OFF -DUSE_DEBUGGERS=OFF -DUSE_GDB_STUB=OFF \
    -DUSE_DISCORD_RPC=OFF -DSKIP_GIT=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_LIBRETRO=ON -DLIBRETRO_LIBDIR=${libdir}/libretro"

OECMAKE_TARGET_COMPILE = "mgba_libretro"
do_install() {
	install -Dm0755 ${B}/mgba_libretro.so ${D}${libdir}/libretro/mgba_libretro.so
}
FILES:${PN} = "${libdir}/libretro"
