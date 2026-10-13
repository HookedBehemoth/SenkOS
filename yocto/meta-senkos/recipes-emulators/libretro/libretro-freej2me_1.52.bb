SUMMARY = "J2ME (FreeJ2ME-Plus) libretro core, its Java app run by the image's JRE"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=768ee3e03623da01ee1f18abc086cc39"
SRC_URI = "https://github.com/TASEmulators/freej2me-plus/archive/refs/tags/${PV}.tar.gz;downloadfilename=freej2me-plus-${PV}.tar.gz;name=src \
    https://github.com/TASEmulators/freej2me-plus/releases/download/${PV}/freej2me-v${PV}.zip;name=jar;subdir=jar \
    file://0005-freej2me-senkos.patch"
SRC_URI[src.sha256sum] = "509c3590827c8556cc0f8ff42c22693dd40e99e01e15f8f5c0e2e00bdbd8c130"
SRC_URI[jar.sha256sum] = "27eb4f51d4201b6f7682ed0259d08d86e0410d41f572412812fe1ae928df9688"
# The tag archive is pinned by the checksum above; the jar is the release's (Java 1.6 bytecode)
ERROR_QA:remove = "src-uri-bad"
WARN_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/freej2me-plus-${PV}"
B = "${S}"

EXTRA_OEMAKE = "-C ${S}/src/libretro platform=unix CC='${CC}' CXX='${CXX}'"
do_compile() {
	oe_runmake
}

do_install() {
	install -Dm0755 ${S}/src/libretro/freej2me_libretro.so ${D}${libdir}/libretro/freej2me_libretro.so
	install -Dm0644 ${UNPACKDIR}/jar/freej2me-lr.jar ${D}${datadir}/freej2me/freej2me-lr.jar
}
FILES:${PN} = "${libdir}/libretro ${datadir}/freej2me"
RDEPENDS:${PN} = "java-runtime"
