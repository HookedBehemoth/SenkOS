# Bonjour for the SMB server (`smb`, senkos-base): macOS Finder and Linux file managers list the
# handheld under Network. Only on the USB link; no D-Bus (nothing on the device asks avahi anything).
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://avahi-daemon.conf file://smb.service"
PACKAGECONFIG = ""

do_install:append() {
    install -m 0644 ${UNPACKDIR}/avahi-daemon.conf ${D}${sysconfdir}/avahi/avahi-daemon.conf
    install -m 0644 ${UNPACKDIR}/smb.service ${D}${sysconfdir}/avahi/services/smb.service
}
