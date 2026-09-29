SUMMARY = "OpenGL (ES) 2.0 benchmark, Wayland EGL flavor"
LICENSE = "GPL-3.0-or-later & SGI-B-2.0"
LIC_FILES_CHKSUM = "file://COPYING;md5=d32239bcb673463ab874e80d47fae504 file://COPYING.SGI;md5=7125c8894bd29eddfd44ede5ce3ab1e4"
SRC_URI = "https://github.com/glmark2/glmark2/archive/refs/tags/${PV}.tar.gz;downloadfilename=${BP}.tar.gz"
SRC_URI[sha256sum] = "8fece3fc323b643644a525be163dc4931a4189971eda1de8ad4c1712c5db3d67"
# Tag tarball over a git clone; the sha256 pins it
ERROR_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/${BP}"

inherit meson pkgconfig

# EGL/GLES come from glad + dlopen (libglvnd at run time); no headers needed
DEPENDS = "libpng jpeg wayland wayland-native wayland-protocols"
EXTRA_OEMESON = "-Dflavors=wayland-glesv2"
