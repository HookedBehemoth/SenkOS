SUMMARY = "What a new data partition starts with (the repo's data/), and the systems, emulators and players the launcher knows"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"

# data/ from the repo (as apps.bb takes apps/), systems/ and emulators/ from here
FILESEXTRAPATHS:prepend := "${@'/repo' if os.path.isdir('/repo/data') else os.path.abspath(d.getVar('THISDIR') + '/../../../..')}:"
SRC_URI = "file://data/ file://systems/ file://emulators/"
S = "${UNPACKDIR}"

# /data's tree: `ab mount` copies it onto a freshly made data partition. Only OS, apps, roms,
# music and videos are ours: settings in OS/config, the cores' saves in OS/saves, what they need in OS/assets/emulator
# (PPSSPP's: its recipe), game packs in OS/assets/thumbnails, the player's shader presets and overlays
# (tools/slangc, tools/border) in OS/assets/shaders and overlays, rootfs overlays in OS/overlay; root's
# init.d, profile.d and dropbear keys.
# Systems (*.system) and emulators and players (*.desktop): the image's; apps/systems and
# apps/emulators on the card over them (apps/launcher/apps.h).
do_install() {
    t=${D}${datadir}/senkos
    install -d $t/data/apps/emulators $t/data/apps/systems $t/data/roms $t/data/music $t/data/videos \
        $t/data/OS/config $t/data/OS/saves $t/data/OS/overlay $t/data/OS/init.d $t/data/OS/profile.d $t/data/OS/dropbear \
        $t/data/OS/assets/emulator $t/data/OS/assets/splash $t/data/OS/assets/thumbnails \
        $t/data/OS/assets/shaders $t/data/OS/assets/overlays
    cp -r --no-preserve=ownership ${S}/data/. $t/data/
    install -d $t/systems $t/emulators
    install -m0644 ${S}/systems/*.system $t/systems/
    install -m0644 ${S}/emulators/*.desktop $t/emulators/
    # The cores' paths say the handheld's /usr/lib64: the libdir here (qemu's is /usr/lib), which
    # makes this package the architecture's rather than allarch
    sed -i 's#/usr/lib64/#${libdir}/#' $t/emulators/*.desktop
}
FILES:${PN} = "${datadir}/senkos"
