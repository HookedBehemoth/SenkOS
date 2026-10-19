SUMMARY = "libfuse 2, the library only: older AppImage runtimes load libfuse.so.2"
# They mount with fusermount, which fuse3 links to fusermount3
LICENSE = "LGPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING.LIB;md5=4fbd65380cdd255951079008b364516c"
SRC_URI = "https://github.com/libfuse/libfuse/releases/download/${BP}/${BP}.tar.gz \
           file://aarch64.patch \
           file://0001-fuse-fix-the-return-value-of-help-option.patch"
SRC_URI[sha256sum] = "d0e69d5d608cc22ff4843791ad097f554dd32540ddc9bed7638cc6fea7c1b4b5"

inherit autotools pkgconfig

DEPENDS = "gettext-native"
EXTRA_OECONF = "--disable-util --disable-example --disable-mtab"

do_install:append() {
    rm -rf ${D}/dev ${D}${sysconfdir}
}
PACKAGES =+ "libfuse2"
FILES:libfuse2 = "${libdir}/libfuse.so.*"
