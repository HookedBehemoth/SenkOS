SUMMARY = "Suspend on lid close / power key"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://sleepd.c file://suspend"
S = "${UNPACKDIR}"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -Wall -o sleepd ${S}/sleepd.c
}

do_install() {
    install -Dm0755 sleepd ${D}${sbindir}/sleepd
    install -Dm0755 ${S}/suspend ${D}${sbindir}/suspend
}
