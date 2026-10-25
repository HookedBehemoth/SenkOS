SUMMARY = "PortMaster's ports as they are: bash, Python for its GUI, /roms, FUSE mounts for their runtimes"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"

SRC_URI = "file://portmaster file://mod_SenkOS.txt file://mount file://umount"
S = "${UNPACKDIR}"

# PortMaster itself is on the card (`portmaster` downloads it); this is what it expects of the OS.
# /roms: where ports are on most of the OSes they're made for (control.txt's $directory)
do_install() {
    install -Dm0755 ${S}/portmaster ${D}${bindir}/portmaster
    install -Dm0644 ${S}/mod_SenkOS.txt ${D}${datadir}/senkos/portmaster/mod_SenkOS.txt
    sed -i 's#@LIBDIR@#${libdir}#' ${D}${datadir}/senkos/portmaster/mod_SenkOS.txt
    install -Dm0755 ${S}/mount ${D}${libexecdir}/portmaster/mount
    install -Dm0755 ${S}/umount ${D}${libexecdir}/portmaster/umount
    ln -s /data/roms ${D}/roms
}

FILES:${PN} = "/roms ${bindir} ${datadir}/senkos/portmaster ${libexecdir}/portmaster"
# CFW_NAME is os-release's NAME (device_info.txt); the GUI is Python (pysdl2 over the image's SDL2).
# zip and jq: what ports' patch scripts look for (GameMaker ports repack their data with zip)
RDEPENDS:${PN} = "bash python3-modules zip jq squashfuse fusermount3 os-release util-linux-mount libsdl2 libsdl2-ttf libsdl2-image"
