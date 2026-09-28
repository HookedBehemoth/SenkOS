SUMMARY = "SuperTuxKart kart racer, SDL2 + GLES renderer"
LICENSE = "GPL-3.0-or-later"
LIC_FILES_CHKSUM = "file://COPYING;md5=bcfdeb69518cfe348a07845ebba5c295"
# Not in the image: the data is ~840 MB, more than a rootfs slot. Deployed as a directory
# (binary + data/, found next to the exe), copied to /data/supertuxkart.
SRC_URI = "https://github.com/supertuxkart/stk-code/releases/download/${PV}/SuperTuxKart-${PV}-src.tar.gz"
SRC_URI[sha256sum] = "33cf8841e4ff4082d80b9248014295bbbea61d14683e86dff100e3ab8f7b27cb"
S = "${UNPACKDIR}/SuperTuxKart-${PV}-src"

inherit cmake pkgconfig deploy
# The shared ones are in the image; bullet/irrlicht/angelscript/enet/mcpp/squish are bundled
DEPENDS = "libsdl2 openal-soft libogg libvorbis freetype harfbuzz curl openssl jpeg libpng zlib"

# GLES renderer only: the Vulkan one needs shaderc (glslang+spirv-tools) on target, and PanVK on G31 is shaky
EXTRA_OECMAKE = "-DUSE_GLES2=ON -DNO_SHADERC=ON -DCHECK_ASSETS=OFF -DBUILD_RECORDER=OFF \
    -DUSE_WIIUSE=OFF -DUSE_SQLITE3=OFF -DUSE_SYSTEM_ANGELSCRIPT=OFF -DCMAKE_BUILD_TYPE=Release"

# The package would only be the binary: the data is deployed from ${S}, not packaged
do_install:append() {
	rm -rf ${D}${datadir}
}

do_deploy() {
	install -d ${DEPLOYDIR}/supertuxkart
	${STRIP} -o ${DEPLOYDIR}/supertuxkart/supertuxkart ${B}/bin/supertuxkart
	cp -r --no-preserve=ownership ${S}/data ${DEPLOYDIR}/supertuxkart/
}
addtask deploy after do_compile before do_build
