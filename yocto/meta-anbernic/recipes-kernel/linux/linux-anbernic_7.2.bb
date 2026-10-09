SUMMARY = "Mainline Linux with ROCKNIX H700 patches for Anbernic H700 boards"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=6bc538ed5bd9a7fc9398086aedcd7e46"

inherit kernel

COMPATIBLE_MACHINE = "h700"

# 0001-0029: ROCKNIX's H700 board patches (their number is the second one in the name),
# minus their out-of-tree joypad driver: the buttons are mainline gpio-keys.
# 0030-0032, 0037, 0041-0057: ours (0044-0051: super standby, docs/suspend.md). 0009 (battery name)
# and 0052 (boot splash) are the distro's, in meta-senkos.
# 0033-0036: Cedrus H616 + VP9 from LibreELEC; 0038-0040: its P010 output for 10-bit VP9 and
# HEVC (without them the engine writes 10-bit streams as 8-bit NV12).
SRC_URI = "https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-${PV}.tar.xz \
    file://defconfig \
    file://firmware \
    file://0001-0002-rg35xx-enable-HDMI-LCD.patch \
    file://0002-0003-Update-sun8i_tcon_top.c.patch \
    file://0003-0007-rg35xx-add-GPU-opp.patch \
    file://0004-0008-v8_20260804_james_hilliard1_introduce_allwinner.patch \
    file://0005-0010-rg35xx-enable-pwm-backlight.patch \
    file://0006-0040-Revert-usb-musb-Fix-hardware-lockup-on-first-Rx.patch \
    file://0007-0110-v2_20250226_kikuchan98_drm_panel_add_generic_mi.patch \
    file://0008-0111-rg35xx-2024-use-panel-mipi-dpi-spi-driver.patch \
    file://0010-0126-20241018_macroalpha82_rg35xx_add_additional_har.patch \
    file://0011-0127-enable-mmc1-for-RG35XX-2024.patch \
    file://0012-0151-phy-fix-OTG-host-mode.patch \
    file://0013-0152-rg35xx-2024-enable-usb-otg.patch \
    file://0014-0153-enable-rgb-leds.patch \
    file://0015-0154-rocknix-dt-id.patch \
    file://0016-0155-sun4i-set-rgb-connector-as-DSI.patch \
    file://0017-0156-rg35xx-plus-add-pcf8563-rtc.patch \
    file://0018-0157-input-axp20x-pek-do-not-report-the-key-press-th.patch \
    file://0019-0158-mmc-sunxi-mmc-mask-data0-when-updating-the-cloc.patch \
    file://0020-0159-arm64-dts-rg35xx-2024-keep-the-second-sd-card-p.patch \
    file://0021-0160-phy-sun4i-usb-disable-the-clock-that-was-enable.patch \
    file://0022-0161-drm-sun4i-gate-the-display-pipeline-across-syst.patch \
    file://0023-0162-pinctrl-sunxi-release-the-lcd0-pads-while-the-p.patch \
    file://0024-0163-asoc-sun4i-codec-suspend-the-codec-and-switch-t.patch \
    file://0025-0200-h700-update-opps.patch \
    file://0026-0204-dts-Enable-hdmi-sound.patch \
    file://0027-0210-h616-cpu-opp-mark-480MHz-as-suspend-opp.patch \
    file://0028-0901-dts-rg35xx-2024-fix-led-1-color.patch \
    file://0029-arm64-dts-allwinner-add-RG35XX-SP-v2-panel-dts.patch \
    file://0030-drm-sun4i-rgb-set-panel-orientation-before-connector.patch \
    file://0031-arm64-dts-allwinner-h616-add-the-video-engine-Cedrus.patch \
    file://0032-iommu-sun50i-only-translate-masters-that-are-IOMMU-c.patch \
    file://0033-drivers-staging-media-sunxi-cedrus-add-H616-variant.patch \
    file://0034-media-cedrus-Don-t-CPU-map-source-buffers.patch \
    file://0035-media-cedrus-Add-VP9-decoding-support.patch \
    file://0036-media-cedrus-Select-V4L2_VP9-for-the-VP9-helpers.patch \
    file://0037-Bluetooth-hci_h5-register-from-a-work-item.patch \
    file://0038-media-cedrus-Add-P010-capture-format-and-shared-10-b.patch \
    file://0039-media-cedrus-Add-VP9-P010-output-support.patch \
    file://0040-media-cedrus-Add-H265-P010-output-support.patch \
    file://0041-arm64-dts-allwinner-rg35xx-sp-swap-the-X-and-Y-keys.patch \
    file://0042-arm64-dts-allwinner-h616-put-the-display-engine-behind-the-IOMMU.patch \
    file://0043-arm64-dts-allwinner-rg35xx-2024-keep-the-DRAM-voltage-from-SPL.patch \
    file://0044-iommu-sun50i-program-the-IOMMU-again-after-a-power-l.patch \
    file://0045-thermal-sun8i-calibrate-and-set-up-the-sensors-again.patch \
    file://0046-drm-sun4i-mixer-set-the-DE33-mixer-up-again-on-resum.patch \
    file://0047-drm-sun4i-tcon-top-restore-the-registers-on-resume.patch \
    file://0048-drm-sun4i-tcon-keep-the-dot-clock-divider-across-sus.patch \
    file://0049-usb-musb-sunxi-set-the-PHY-up-again-on-resume.patch \
    file://0050-clk-sunxi-ng-de2-restore-the-DE33-clock-registers-on.patch \
    file://0051-soc-sunxi-restore-the-SoC-state-after-an-H616-super-.patch \
    file://0053-mmc-sunxi-power-modules-with-a-pwrseq-up-from-the-re.patch \
    file://0054-backlight-pwm_bl-honour-default-brightness-level-wit.patch \
    file://0055-ASoC-sun4i-codec-keep-the-H616-module-clock-while-th.patch \
    file://0056-power-supply-axp20x_battery-report-the-AXP717-s-batt.patch \
    file://0057-backlight-pwm_bl-start-the-LED-at-a-tenth-when-lit-f.patch \
"
SRC_URI[sha256sum] = "f9fef3d14c0df53819026f4be74459835c2a0b0dcbf5b5bbd9ea19f0829402b3"

S = "${UNPACKDIR}/linux-${PV}"

# Panel init sequences + wifi/bt firmware are built into the kernel
# (CONFIG_EXTRA_FIRMWARE) so the display comes up without an initramfs.
do_configure:prepend() {
    cp -rT ${UNPACKDIR}/firmware ${S}/external-firmware
}
