# FEL: USB boot mode

When TF1 has nothing bootable, the H700's boot ROM falls back to **FEL**: it enumerates on the USB-C port as
`1f3a:efe8` and waits for commands. It's mask ROM, so nothing written to a card can brick the device.

## Tooling

- `sunxi-fel` from [linux-sunxi/sunxi-tools](https://github.com/linux-sunxi/sunxi-tools) master (1.4.2 is too old for the H616).
- Permissions: `sudo cp tools/60-sunxi-fel.rules /etc/udev/rules.d/ && sudo udevadm control --reload`, then replug.

| Use | Command |
|---|---|
| Identify | `sunxi-fel ver` → `soc=00001823(H616)`, `sunxi-fel sid` |
| Boot U-Boot over USB | `sunxi-fel -v uboot u-boot-sunxi-with-spl.bin` |
| Boot a kernel from RAM | `sunxi-fel -v uboot u-boot-sunxi-with-spl.bin write 0x40200000 Image write 0x4fa00000 board.dtb write 0x4fc00000 boot.scr` |
| Write cards through the device | U-Boot `ums 0 mmc 0` (USB mass storage) |

## Limits

- Full speed only (~1 MB/s): a 20 MB kernel takes about 20 s. U-Boot's own gadgets (UMS, fastboot) are high speed.
- `sunxi-fel spl` doesn't return to FEL on this die (`SRAM_VER_REG = 0x3b02`, RVBAR `0x08100040`): the SoC restarts instead, so it can't be used to test DRAM init on its own.
- Without UART you see nothing until the kernel lights up the LCD; U-Boot has no driver for the panel.
- FEL experiments can leave the SoC/PMIC wedged, and card boot then silently falls back to FEL. Unplug USB and disconnect the battery before drawing conclusions.
