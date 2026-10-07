FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

COMPATIBLE_MACHINE:rg35xx-sp = "rg35xx-sp"

# PSCI SYSTEM_SUSPEND ("deep" sleep) for the H616/H700 (patch from ROCKNIX): BL31 runs an
# SRAM stub that puts DRAM into self-refresh, WFIs for the wake IRQ, and re-initialises DRAM.
# The stub is built from U-Boot's own DRAM driver + our U-Boot defconfig, so it trains DRAM
# with the exact parameters our SPL uses. BL31 refuses suspend if they don't match at runtime.
# Super standby on top (0002 + the stub patch): the PMIC cuts the SoC's power, SPL resumes;
# see docs/suspend.md.
STUB_REV = "712653d11230b2450061ddb8e25f31e072da7a48"
SRC_URI:append:rg35xx-sp = " \
    file://0001-allwinner-h616-psci-system-suspend.patch \
    file://0002-allwinner-h616-super-standby.patch \
    file://stub-0001-super-standby.patch;patchdir=${UNPACKDIR}/h700-suspend-stub-${STUB_REV} \
    https://github.com/Jacob-Matthew-Cook/h700-suspend-stub/archive/${STUB_REV}.tar.gz;name=stub;downloadfilename=h700-suspend-stub-${STUB_REV}.tar.gz \
    https://git.u-boot-project.org/u-boot/u-boot/-/archive/v2026.01/u-boot-v2026.01.tar.gz;name=uboot \
"
# Release/commit tarballs over git clones; the sha256 pins them, so an archive that changes fails the fetch
ERROR_QA:remove = "src-uri-bad"
SRC_URI[stub.sha256sum] = "3698da9aad40c26c10f6aa31f8366158914e0a351d31f1084c6b8032147f4616"
SRC_URI[uboot.sha256sum] = "33b09b59bdf1f175c0c9b69dfebcfaa3eeba5313aabbad4791ef1ee12e422f16"

STUB_DIR = "${UNPACKDIR}/h700-suspend-stub-${STUB_REV}"
STUB_UBOOT = "${UNPACKDIR}/u-boot-v2026.01"

do_compile:prepend:rg35xx-sp() {
    mkdir -p ${B}/suspend-stub
    # plain make: EXTRA_OEMAKE (CC/LD/PLAT/...) is for TF-A, the stub sets its own freestanding flags
    make -C ${B}/suspend-stub -f ${STUB_DIR}/Makefile SRC_DIR=${STUB_DIR} \
        UBOOT_DIR=${STUB_UBOOT} ATF_DIR=${S} DEFCONFIG=${STUB_UBOOT}/configs/${UBOOT_MACHINE} \
        OUT=suspend_stub_lpddr4.bin CROSS_COMPILE=${TARGET_PREFIX}
}
EXTRA_OEMAKE:append:rg35xx-sp = " SUNXI_SYSTEM_SUSPEND=1 SUNXI_SUSPEND_STUB=${STUB_DIR}/suspend_stub_lpddr4.bin"
# Errors only: each NOTICE line costs ~4 ms at 115200 baud on every boot
EXTRA_OEMAKE:append:rg35xx-sp = " LOG_LEVEL=10"
