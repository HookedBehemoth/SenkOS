SUMMARY = "Weston module: volume keys set the codec's Line Out (the launcher shows the level)"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"
SRC_URI = "file://volume-keys.c"
S = "${UNPACKDIR}"

inherit pkgconfig
DEPENDS = "weston alsa-lib"

do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} -shared -fPIC -o volume-keys.so ${S}/volume-keys.c \
		$(pkg-config --cflags --libs libweston-15 alsa)
}
do_install() {
	install -Dm 0755 volume-keys.so ${D}${libdir}/weston/volume-keys.so
}
FILES:${PN} = "${libdir}/weston/volume-keys.so"
