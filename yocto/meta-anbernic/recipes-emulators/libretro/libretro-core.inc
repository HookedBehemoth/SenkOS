# A libretro core from a plain libretro Makefile: one <core>_libretro.so in /usr/lib/libretro, run by
# corehost (an emulator's .desktop in anbernic-data). Recipes set LIBRETRO_REPO/LIBRETRO_REV and the checksum.
SUMMARY ?= "libretro core ${LIBRETRO_CORE}"
PV = "0+git${@d.getVar('LIBRETRO_REV')[:10]}"
SRC_URI = "https://github.com/libretro/${LIBRETRO_REPO}/archive/${LIBRETRO_REV}.tar.gz;downloadfilename=${LIBRETRO_REPO}-${LIBRETRO_REV}.tar.gz"
# Cores have no releases: the commit archive is pinned by SRC_URI[sha256sum]
ERROR_QA:remove = "src-uri-bad"
WARN_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/${LIBRETRO_REPO}-${LIBRETRO_REV}"
B = "${S}"

LIBRETRO_CORE ?= "${@d.getVar('BPN').removeprefix('libretro-').replace('-', '_')}"
LIBRETRO_MAKEFILE ?= "Makefile"
LIBRETRO_DIR ?= "${S}"
# The Makefiles hardcode gcc and guess the target from `uname -m` unless told
EXTRA_OEMAKE = "-C ${LIBRETRO_DIR} -f ${LIBRETRO_MAKEFILE} platform=unix \
    CC='${CC}' CXX='${CXX}' AR='${AR}' ${LIBRETRO_MAKE_ARGS}"
LIBRETRO_MAKE_ARGS ?= ""

do_compile() {
	oe_runmake
}

do_install() {
	install -Dm0755 ${LIBRETRO_DIR}/${LIBRETRO_CORE}_libretro.so ${D}${libdir}/libretro/${LIBRETRO_CORE}_libretro.so
}
FILES:${PN} = "${libdir}/libretro"

