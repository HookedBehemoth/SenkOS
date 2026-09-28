SUMMARY = "Weston module: volume keys set Line Out and show a level bar over every client"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
SRC_URI = "file://volume-osd.c"
S = "${UNPACKDIR}"

inherit pkgconfig
DEPENDS = "weston alsa-lib"

do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} -shared -fPIC -o volume-osd.so ${S}/volume-osd.c \
		$(pkg-config --cflags --libs libweston-15 alsa)
}
do_install() {
	install -Dm 0755 volume-osd.so ${D}${libdir}/weston/volume-osd.so
}
FILES:${PN} = "${libdir}/weston/volume-osd.so"
