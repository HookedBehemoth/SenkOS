SUMMARY = "Suspend on lid close / power key"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"

SRC_URI = "file://sleepd.c file://suspend"
S = "${UNPACKDIR}"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -Wall -o sleepd ${S}/sleepd.c
}

do_install() {
    install -Dm0755 sleepd ${D}${sbindir}/sleepd
    install -Dm0755 ${S}/suspend ${D}${sbindir}/suspend
}
