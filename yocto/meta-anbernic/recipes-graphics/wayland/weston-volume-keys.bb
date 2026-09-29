SUMMARY = "Weston module: volume keys set the codec's Line Out (the launcher shows the level)"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
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
