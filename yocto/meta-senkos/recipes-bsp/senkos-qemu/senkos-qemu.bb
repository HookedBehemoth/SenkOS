SUMMARY = "qemu glue (qemu.yml, tools/qemu): the pad from the keyboard, the handheld's ssh address on qemu's network"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"

SRC_URI = "file://qemu.init"
S = "${UNPACKDIR}"
COMPATIBLE_MACHINE = "qemuall"

do_install() {
    install -Dm0755 ${S}/qemu.init ${D}${sysconfdir}/init.d/qemu
    # Before weston (S01weston): the launcher opens the pad when it starts
    install -d ${D}${sysconfdir}/rcS.d
    ln -s ../init.d/qemu ${D}${sysconfdir}/rcS.d/S01qemu
}

RDEPENDS:${PN} = "qemupad"
