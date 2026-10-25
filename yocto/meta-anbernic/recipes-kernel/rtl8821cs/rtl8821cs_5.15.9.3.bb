SUMMARY = "Realtek RTL8821CS SDIO vendor Wi-Fi driver"
HOMEPAGE = "https://github.com/u-osmi/rtl8821cs-arm64"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://core/rtw_cmd.c;beginline=1;endline=14;md5=ca671256c791bbbf7c985ca88dc89fc9"

# Same revision and embedded 24.13 firmware used in the hardware investigation.
SRCREV = "d28399aa2de118d70d128a1d5c4ad9ec667f6f57"
SRC_URI = "git://github.com/u-osmi/rtl8821cs-arm64.git;protocol=https;branch=main \
           file://0001-linux-7.2-compatibility.patch"

inherit module

COMPATIBLE_MACHINE = "h700"

# Use Yocto's kernel toolchain and prepared build, bypassing the upstream
# standalone Makefile's host/platform detection and installation commands.
EXTRA_OEMAKE += "CONFIG_PLATFORM_ARM64_ALL=n CONFIG_RTL8821CS=m USER_MODULE_NAME=rtl8821cs"
EXTRA_OEMAKE += "'USER_EXTRA_CFLAGS=-DCONFIG_LITTLE_ENDIAN -DCONFIG_IOCTL_CFG80211 -DRTW_USE_CFG80211_STA_EVENT'"

do_compile() {
    unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS
    oe_runmake -C ${STAGING_KERNEL_DIR} O=${STAGING_KERNEL_BUILDDIR} M=${S} \
        CC="${KERNEL_CC}" LD="${KERNEL_LD}" AR="${KERNEL_AR}" \
        OBJCOPY="${KERNEL_OBJCOPY}" STRIP="${KERNEL_STRIP}" modules
}

do_install() {
    install -Dm0644 ${S}/rtl8821cs.ko ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/extra/rtl8821cs.ko
}
