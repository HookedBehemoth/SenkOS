FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Release tarball instead of a full git clone
SRC_URI:remove = "git://source.denx.de/u-boot/u-boot.git;protocol=https;branch=master;tag=v${PV}"
SRC_URI:prepend = "https://git.u-boot-project.org/u-boot/u-boot/-/archive/v${PV}/u-boot-v${PV}.tar.gz "
S = "${UNPACKDIR}/u-boot-v${PV}"

SRC_URI += "file://0001-sunxi-FIT-with-external-sector-aligned-data.patch file://0002-armv8-i-cache-on-before-relocation.patch file://0003-sunxi-silent-console-by-default.patch file://0004-board_f-no-pre-relocation-serial-probe-when-silent.patch file://0005-lz4-inline-fixed-size-copies-unaligned-arm64.patch file://0006-fs-squashfs-read-file-data-in-runs-decompress-in-place.patch file://0007-sunxi-axp717-no-boot-on-plug-in.patch file://0008-sunxi-H616-pick-LPDDR3-or-LPDDR4-by-the-boot0-DRAM-straps.patch file://0009-sunxi-H616-resume-from-super-standby.patch"
SRC_URI += "file://fastboot.cfg file://status-led.cfg file://watchdog.cfg file://squashfs.cfg file://silent.cfg file://charge-off.cfg file://dram-straps.cfg file://super-standby.cfg file://boot.cmd file://splash.lz4"

# boot.cmd -> boot.scr (A/B slot selection), made with the mkimage this build already produces
# instead of u-boot-mkimage-native (a second U-Boot fetch/build)
UBOOT_ENV = "boot"
UBOOT_ENV_SUFFIX = "scr"
UBOOT_MKIMAGE = "${B}/tools/mkimage"
DEPENDS:remove = "u-boot-mkimage-native"

# Boot splash (tools/mksplash.py)
do_deploy:append() {
    install -m 0644 ${UNPACKDIR}/splash.lz4 ${DEPLOYDIR}/splash.lz4
}

# sunxi SPL image embeds TF-A BL31; H616/H700 has no SCP firmware
do_compile[depends] += "trusted-firmware-a:do_deploy"
EXTRA_OEMAKE += "BL31=${DEPLOY_DIR_IMAGE}/trusted-firmware-a/bl31.bin SCP=/dev/null"

# Boot-time marks (SPL, FIT, init phases, file loads) exported to Linux as
# /sys/firmware/devicetree/base/bootstage; read with tools/bootstage. Set UBOOT_BOOTSTAGE = "1"
# in local.conf, rebuild and flash the bootloader.
UBOOT_BOOTSTAGE ??= "0"
SRC_URI += "${@'file://bootstage-marks.patch file://bootstage.cfg' if d.getVar('UBOOT_BOOTSTAGE') == '1' else ''}"
