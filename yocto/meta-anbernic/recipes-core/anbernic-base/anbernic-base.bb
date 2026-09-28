SUMMARY = "RG35XX-SP glue: USB gadget (network + serial console), A/B slot tool, watchdog, Weston startup"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://anbernic.init file://ab file://udhcpd.conf file://panvk.sh file://wayland.sh file://weston.init file://weston.ini"
S = "${UNPACKDIR}"

inherit allarch

do_install() {
    install -Dm0755 ${S}/anbernic.init ${D}${sysconfdir}/init.d/anbernic
    install -Dm0755 ${S}/ab ${D}${sbindir}/ab
    install -Dm0644 ${S}/udhcpd.conf ${D}${sysconfdir}/udhcpd.conf
    install -Dm0644 ${S}/panvk.sh ${D}${sysconfdir}/profile.d/panvk.sh
    install -Dm0644 ${S}/wayland.sh ${D}${sysconfdir}/profile.d/wayland.sh
    install -Dm0755 ${S}/weston.init ${D}${sysconfdir}/init.d/weston
    install -Dm0644 ${S}/weston.ini ${D}${sysconfdir}/xdg/weston/weston.ini
    install -d ${D}${sysconfdir}/rcS.d ${D}${sysconfdir}/rc5.d ${D}/uboot
    # after populate-volatile (S37); trial timer last, once everything else came up
    ln -s ../init.d/anbernic ${D}${sysconfdir}/rcS.d/S40anbernic
    printf '#!/bin/sh\n[ "$1" = start ] && exec %s/ab boot\n' ${sbindir} > ${D}${sysconfdir}/init.d/ab-boot
    chmod 0755 ${D}${sysconfdir}/init.d/ab-boot
    ln -s ../init.d/ab-boot ${D}${sysconfdir}/rc5.d/S99ab-boot
    ln -s ../init.d/weston ${D}${sysconfdir}/rc5.d/S20weston
}

FILES:${PN} += "/uboot"
RDEPENDS:${PN} = "busybox dropbear weston seatd"
