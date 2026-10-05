# Suspend to RAM

Linux can't put DRAM into self-refresh itself (it runs from DRAM), and the H616/H700 has no management
core for firmware like Crust. So the secure firmware does it: TF-A's PSCI `SYSTEM_SUSPEND` runs a small
stub from SRAM that
1. puts DRAM into self-refresh and stops the DRAM clocks,
2. waits in `WFI` for a wake interrupt,
3. retrains the DRAM controller without touching its contents and returns to Linux.

Step 3 needs the exact DRAM parameters the SPL used, so the stub is built from U-Boot's own DRAM driver
and our U-Boot defconfig ([TF-A bbappend](../yocto/meta-anbernic/recipes-bsp/trusted-firmware-a/trusted-firmware-a_%.bbappend)).
BL31 refuses to suspend if the parameters don't match at runtime. The stub also pauses the watchdog.

## Pieces

| Where | What |
|---|---|
| TF-A | `SYSTEM_SUSPEND` patch + [h700-suspend-stub](https://github.com/Jacob-Matthew-Cook/h700-suspend-stub) (LPDDR4) |
| Kernel | PM fixes for the power-key wake event, MMC, the TF2 slot power, the USB PHY clock, the display pipeline and LCD pads, the codec, and a suspend OPP (patches 0018-0024, 0027) |
| `sleepd` | lid close, power key or `suspend` written to `/run/power` (the launcher's Sleep; `reboot`, `poweroff` too; and the Power page's `cpu KHZ`, `gpu HZ`, `brightness N`) → `/usr/sbin/suspend` (`echo mem > /sys/power/state`). Stays asleep while the lid is closed; ignores the power-key press that woke it. Reacts only to events, so a misread lid at boot can't suspend. Locks the VT (`VT_LOCKSWITCH`) meanwhile: with no framebuffer (fbdev emulation off) the kernel would switch to a text console for every sleep, seatd would take the display from Weston and give it back, and a few sleeps in a row left Weston without frames |
| `anbernic.init` | rebinds the USB gadget when musb comes back detached |

Wake sources: power key (PMIC), lid (Hall GPIO), RTC. Not the charger: `90-wake.rules` turns off the `axp20x-usb` supply's wakeup, which would wake on every plug-in.

Check: `cat /sys/power/mem_sleep` shows `[deep]`, and `dmesg` has `PM: suspend entry (deep)`.

## Later: Super standby (PMIC-level)

The stock firmware has a deeper mode: the AXP717 cuts every rail except DRAM (regs `0x25`, `0x28`..`0x2a`)
and the **SoC really powers off**. Wake-up is a cold boot through the BROM; boot0 re-inits DRAM without
clearing it, checks a RAM sum and jumps back into BL31, which restores the SoC and returns to Linux.
It draws less than our deep sleep.

What the stock boot0 (eGON.BT0 at 8 KiB, AArch32 Thumb, loaded at `0x20000`) reads on wake:

| RTC register (`0x07000000 +`) | Meaning |
|---|---|
| `0x1f8` bit 16 | super standby flag; boot0 clears the register |
| `0x100` (GP0) | `0x7654321x`: magic in the upper 28 bits, low nibble = mode |
| `0x118`/`0x11c` (GP6/GP7) | start / length of a DRAM range boot0 sums (32-bit words) after DRAM init |
| `0x10c` (GP3) | expected sum; a mismatch hangs ("dram crc error") |
| `0x1fc` | resume address, jumped to instead of loading U-Boot |
| `0x1f4` bit 0 | DRAM pad hold (retention); our suspend stub releases it on resume too |
| `0x108` (GP2) | not standby: `0x5AA5A55A` = early jump to FEL, `0x5AA55AA5` = crashdump → FEL |

Doing it on our stack takes three pieces:
1. **SPL**: flag check, DRAM init in resume mode (the stub's `dram-resume.patch` has that path), jump to the BL31 warm entry.
2. **TF-A**: the stub ends with an AXP717 power-down instead of WFI.
3. **Linux**: every driver has to survive a full SoC power loss (CCU, pinctrl, GIC and so on get reset).
   Mainline sunxi drivers mostly don't save and restore that state.

Only PMIC events can wake it: the power key and VBUS. The lid Hall sensor is on a SoC GPIO (PE7), so the
lid can't. Measure deep sleep drain first to see whether it's worth the kernel-wide effort.
