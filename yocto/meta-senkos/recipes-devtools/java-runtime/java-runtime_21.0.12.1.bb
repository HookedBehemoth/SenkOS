SUMMARY = "Java 21 runtime (Temurin) for FreeJ2ME: java.base, java.desktop, java.rmi"
LICENSE = "GPL-2.0-with-classpath-exception"
LIC_FILES_CHKSUM = "file://legal/java.base/LICENSE;md5=3e0b59f8fac05c3c03d4a26bbda13f8f"
# The target JDK's jmods, linked by the build host's jlink of the same version
TEMURIN = "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1"
SRC_URI = "${TEMURIN}/OpenJDK21U-jdk_aarch64_linux_hotspot_21.0.12.1_1.tar.gz;name=target;subdir=target;striplevel=1 \
    ${TEMURIN}/OpenJDK21U-jdk_x64_linux_hotspot_21.0.12.1_1.tar.gz;name=host;subdir=host;striplevel=1 \
    file://java"
SRC_URI[target.sha256sum] = "23e37e026f12f3e706f18938ff611db3032d075b09d0879a25d06718c773e223"
SRC_URI[host.sha256sum] = "ce79869e1307ed8ee1e2baa86a412b1eb5b75d10a01006d788a6f968bcfaee94"
S = "${UNPACKDIR}/target"
# libjsound.so links ALSA (its shlib provider)
DEPENDS = "alsa-lib"
COMPATIBLE_HOST = "aarch64.*-linux"

do_compile() {
	rm -rf ${B}/jre
	${UNPACKDIR}/host/bin/jlink --module-path ${S}/jmods --add-modules java.base,java.desktop,java.rmi \
		--strip-java-debug-attributes --no-man-pages --no-header-files --compress=zip-6 --output ${B}/jre
	# X11 (no X here: AWT runs headless), the splash screen, tools
	rm -f ${B}/jre/lib/libawt_xawt.so ${B}/jre/lib/libjawt.so ${B}/jre/lib/libsplashscreen.so \
		${B}/jre/bin/keytool ${B}/jre/bin/rmiregistry
}

do_install() {
	install -d ${D}${libdir}/jvm
	cp -R --no-preserve=ownership ${B}/jre ${D}${libdir}/jvm/java-21
	install -Dm0755 ${UNPACKDIR}/java ${D}${bindir}/java
}
FILES:${PN} = "${libdir}/jvm ${bindir}/java"
# Prebuilt: its own linking; its private libs (libjvm.so, libawt.so) found by RPATH
INSANE_SKIP:${PN} = "already-stripped ldflags textrel dev-so"
PRIVATE_LIBS = "libjvm.so libjava.so libjli.so libawt.so libnet.so libfreetype.so libnio.so libzip.so \
    libverify.so libjimage.so libjsig.so libawt_headless.so libfontmanager.so libjavajpeg.so liblcms.so \
    libmlib_image.so libjsound.so libprefs.so librmi.so libsyslookup.so"
