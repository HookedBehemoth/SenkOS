# The same library under the soname upstream's Makefile (and so Debian, and the games built there)
# gives it: libbz2.so.1.0
do_install:append() {
    ln -sf libbz2.so.1.0.8 ${D}${libdir}/libbz2.so.1.0
}
FILES:libbz2 += "${libdir}/libbz2.so.1.0"
