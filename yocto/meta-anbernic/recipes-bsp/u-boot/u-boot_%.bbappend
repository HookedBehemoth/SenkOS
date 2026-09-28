FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Release tarball instead of a full git clone (source.denx.de also redirects to a host git can't follow)
SRC_URI:remove = "git://source.denx.de/u-boot/u-boot.git;protocol=https;branch=master;tag=v${PV}"
SRC_URI:prepend = "https://git.u-boot-project.org/u-boot/u-boot/-/archive/v${PV}/u-boot-v${PV}.tar.gz "
SRC_URI[sha256sum] = "33b09b59bdf1f175c0c9b69dfebcfaa3eeba5313aabbad4791ef1ee12e422f16"
S = "${UNPACKDIR}/u-boot-v${PV}"

SRC_URI += "file://fastboot.cfg file://status-led.cfg file://watchdog.cfg file://squashfs.cfg file://boot.cmd"

# boot.cmd -> boot.scr (A/B slot selection), made with the mkimage this build already produces
# instead of u-boot-mkimage-native (a second U-Boot fetch/build)
UBOOT_ENV = "boot"
UBOOT_ENV_SUFFIX = "scr"
UBOOT_MKIMAGE = "${B}/tools/mkimage"
DEPENDS:remove = "u-boot-mkimage-native"

# sunxi SPL image embeds TF-A BL31; H616/H700 has no SCP firmware
do_compile[depends] += "trusted-firmware-a:do_deploy"
EXTRA_OEMAKE += "BL31=${DEPLOY_DIR_IMAGE}/trusted-firmware-a/bl31.bin SCP=/dev/null"
