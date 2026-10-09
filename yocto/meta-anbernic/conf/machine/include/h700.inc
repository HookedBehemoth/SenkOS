# Allwinner H700 (H616 family): what every board on it shares
MACHINEOVERRIDES =. "h700:"

# H700 has the ARMv8 Crypto Extensions (AES/SHA1/SHA2/PMULL): let everything use them
DEFAULTTUNE = "cortexa53-crypto"
require conf/machine/include/arm/armv8a/tune-cortexa53.inc

PREFERRED_PROVIDER_virtual/kernel = "linux-anbernic"
KERNEL_IMAGETYPE = "Image"

# TF-A BL31 gets linked into the U-Boot SPL image. 2.12 LTS: the H616 SYSTEM_SUSPEND patch
# (see trusted-firmware-a_%.bbappend) uses the pre-2.13 pwr_domain_pwr_down_wfi PSCI hook.
PREFERRED_VERSION_trusted-firmware-a = "2.12%"
TFA_PLATFORM = "sun50i_h616"
TFA_BUILD_TARGET = "bl31"
TFA_INSTALL_TARGET = "bl31"
TFA_DEBUG = "0"

PREFERRED_PROVIDER_virtual/bootloader = "u-boot"
SPL_BINARY = "u-boot-sunxi-with-spl.bin"
PREFERRED_VERSION_u-boot = "2026.07"
EXTRA_IMAGEDEPENDS += "u-boot"

# Mali-G31 (Bifrost v7): panfrost for GL, PanVK for Vulkan, from the Mesa snapshot in recipes-graphics/mesa.
# libclc: panfrost needs mesa-clc at build time (native LLVM/clang; nothing of it lands on target)
PACKAGECONFIG:append:pn-mesa = " panfrost libclc"
