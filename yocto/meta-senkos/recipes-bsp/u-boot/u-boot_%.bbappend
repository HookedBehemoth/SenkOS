FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Boot speed and a quiet console
SRC_URI += "file://0003-sunxi-silent-console-by-default.patch file://0004-board_f-no-pre-relocation-serial-probe-when-silent.patch file://0005-lz4-inline-fixed-size-copies-unaligned-arm64.patch file://0006-fs-squashfs-read-file-data-in-runs-decompress-in-place.patch"
SRC_URI += "file://fastboot.cfg file://watchdog.cfg file://squashfs.cfg file://silent.cfg file://boot.cmd file://splash.lz4"

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

# Boot-time marks (SPL, FIT, init phases, file loads) exported to Linux as
# /sys/firmware/devicetree/base/bootstage; read with tools/bootstage. Set UBOOT_BOOTSTAGE = "1"
# in local.conf, rebuild and flash the bootloader.
UBOOT_BOOTSTAGE ??= "0"
SRC_URI += "${@'file://bootstage-marks.patch file://bootstage.cfg' if d.getVar('UBOOT_BOOTSTAGE') == '1' else ''}"
