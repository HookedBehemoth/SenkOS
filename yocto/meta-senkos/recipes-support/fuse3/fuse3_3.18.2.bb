SUMMARY = "libfuse 3: the library squashfuse serves squashfs images with"
# Only the library: no fusermount3. Whoever runs squashfuse mounts /dev/fuse itself in a user
# namespace and hands it the fd, so nothing setuid is needed.
LICENSE = "LGPL-2.1-only"
LIC_FILES_CHKSUM = "file://LGPL2.txt;md5=4fbd65380cdd255951079008b364516c"
SRC_URI = "https://github.com/libfuse/libfuse/releases/download/fuse-${PV}/fuse-${PV}.tar.gz"
SRC_URI[sha256sum] = "f01de85717e20adf5f98aff324acd85dd73d61a5ca3834d573dcf0bd6e54a298"
S = "${UNPACKDIR}/fuse-${PV}"

inherit meson pkgconfig

EXTRA_OEMESON = "-Dutils=false -Dexamples=false -Dtests=false -Denable-io-uring=false -Ddisable-mtab=true"
