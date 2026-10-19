SUMMARY = "squashfuse: squashfs over FUSE (squashfuse_ll), and the library the launcher reads AppImages with"
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=d5313a8e4d3bf0f9797037a8982e61ba"
SRC_URI = "https://github.com/vasi/squashfuse/releases/download/${PV}/squashfuse-${PV}.tar.gz"
SRC_URI[sha256sum] = "7b18a58c40a3161b5c329ae925b72336b5316941f906b446b8ed6c5a90989f8c"

inherit autotools pkgconfig

# gzip, zstd and lz4 images
DEPENDS = "fuse3 zlib zstd lz4"
EXTRA_OECONF = "--disable-demo --without-lzo --without-xz"

# The low-level one, and libsquashfuse (no FUSE in it)
do_install:append() {
    rm -rf ${D}${libdir}/libsquashfuse_ll* ${D}${libdir}/pkgconfig/squashfuse_ll.pc ${D}${includedir}/squashfuse/ll.h \
        ${D}${bindir}/squashfuse ${D}${datadir}
}
PACKAGES =+ "libsquashfuse"
FILES:libsquashfuse = "${libdir}/libsquashfuse.so.*"
FILES:${PN} = "${bindir}/squashfuse_ll"
