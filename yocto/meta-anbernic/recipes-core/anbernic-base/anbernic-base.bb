SUMMARY = "RG35XX-SP glue: USB gadget (network + serial console), A/B slots + /data, watchdog, Weston + launcher startup"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"

SRC_URI = "file://anbernic.init file://ab file://udhcpd.conf file://panvk.sh file://wayland.sh file://weston.init file://weston.ini file://sdcard.rules file://usb-gadget-check file://stop-all file://ums file://usb file://overlay file://usb-gadget.rules file://data-init.init file://zz-data.sh file://uinput.rules file://pipewire.conf"
S = "${UNPACKDIR}"

inherit allarch useradd

# Weston, the launcher and the apps it starts run as player: uid 1000, as files tar'ed in from a
# host usually are. HOME is /data. seat: seatd's socket (seatd's recipe makes the group too).
USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = "-r seat"
USERADD_PARAM:${PN} = "-u 1000 -U -d /data -M -s /bin/sh -G video,input,audio,seat player"

do_install() {
    install -Dm0755 ${S}/anbernic.init ${D}${sysconfdir}/init.d/anbernic
    install -Dm0755 ${S}/ab ${D}${sbindir}/ab
    install -Dm0755 ${S}/usb-gadget-check ${D}${sbindir}/usb-gadget-check
    install -Dm0755 ${S}/stop-all ${D}${sbindir}/stop-all
    install -Dm0755 ${S}/ums ${D}${sbindir}/ums
    install -Dm0755 ${S}/usb ${D}${sbindir}/usb
    install -Dm0755 ${S}/overlay ${D}${sbindir}/overlay
    install -Dm0755 ${S}/data-init.init ${D}${sysconfdir}/init.d/data-init
    install -Dm0644 ${S}/udhcpd.conf ${D}${sysconfdir}/udhcpd.conf
    install -Dm0644 ${S}/pipewire.conf ${D}${sysconfdir}/pipewire/pipewire.conf.d/10-anbernic.conf
    install -Dm0644 ${S}/panvk.sh ${D}${sysconfdir}/profile.d/panvk.sh
    install -Dm0644 ${S}/wayland.sh ${D}${sysconfdir}/profile.d/wayland.sh
    install -Dm0644 ${S}/zz-data.sh ${D}${sysconfdir}/profile.d/zz-data.sh
    install -Dm0755 ${S}/weston.init ${D}${sysconfdir}/init.d/weston
    install -Dm0644 ${S}/weston.ini ${D}${sysconfdir}/xdg/weston/weston.ini
    install -Dm0644 ${S}/sdcard.rules ${D}${sysconfdir}/udev/rules.d/90-sdcard.rules
    install -Dm0644 ${S}/usb-gadget.rules ${D}${sysconfdir}/udev/rules.d/90-usb-gadget.rules
    install -Dm0644 ${S}/uinput.rules ${D}${sysconfdir}/udev/rules.d/90-uinput.rules
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
RDEPENDS:${PN} = "busybox busybox-syslog dropbear weston seatd launcher corehost util-linux-sfdisk util-linux-mount e2fsprogs-mke2fs alsa-utils-amixer weston-volume-keys weston-frames ksmbd-tools avahi-daemon umtprd anbernic-data"
