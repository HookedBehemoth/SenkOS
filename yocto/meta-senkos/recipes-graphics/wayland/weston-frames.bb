SUMMARY = "Weston module: counts the frames shown, for the launcher's power calibration"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"
SRC_URI = "file://frames.c"
S = "${UNPACKDIR}"

inherit pkgconfig
DEPENDS = "weston"

do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} -shared -fPIC -o frames.so ${S}/frames.c $(pkg-config --cflags --libs libweston-15)
}
do_install() {
	install -Dm 0755 frames.so ${D}${libdir}/weston/frames.so
}
FILES:${PN} = "${libdir}/weston/frames.so"
