# RG35XX-SP hardware

| Part | What | Evidence |
|---|---|---|
| SoC | Allwinner **H700** (sun50iw9, H616 family), 4x Cortex-A53 @ up to 1.34 GHz (BSP OPPs 600/792/1032/1296/1344 MHz) | dmesg `sun50iw9p1-pinctrl`, `cpu cpu0: _opp_add` |
| RAM | 1 GB **LPDDR4 @ 672 MHz** (`dram_type=8`, `dram_clk=0x2a0`). DDR3 units exist — ROCKNIX ships both U-Boots | active `dram_para` in [raw/bsp/sunxi.dts](raw/bsp/sunxi.dts), see [raw/bsp/active-dram-para.txt](raw/bsp/active-dram-para.txt) |
| GPU | Mali-G31 MP2 (Bifrost, "arch 7.0.9") → mainline **panfrost** | dmesg `mali gpu: GPU identified as 0x3 arch 7.0.9` |
| PMIC | X-Powers **AXP717** (BSP calls it axp2202) on i2c-5 @0x34 | dmesg `axp2202-pek ... 5-0034` |
| RTC | NXP **PCF8563** on i2c-5 @0x51 (+ SoC RTC) | dmesg `rtc-pcf8563 5-0051` |
| Display | 3.5" 640x480 IPS, parallel RGB (`lcd_if=0`), SPI-initialised. BSP driver `fog_fj035fhd05_v1`; dclk 48 MHz, ht 1414 / hbp 48 / hspw 8, vt 566 / vbp 48 / vspw 4; PWM0 backlight 50 kHz. Newer units have a "v2 panel" | [raw/bsp/sunxi.dts](raw/bsp/sunxi.dts) `lcd0@01c0c000`, ROCKNIX dts |
| HDMI | mini-HDMI, HDMI 2.0 IP | dmesg `HDMI 2.0 driver init` |
| Wi-Fi/BT | Realtek **RTL8821CS** (SDIO on mmc2/sdc1) + BT over UART | dmesg `RTW: rtl8821cs` |
| Storage | **No eMMC / no SPI flash.** TF1 = `sdc0` → `mmcblk0` (OS, boots from here), TF2 = `sdc2` (card-detect GPIO) | dmesg `sdc0: detmode:alway in` |
| Lid | Hall sensor (sleep on close) | dmesg `hall_switch_isr` |
| Inputs | GPIO buttons (BSP `muOS-Keys` gpio_keys + `dierct-keys-polled`), no analog sticks on SP | dmesg `input: muOS-Keys` |
| USB | 1x USB-C OTG (musb), EHCI/OHCI hosts 1-3 | dmesg |
| Recovery | **FEL** mode (USB id `1f3a:efe8`) when no bootable card present → `sunxi-fel` | linux-sunxi |

## SD card layout (stock / muOS, BSP boot chain)

| Offset | Content |
|---|---|
| 0 | MBR/GPT |
| 8 KiB | `boot0` (Allwinner SPL, DRAM init) — BROM looks for `eGON.BT0` here |
| 16400 KiB | `boot_package` (vendor U-Boot + DTB) |
| ~36 MiB+ | GPT partitions (below) |

Actual partitions on this card (GPT, read with `lsblk` in a card reader):

| Part | Size | FS | Name | Content |
|---|---|---|---|---|
| p1 | 8 MiB | exfat | `spare` | unused |
| p2 | 32 MiB | vfat `BOOT` | `boot-resource` | `bootlogo.bmp` (640x480 24-bit, shown by vendor U-Boot), battery images |
| p3 | 16 MiB | raw | `env` | U-Boot env |
| p4 | 64 MiB | raw | `boot` | Android `boot.img` (kernel 4.9.170) |
| p5 | 8 GiB | ext4 `ROOTFS` | `rootfs` | muOS 2601.0 "JACARANDA", busybox init ([raw/bsp/inittab](raw/bsp/inittab)), 2.6 GB used |
| p6 | 49.8 GiB | exfat `ROMS` | `roms` | games/saves — what MTP exposes as `SD1` |

muOS keeps its copy of the vendor boot package in `/opt/muos/device/package/` (`boot_package.fex`, `u-boot.fex`, `monitor.fex` = BL31, `sunxi.fex` = DTB, decompiled `sunxi.dts`) — copied to [raw/bsp/](raw/bsp/).

## SD card layout (ours, mainline boot chain)

Same as ROCKNIX/Knulli-mainline — no vendor blobs at all:

| Offset | Content |
|---|---|
| 8 KiB | `u-boot-sunxi-with-spl.bin` (mainline SPL does DRAM init + TF-A BL31 + U-Boot) |
| 4 MiB | p1 vfat `boot`: `Image`, `*.dtb`, `extlinux/extlinux.conf` |
| next | p2 ext4 rootfs |

The BROM is fixed silicon: it only cares about the `eGON.BT0` header at 8 KiB, so the mainline SPL just replaces boot0.

Sources: [linux-sunxi RG35XX Plus](https://linux-sunxi.org/Anbernic_RG35XX_Plus),
[RG35XXSP CFW gist](https://gist.github.com/MrJackSpade/faa9c284a1592c8d956b7acff7c9222b),
[ROCKNIX H700 device](https://github.com/ROCKNIX/distribution/tree/next/projects/ROCKNIX/devices/H700),
[mainline dts](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/allwinner/sun50i-h700-anbernic-rg35xx-plus.dts).
