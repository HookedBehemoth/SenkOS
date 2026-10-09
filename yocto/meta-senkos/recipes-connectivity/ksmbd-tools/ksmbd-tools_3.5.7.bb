SUMMARY = "ksmbd-tools: userspace for the in-kernel SMB3 server (ksmbd.mountd: users, shares)"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://COPYING;md5=95f3a93a5c3c7888de623b46ea085a84"
SRC_URI = "https://github.com/cifsd-team/ksmbd-tools/releases/download/${PV}/ksmbd-tools-${PV}.tar.gz"
SRC_URI[sha256sum] = "d8e532b2d032e3f447f3fd19e0f9ea0abeb355285f8d6871ce9036bc6393a5b2"
S = "${UNPACKDIR}/ksmbd-tools-${PV}"

inherit meson pkgconfig
DEPENDS = "glib-2.0 libnl"

EXTRA_OEMESON = "-Dkrb5=disabled -Drundir=/run"

# Config, users and the generated SID live in /run (read-only root), written by `smb` (senkos-base)
do_install:append() {
    rm -rf ${D}${sysconfdir}/ksmbd
    ln -s /run/ksmbd ${D}${sysconfdir}/ksmbd
}

FILES:${PN} += "${libexecdir}/ksmbd.tools"
