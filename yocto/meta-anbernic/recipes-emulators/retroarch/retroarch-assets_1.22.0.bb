SUMMARY = "RetroArch menu assets: Ozone, XMB (monochrome), MaterialUI, RGUI, fonts, sounds"
LICENSE = "CC-BY-4.0"
LIC_FILES_CHKSUM = "file://COPYING;md5=7bd61880991ed797753fcc00acae2c51"
SRC_URI = "https://github.com/libretro/retroarch-assets/archive/refs/tags/v${PV}.tar.gz;downloadfilename=retroarch-assets-${PV}.tar.gz"
SRC_URI[sha256sum] = "183c638bf2e9273c975e1f46c2d19eff2f63529312856a984c0a777f973672ac"
# No release archive; the tag archive is pinned by the checksum above
ERROR_QA:remove = "src-uri-bad"
WARN_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/retroarch-assets-${PV}"
inherit allarch deploy

# ~30 of 290 MB: skip the wallpapers, other XMB themes, console-port menus and the SVG sources
ASSET_DIRS = "ozone glui rgui pkg fonts sounds xmb/monochrome"

do_configure[noexec] = "1"
do_compile[noexec] = "1"
do_install() {
	for d in ${ASSET_DIRS}; do
		install -d ${D}${datadir}/libretro/assets/$(dirname $d)
		cp -r --no-preserve=ownership ${S}/$d ${D}${datadir}/libretro/assets/$d
	done
}
FILES:${PN} = "${datadir}/libretro/assets"

do_deploy() {
	install -d ${DEPLOYDIR}/retroarch-data/retroarch
	cp -r --no-preserve=ownership ${D}${datadir}/libretro/assets ${DEPLOYDIR}/retroarch-data/retroarch/
}
addtask deploy after do_install before do_build
