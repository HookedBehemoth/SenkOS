SUMMARY = "mGBA Game Boy Advance emulator, SDL2 frontend"
LICENSE = "MPL-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=815ca599c9df247a0c7f619bab123dad"
# Not in the image yet: deployed as a directory, copied to /data/mgba. Run it from there:
# mGBA looks for portable.ini (config in its own directory) in the cwd.
SRC_URI = "https://github.com/mgba-emu/mgba/archive/refs/tags/${PV}.tar.gz;downloadfilename=mgba-${PV}.tar.gz \
           file://config.ini"
SRC_URI[sha256sum] = "91d6fbd32abcbdf030d58d3f562de25ebbc9d56040d513ff8e5c19bee9dacf14"
# No source tarball among the release assets; the tag archive is pinned by the checksum above
ERROR_QA:remove = "src-uri-bad"
WARN_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/mgba-${PV}"

inherit cmake pkgconfig deploy
DEPENDS = "libsdl2 zlib libpng"

# One static binary, SDL_Renderer output (no GL: GBA is 240x160), bundled minizip/lzma for zipped ROMs
EXTRA_OECMAKE = "-DBUILD_QT=OFF -DBUILD_SDL=ON -DBUILD_STATIC=ON -DBUILD_SHARED=OFF \
    -DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF -DUSE_EPOXY=OFF \
    -DUSE_FFMPEG=OFF -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_SQLITE3=OFF -DUSE_LUA=OFF \
    -DENABLE_SCRIPTING=OFF -DUSE_ELF=OFF -DUSE_EDITLINE=OFF -DUSE_DEBUGGERS=OFF -DUSE_GDB_STUB=OFF \
    -DUSE_DISCORD_RPC=OFF -DSKIP_GIT=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5"

do_deploy() {
	install -d ${DEPLOYDIR}/mgba
	install -m 0755 ${B}/sdl/mgba ${DEPLOYDIR}/mgba/mgba
	install -m 0644 ${UNPACKDIR}/config.ini ${DEPLOYDIR}/mgba/config.ini
	# portable.ini in the cwd: mGBA keeps config, saves and states in its own directory
	touch ${DEPLOYDIR}/mgba/portable.ini
}
addtask deploy after do_compile before do_build
FILES:${PN} += "${datadir}/icons"
