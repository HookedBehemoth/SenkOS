SUMMARY = "Root's side of the UI: suspend on lid / power key, the launcher's requests"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"

SRC_URI = "file://sysd.c file://suspend"
S = "${UNPACKDIR}"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -Wall -o sysd ${S}/sysd.c
}

do_install() {
    install -Dm0755 sysd ${D}${sbindir}/sysd
    install -Dm0755 ${S}/suspend ${D}${sbindir}/suspend
}
