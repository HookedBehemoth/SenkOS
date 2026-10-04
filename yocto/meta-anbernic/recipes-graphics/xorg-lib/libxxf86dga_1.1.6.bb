SUMMARY = "XFree86-DGA: XFree86 Direct Graphics Access extension library"
DESCRIPTION = "Part of the ports' platform (docs/ports.md): binaries built on Debian link libXxf86dga.so.1."

require recipes-graphics/xorg-lib/xorg-lib-common.inc

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://COPYING;md5=abb99ac125f84f424a4278153988e32f"

DEPENDS += "libxext xorgproto"

XORG_PN = "libXxf86dga"
SRC_URI[sha256sum] = "be44427579808fe3a217d59f51cae756a26913eb6e4c8738ccab65ff56d7980f"
