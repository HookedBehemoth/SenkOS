SUMMARY = "PSP (PPSSPP) libretro core, GLES + Vulkan"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://LICENSE.TXT;md5=e336f8162cddec7981e240f46825d8a2"
# The release tarball carries the submodules (glslang, SPIRV-Cross, ffmpeg, ...)
SRC_URI = "https://github.com/hrydgard/ppsspp/releases/download/v${PV}/ppsspp-${PV}.tar.xz \
    file://0001-libretro-resize-vulkan-output.patch \
    file://0002-mediaengine-handle-empty-ffmpeg-reads.patch"
SRC_URI[sha256sum] = "4de21db3105d9d81a465a8a7e78c68ee3c0e0bf6597d1c1d530f7555f3ad8b31"
S = "${UNPACKDIR}/ppsspp-${PV}"

inherit cmake pkgconfig deploy
# wayland: headers for PPSSPP's Vulkan surface code, which in libretro takes the frontend's surface instead
DEPENDS = "virtual/libgles2 virtual/egl zlib libpng ffmpeg wayland vulkan-headers"

# FFmpeg (PSP movies/audio): the system libav*, not the prebuilt static one in the tarball
EXTRA_OECMAKE = "-DLIBRETRO=ON -DUSING_GLES2=ON -DUSING_EGL=OFF -DUSING_X11_VULKAN=OFF -DUSE_WAYLAND_WSI=ON \
    -DUSE_DISCORD=OFF -DUSE_MINIUPNPC=OFF -DHEADLESS=OFF -DUNITTEST=OFF -DUSE_CCACHE=OFF \
    -DUSE_SYSTEM_FFMPEG=ON -DCMAKE_BUILD_TYPE=Release"

do_install() {
	install -Dm0755 ${B}/lib/ppsspp_libretro.so ${D}${libdir}/libretro/ppsspp_libretro.so
	# The core wants these in <system dir>/PPSSPP (fonts, flash0, shaders, lang)
	install -d ${D}${datadir}/ppsspp
	cp -r --no-preserve=ownership ${S}/assets/. ${D}${datadir}/ppsspp/
	rm -rf ${D}${datadir}/ppsspp/debugger
}
FILES:${PN} = "${libdir}/libretro ${datadir}/ppsspp"

# retroarch-data/ mirrors /data: the core, and its assets in RetroArch's system dir (/data/bios)
do_deploy() {
	install -d ${DEPLOYDIR}/retroarch-data/retroarch/cores ${DEPLOYDIR}/retroarch-data/bios
	${STRIP} -o ${DEPLOYDIR}/retroarch-data/retroarch/cores/ppsspp_libretro.so ${D}${libdir}/libretro/ppsspp_libretro.so
	cp -r --no-preserve=ownership ${D}${datadir}/ppsspp ${DEPLOYDIR}/retroarch-data/bios/PPSSPP
}
addtask deploy after do_install before do_build
