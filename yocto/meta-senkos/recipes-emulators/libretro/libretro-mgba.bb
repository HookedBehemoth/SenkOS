SUMMARY = "Game Boy Advance (mGBA) libretro core"
LICENSE = "MPL-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=815ca599c9df247a0c7f619bab123dad"
# libretro's fork, as RetroArch has it: its core has color correction, interframe blending and
# frameskip options. No releases: the commit archive, pinned by its checksum.
LIBRETRO_REV = "7a12d6d4b9acb14c0ae62c9166b6a2f3d08007f6"
PV = "0+git${@d.getVar('LIBRETRO_REV')[:10]}"
SRC_URI = "https://github.com/libretro/mgba/archive/${LIBRETRO_REV}.tar.gz;downloadfilename=mgba-libretro-${LIBRETRO_REV}.tar.gz \
    file://0004-mgba-skip-unshown-frames.patch"
SRC_URI[sha256sum] = "5cbf639e527fb586bf33e14d59037eab35f78f45154e0b79c784fff474c3bc37"
ERROR_QA:remove = "src-uri-bad"
WARN_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/mgba-${LIBRETRO_REV}"

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
