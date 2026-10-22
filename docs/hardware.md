# RG35XX-SP hardware

| Part | What | In Linux |
|---|---|---|
| SoC | Allwinner **H700** (H616 family), 4x Cortex-A53 with the ARMv8 crypto extensions | DT `sun50i-h700-anbernic-rg35xx-sp` |
| RAM | 1 GB **LPDDR4** @ 672 MHz (DDR3/LPDDR3 units exist and need a different SPL config; the type is strapped, see [straps.md](straps.md)) | |
| GPU | Mali-G31 MP2 (Bifrost) | panfrost (GLES 3.1, GL 3.1), PanVK (Vulkan, non-conformant on G31) |
| Video decode | Cedar VE: H.264, H.265, VP8, VP9, MPEG-2; 10-bit H.265 and VP9 as P010 | Cedrus, V4L2 stateless |
| PMIC | X-Powers **AXP717**, i2c @0x34 | `axp20x-pek` power key, `axp20x-usb`, battery |
| RTC | NXP **PCF8563**, i2c @0x51 | `rtc0` |
| Display | 3.5" 640x480 IPS, parallel RGB, initialised over SPI. Two panel revisions ("v2 panel" on newer units), strapped on PE6 ([straps.md](straps.md)) | `panel-mipi-dpi-spi`, init sequence built into the kernel; one dtb per panel |
| HDMI | mini-HDMI | DW HDMI + HDMI audio |
| Audio | SoC codec, speaker amp on Line Out, headphone jack | `H616 Audio Codec` |
| Wi-Fi/BT | Realtek **RTL8821CS**, SDIO + BT over UART | `rtl8821cs` (vendor), `btrtl` (firmware built in) |
| Storage | No eMMC or SPI flash. TF1 boots, TF2 has card detect | TF1 `mmcblk0`, TF2 `mmcblk1` |
| Lid | Hall sensor on a SoC GPIO | `SW_LID` (gpio-keys) |
| Buttons | D-pad, A/B/X/Y, L1/R1/L2/R2, Select/Start/Menu, volume; no sticks | `Anbernic Gamepad` (our `anbernic-gamepad` driver: every board's buttons and sticks as one device, the same buttons on all), `gpio-keys-volume` |
| USB | 1x USB-C OTG, ID pin on PI4 | musb: gadget to a PC, host through an OTG adapter |
| LEDs | red status LED (PI11), RGB | gpio-leds |
| UART | test pads on the board, 115200 8N1 | `ttyS0` |
| Recovery | BROM **FEL** mode when TF1 isn't bootable | see [fel.md](fel.md) |

The BROM loads the SPL from the `eGON.BT0` header at 8 KiB on TF1; nothing else about the card layout is
fixed by the hardware. Our layout is in [dev-workflow.md](dev-workflow.md#ab-slots).
