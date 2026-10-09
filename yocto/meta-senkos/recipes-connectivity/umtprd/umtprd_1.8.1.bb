SUMMARY = "uMTP-Responder: MTP over a FunctionFS gadget function (started by `usb`, senkos-base)"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=d32239bcb673463ab874e80d47fae504"
SRC_URI = "https://github.com/viveris/uMTP-Responder/archive/refs/tags/umtprd-${PV}.tar.gz"
SRC_URI[sha256sum] = "6f57f61a1993059bcdd5f598d8c2a3888991497ef42ba3594947f00bd7746b1f"
# No release assets; the tag archive is pinned by the checksum above
ERROR_QA:remove = "src-uri-bad"
WARN_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/uMTP-Responder-umtprd-${PV}"

# Its Makefile strips (-s) unless DEBUG
EXTRA_OEMAKE = "CC='${CC}' CFLAGS='${CFLAGS} -I./inc -Wall' LDFLAGS='${LDFLAGS} -lpthread -lrt'"

do_install() {
    install -Dm0755 ${S}/umtprd ${D}${sbindir}/umtprd
}
