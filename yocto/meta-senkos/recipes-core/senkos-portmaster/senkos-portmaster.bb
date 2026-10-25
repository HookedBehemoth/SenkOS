SUMMARY = "PortMaster's ports as they are: bash, Python for its GUI, /roms, FUSE mounts for their runtimes"
# MIT: PortMaster.zip, whose bundled parts carry their own in PortMaster/licenses
LICENSE = "AGPL-3.0-only & MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788 \
    file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# PortMaster's GUI release: `portmaster` extracts it onto the card when it isn't there, then it
# updates itself
PORTMASTER_VERSION = "2026.10.24-1611"
SRC_URI = "file://portmaster file://mod_SenkOS.txt file://mount file://umount file://portmaster.desktop file://portmaster.png \
    https://github.com/PortsMaster/PortMaster-GUI/releases/download/${PORTMASTER_VERSION}/PortMaster.zip;downloadfilename=PortMaster-${PORTMASTER_VERSION}.zip;unpack=0 \
"
SRC_URI[sha256sum] = "a1c6df01b40cbefeef9ce1f6d3d48ad7c365e75fa4c10438d865ea39ce93e0cd"
S = "${UNPACKDIR}"

# PortMaster itself is on the card (`portmaster` extracts the image's PortMaster.zip there); this is
# what it expects of the OS, and its app in the launcher's Applications.
# /roms: where ports are on most of the OSes they're made for (control.txt's $directory)
do_install() {
    install -Dm0755 ${S}/portmaster ${D}${bindir}/portmaster
    install -Dm0644 ${S}/mod_SenkOS.txt ${D}${datadir}/senkos/portmaster/mod_SenkOS.txt
    sed -i 's#@LIBDIR@#${libdir}#' ${D}${datadir}/senkos/portmaster/mod_SenkOS.txt
    install -m0644 ${S}/PortMaster-${PORTMASTER_VERSION}.zip ${D}${datadir}/senkos/portmaster/PortMaster.zip
    install -Dm0755 ${S}/mount ${D}${libexecdir}/portmaster/mount
    install -Dm0755 ${S}/umount ${D}${libexecdir}/portmaster/umount
    install -Dm0644 ${S}/portmaster.desktop ${D}${datadir}/senkos/applications/portmaster.desktop
    install -m0644 ${S}/portmaster.png ${D}${datadir}/senkos/applications/
    ln -s /data/roms ${D}/roms
}

FILES:${PN} = "/roms ${bindir} ${datadir}/senkos/portmaster ${datadir}/senkos/applications ${libexecdir}/portmaster"
# CFW_NAME is os-release's NAME (device_info.txt); the GUI is Python (pysdl2 over the image's SDL2).
# zip and jq: what ports' patch scripts look for (GameMaker ports repack their data with zip)
RDEPENDS:${PN} = "bash python3-modules zip jq squashfuse fusermount3 os-release util-linux-mount libsdl2 libsdl2-ttf libsdl2-image"
