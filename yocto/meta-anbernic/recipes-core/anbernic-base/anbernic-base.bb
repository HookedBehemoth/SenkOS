SUMMARY = "RG35XX-SP glue: USB gadget (network + serial console), A/B slots + /data, watchdog, Weston + launcher startup"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://anbernic.init file://ab file://udhcpd.conf file://panvk.sh file://wayland.sh file://weston.init file://weston.ini file://sdcard.rules file://usb-gadget-check file://stop-all file://usb-gadget.rules file://data-init.init file://zz-data.sh"
S = "${UNPACKDIR}"

inherit allarch

do_install() {
    install -Dm0755 ${S}/anbernic.init ${D}${sysconfdir}/init.d/anbernic
    install -Dm0755 ${S}/ab ${D}${sbindir}/ab
    install -Dm0755 ${S}/usb-gadget-check ${D}${sbindir}/usb-gadget-check
    install -Dm0755 ${S}/stop-all ${D}${sbindir}/stop-all
    install -Dm0755 ${S}/data-init.init ${D}${sysconfdir}/init.d/data-init
    install -Dm0644 ${S}/udhcpd.conf ${D}${sysconfdir}/udhcpd.conf
    install -Dm0644 ${S}/panvk.sh ${D}${sysconfdir}/profile.d/panvk.sh
    install -Dm0644 ${S}/wayland.sh ${D}${sysconfdir}/profile.d/wayland.sh
    install -Dm0644 ${S}/zz-data.sh ${D}${sysconfdir}/profile.d/zz-data.sh
    install -Dm0755 ${S}/weston.init ${D}${sysconfdir}/init.d/weston
    install -Dm0644 ${S}/weston.ini ${D}${sysconfdir}/xdg/weston/weston.ini
    install -Dm0644 ${S}/sdcard.rules ${D}${sysconfdir}/udev/rules.d/90-sdcard.rules
    install -Dm0644 ${S}/usb-gadget.rules ${D}${sysconfdir}/udev/rules.d/90-usb-gadget.rules
    install -d ${D}${sysconfdir}/rcS.d ${D}${sysconfdir}/rc5.d ${D}/uboot ${D}/data
    # The UI first (mounts /data, runs data-init, starts seatd itself); the rest after
    # populate-volatile (S37); trial timer last, once everything else came up
    ln -s ../init.d/weston ${D}${sysconfdir}/rcS.d/S01weston
    ln -s ../init.d/anbernic ${D}${sysconfdir}/rcS.d/S40anbernic
    printf '#!/bin/sh\n[ "$1" = start ] && exec %s/ab boot\n' ${sbindir} > ${D}${sysconfdir}/init.d/ab-boot
    chmod 0755 ${D}${sysconfdir}/init.d/ab-boot
    ln -s ../init.d/ab-boot ${D}${sysconfdir}/rc5.d/S99ab-boot
}

FILES:${PN} += "/uboot /data"
RDEPENDS:${PN} = "busybox busybox-syslog dropbear weston seatd launcher util-linux-sfdisk e2fsprogs-mke2fs alsa-utils-amixer weston-volume-keys"
