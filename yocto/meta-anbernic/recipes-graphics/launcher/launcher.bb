SUMMARY = "Handheld launcher: status bar + menus, pause screen, volume OSD; SDL2/HarfBuzz/fontconfig"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"

# Built from the repo's launcher/ (kas-container mounts the repo read-only at /repo): edit,
# `tools/deploy-launcher`, no recipe changes. The fetcher checksums every file in it.
FILESEXTRAPATHS:prepend := "${@'/repo' if os.path.isdir('/repo/launcher') else os.path.abspath(d.getVar('THISDIR') + '/../../../..')}:"
SRC_URI = "file://launcher/"
S = "${UNPACKDIR}/launcher"

inherit meson pkgconfig deploy

DEPENDS = "libsdl2 wayland wayland-native alsa-lib harfbuzz fontconfig freetype"
# The image's fallback UI needs a font to find
RDEPENDS:${PN} = "liberation-fonts"

# The bare binary, for tools/deploy-launcher to copy to /data
do_deploy() {
    install -m 0755 ${B}/launcher ${DEPLOYDIR}/launcher
}
addtask deploy after do_compile before do_build
