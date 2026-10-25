# Bonjour for the SMB server (`smb`, senkos-base): macOS Finder and Linux file managers list the
# handheld under Network. Only on the USB link; no D-Bus (nothing on the device asks avahi anything).
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://avahi-daemon.conf"
PACKAGECONFIG = ""

do_install:append() {
    install -m 0644 ${UNPACKDIR}/avahi-daemon.conf ${D}${sysconfdir}/avahi/avahi-daemon.conf
    # `smb` writes the service there, named after the device tree's model
    rm -rf ${D}${sysconfdir}/avahi/services
    ln -s /run/avahi-services ${D}${sysconfdir}/avahi/services
}
