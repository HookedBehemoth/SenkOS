# FEL: USB boot mode

When TF1 has no bootable card, the H700's **boot ROM** falls back to **FEL**: it enumerates on the USB-C port as `1f3a:efe8` and waits for commands. It's mask ROM, so it's always there and **the device cannot be bricked** by anything written to an SD card.

Seen on this host 2026-09-28 17:54–17:57 (`journalctl -k`): `idVendor=1f3a, idProduct=efe8` enumerated **full-speed**, and disconnected after about 1–2 s each time (cause unknown: power key? PMIC power-off? cable?).

## Tooling

- `sunxi-fel` from [linux-sunxi/sunxi-tools](https://github.com/linux-sunxi/sunxi-tools) master. The distro package (1.4.2) is too old for H616. Built with a static libfdt → `~/.local/bin/sunxi-fel`.
- Permissions: `sudo cp tools/60-sunxi-fel.rules /etc/udev/rules.d/ && sudo udevadm control --reload`, then replug.

## What it lets us do

| Use | Command | Why it matters |
|---|---|---|
| Identify SoC / chip ID | `sunxi-fel ver`, `sunxi-fel sid` | confirms H616 die (`0x1823`) |
| **Test our SPL/DRAM init with no card at all** | `sunxi-fel spl u-boot-sunxi-with-spl.bin` | proves the LPDDR4 config before flashing anything |
| **Boot mainline U-Boot over USB** | `sunxi-fel -v uboot u-boot-sunxi-with-spl.bin` | U-Boot console on UART |
| **Boot kernel + dtb (+ initramfs) into RAM** | `sunxi-fel -v uboot u-boot-sunxi-with-spl.bin write 0x40200000 Image write 0x4fa00000 board.dtb write 0x4fc00000 boot.scr` | kernel dev loop with no SD writes |
| Flash cards through the device | U-Boot `ums 0 mmc 0` (USB mass-storage gadget) | write TF1/TF2 over USB-C |
| Recovery | always available | nothing we write can lock us out |

## Limits

- **Full-speed only (12 Mbit/s, ~1 MB/s)** as enumerated here. A 20 MB kernel takes about 20 s, which is still fine for bring-up. U-Boot's own gadget (UMS/fastboot) runs at high speed.
- FEL starts only when TF1 is empty or unbootable. There's no known FEL key combo on the RG35XX-SP (unverified).
- Without UART you're blind until the kernel lights up the LCD. U-Boot has no driver for this panel.
- If the unit really powers off after about 1 s in FEL, that's the AXP717 PMIC (nothing holds power). Try keeping USB power connected or holding the power key.

## Test log

**2026-09-28:** device in FEL, udev rule installed.
- `sunxi-fel ver` → `AWUSBFEX soc=00001823(H616)`, SID `33806c00:0c004808:01080c45:248a2212`. FEL is stable when the device is powered on normally (earlier 1–2 s drops were presumably power-button handling).
- `sunxi-fel -v spl u-boot-sunxi-with-spl.bin` (our U-Boot 2026.01, `anbernic_rg35xx_h700_defconfig`, LPDDR4 @ 672 MHz): SPL executes, then USB **disconnects and re-enumerates as FEL**. `sunxi-fel writel 0x40000000 …` afterwards **hangs** (DRAM not accessible).
- Interpretation (unconfirmed, no UART): a 64-bit SPL returns to the 32-bit BROM via RVBAR + RMR warm reset. The H616 has two die variants with different RVBAR addresses (see `aw_rmr_request()` in sunxi-tools `fel.c`). If the return lands in BROM reset instead of FEL resume, the SoC restarts from scratch, so this test **doesn't tell us whether DRAM init works**.
- The DRAM parameters in the mainline defconfig match the vendor DT (`dram_para`: clk 0x2a0, type 8, ODT/DRI, TPR6 0x40808080, TPR10 0x402f6633). ROCKNIX's `dram_sun50i_h616.c` patch only touches LPDDR3.
- Next: SD-card boot (no FEL return involved), or UART to see SPL output.

**2026-09-28, later: it was the device state, not the image.**
- Card boot showed no LED and no screen and fell into FEL on every power-on. FEL probes showed SRAM `0x20000` holding power-on noise: the boot ROM never loaded our SPL. The card's first 4 MiB were byte-identical to the image.
- **Disconnecting the battery fixed it.** The next boot went straight to the LCD console. Most likely our FEL experiments (SPL over FEL, then a hanging DRAM write) left the SoC/PMIC wedged, and with USB power attached a long power-button press didn't fully reset it.
- **Rule:** after FEL experiments, if card boot misbehaves, unplug USB and disconnect the battery before drawing conclusions.
- Die: `SRAM_VER_REG = 0x3b02` → alternative H616 die (RVBAR `0x08100040`). U-Boot's arm64 `return_to_fel` on H616 sets the hotplug flag at `R_CPUCFG + 0x1c0`. Possibly wrong for this die, which would explain why `sunxi-fel spl` can't return to FEL. Unverified.
