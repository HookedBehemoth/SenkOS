SUMMARY = "libfuse 3, and fusermount3 (setuid): AppImages mount themselves with it"
LICENSE = "LGPL-2.1-only"
LIC_FILES_CHKSUM = "file://LGPL2.txt;md5=4fbd65380cdd255951079008b364516c"
SRC_URI = "https://github.com/libfuse/libfuse/releases/download/fuse-${PV}/fuse-${PV}.tar.gz"
SRC_URI[sha256sum] = "f01de85717e20adf5f98aff324acd85dd73d61a5ca3834d573dcf0bd6e54a298"
S = "${UNPACKDIR}/fuse-${PV}"

inherit meson pkgconfig

EXTRA_OEMESON = "-Dinitscriptdir= -Dudevrulesdir=${nonarch_base_libdir}/udev/rules.d -Dexamples=false -Dtests=false -Denable-io-uring=false -Ddisable-mtab=true"

# fusermount: the name libfuse 2 runs (old AppImages); fusermount3 takes its options. No
# mount.fuse3, no udev rule: /dev/fuse is 0666 from the kernel.
do_install:append() {
    rm -rf ${D}/dev ${D}${sbindir} ${D}${nonarch_base_libdir}/udev
    rmdir ${D}${nonarch_base_libdir}
    ln -s fusermount3 ${D}${bindir}/fusermount
}
PACKAGES =+ "fusermount3"
FILES:fusermount3 = "${bindir}/fusermount3 ${bindir}/fusermount ${sysconfdir}/fuse.conf"
