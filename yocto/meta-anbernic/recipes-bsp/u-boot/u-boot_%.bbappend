FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Release tarball instead of a full git clone
SRC_URI:remove = "git://source.denx.de/u-boot/u-boot.git;protocol=https;branch=master;tag=v${PV}"
SRC_URI:prepend = "https://git.u-boot-project.org/u-boot/u-boot/-/archive/v${PV}/u-boot-v${PV}.tar.gz "
S = "${UNPACKDIR}/u-boot-v${PV}"

SRC_URI += "file://0001-sunxi-FIT-with-external-sector-aligned-data.patch file://0002-armv8-i-cache-on-before-relocation.patch file://0007-sunxi-axp717-no-boot-on-plug-in.patch file://0008-sunxi-H616-pick-LPDDR3-or-LPDDR4-by-the-boot0-DRAM-straps.patch file://0009-sunxi-H616-resume-from-super-standby.patch"
SRC_URI += "file://status-led.cfg file://charge-off.cfg file://dram-straps.cfg file://super-standby.cfg"

# sunxi SPL image embeds TF-A BL31; H616/H700 has no SCP firmware
do_compile[depends] += "trusted-firmware-a:do_deploy"
EXTRA_OEMAKE += "BL31=${DEPLOY_DIR_IMAGE}/trusted-firmware-a/bl31.bin SCP=/dev/null"
