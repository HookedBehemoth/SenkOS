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
| `sysd` | lid close, the power key let go within 1 s (held longer, the launcher shows Sleep / Reboot / Power off) or the launcher's `suspend` request (Sleep) → `/usr/sbin/suspend` (`echo mem > /sys/power/state`). Stays asleep while the lid is closed; ignores the power-key press that woke it, and a `suspend` request within 1.5 s of waking. Reacts only to events, so a misread lid at boot can't suspend. Locks the VT (`VT_LOCKSWITCH`) meanwhile: with no framebuffer (fbdev emulation off) the kernel would switch to a text console for every sleep, and seatd would take the display from Weston and give it back |
| `senkos.init` | rebinds the USB gadget when musb comes back detached |
| launcher ([idle.c](../apps/launcher/idle.c)) | idle: dims the backlight to a third, turns the screen off (Weston DPMS through `senkos_shell_v1.set_screen`, which also powers the backlight down), asks sysd to sleep; each after its time in Settings → Screen and sleep (`OS/launcher/config/launcher-idle`: `DIM OFF SLEEP MUSIC`, seconds, 0 never). A key (pad, volume, power) wakes the screen and also reaches the app. A shown surface with a `zwp_idle_inhibitor_v1` (the shell implements the protocol; mediaplayer while a video plays) keeps it all away; SDL apps don't inhibit (`SDL_VIDEO_ALLOW_SCREENSAVER=1`). Music (an MPRIS player Playing, with "Music keeps it awake"): no idle sleep, and sysd's `SetSleepHold(lid)` makes a closed lid turn the screen off instead; once the music stops the hold goes, and sysd sleeps if the lid is closed. While the screen is off `SetSleepHold(power_key)` makes the power key turn it on instead of sleeping. Holds go when the launcher leaves the bus |

Wake sources: power key (PMIC), lid (Hall GPIO), RTC. Not the charger: `90-wake.rules` turns off the `axp20x-usb` supply's wakeup, which would wake on every plug-in.

Check: `cat /sys/power/mem_sleep` shows `[deep]`, and `dmesg` has `PM: suspend entry (deep)`.

## Drain

Our deep sleep drew about 4 %/h (60 % in 16.5 h asleep, no wake-ups in between), roughly 130 mA from the
3300 mAh battery. ROCKNIX reports 2.5-3 %/h with the same stub and 3.5 %/h with s2idle: gating clocks saves
little, because every SoC rail stays up and leaks. The AXP717 can't measure discharge current (`current_now` is
the charge-current ADC; on battery it reads a meaningless ~400 mA), so measure with a USB-C meter on a full battery.

Super standby (below): about 0.5 %/h by the fuel gauge (9 % in 17 h, 2026-10-08), roughly 17 mA. Not yet measured
with a meter.

## Super standby (stock)

The stock firmware cuts the SoC's power and keeps only DRAM in self-refresh. Decompiled from the stock BL31
(`monitor.fex`, AArch64, ATF 1.x, runs at `0x48000000`; Ghidra project `/mnt/multimedia/source/re-h700`, programs
`sp-monitor.bin` and `sp-standby.bin`).

**Mode choice (BL31).** The kernel registers wake sources by SMC (`arisc_set_wakeup_source`: a GIC IRQ bitmap, or
bits 31:30 = 3 for a wake timer). `standby_type` always has bit 1 (super standby), plus bit 0 (normal standby,
WFI with the SoC powered) as soon as any wake IRQ is set other than GIC IDs 103/104, or a wake timer. So stock
only powers off when nothing but the PMIC can wake it. The dtb's `standby_param` (`universal-standby = 1`,
`vdd-cpua = 0x1`, `vdd-sys = 0x2`, `vcc-io = 0x4000`, `vcc-pll = 0`, `vcc-dram = 0`) lists the rails to cut, as
AXP717 regulator bit numbers.

**Entry (BL31 `psci_system_suspend`).** After the generic PSCI context save, BL31 copies an embedded standby
program (BL31 + `0xc260`, `0x9410` bytes, AArch64) to SRAM A1 `0x20000`, the DRAM parameters boot0 handed it
(BL31 + `0x30`) over its start, the `standby_param` values to `0x20080`, sets the resume entry (`0x200c4` = BL31's
warm-boot entry `0x48015d54`) and `0x07000104` (the DRAM backup boot0 restores), and jumps to `0x201b8`.

**Standby program, super standby path:**
1. Sum the DRAM range the kernel gave by SMC (32-bit words), DRAM into self-refresh.
2. Bus clocks (CCU `0x1510`, R_PRCM `0x000`/`0x010`) to their low-power source, RTC `0x160` bit 31 set, bit 1 clear,
   PLL LDO key `0xa7` in R_PRCM `0x244`.
3. RTC registers for boot0: `0x1f8` = `0x10000` (flag), `0x1fc` = resume entry, `0x118`/`0x11c` = sum range,
   `0x10c` = sum, `0x100` = `0x76543210 | mode`. (Also read by boot0: `0x1f4` bit 0 = DRAM pad hold, which our
   stub releases on resume too; `0x108` = `0x5AA5A55A` early jump to FEL, `0x5AA55AA5` crashdump → FEL. The
   standby program writes progress codes to `0x108` as it goes.)
4. AXP717 (identified by reg `0x0e` == 2): `0x48`-`0x4c` = `0xff` (clear IRQs), `0x25` = `0x2d`,
   `0x28`/`0x29`/`0x2a` = the rail mask (stock: `0x03`, `0x40`, `0x00` = DCDC1, DCDC2, CLDO3), `0x25` = `0x6d`. Spin.

AXP717 `0x25` (datasheet): bit 0 sleep, bit 2 restore the set voltages on wake (not the defaults), bit 5 wake on
IRQ pin low, bit 6 auto sleep (cut the rails in maps `0x28`-`0x2a`: DCDC1-4, ALDO1-4 / BLDO1-4, CLDO1-4 /
CPUSLDO, green mode). Any enabled PMIC IRQ (power key, VBUS) pulls the IRQ pin low; the rails come back in
startup order and the SoC cold-boots through the BROM.

**Resume.** boot0 sees `0x1f8` bit 16, clears it, inits DRAM without clearing it, checks the sum ("dram crc
error" hangs), restores the first `0x280` bytes from the `0x104` copy and jumps to `0x1fc` = BL31's warm entry,
which resumes like a CPU coming out of PSCI suspend and returns to the kernel.

What stays on in stock's super standby: DCDC3 (DRAM), BLDO2 (PLL), CPUSLDO (the RTC/R_PRCM domain), and whatever
Linux left on. DCDC1, DCDC2 and CLDO3 (all GPIO banks and the SD cards) are off.

## Super standby (ours)

Which sleep (`/usr/sbin/suspend`, called by sysd): TF-A picks super standby when no pin interrupt is armed
as a wake source and no SoC RTC alarm is set, like stock. The lid is a SoC GPIO and can't wake a powered-off
SoC, so:
- Lid open (power key, the launcher's Sleep): the lid's wakeup is turned off, super standby. The power key wakes.
- Lid just closed: lid wakeup on and a 30 min alarm on the SoC RTC (`rtc1`; `rtc0` is the pcf8563): a deep sleep
  the lid can end. If the alarm comes first, sysd (lid still closed) suspends again without them: super standby.

| Where | What |
|---|---|
| TF-A `0002` | decides, arms the AXP717 (IRQs: power key only; maps `0x28`/`0x29` = DCDC1, DCDC2, DCDC4 / CLDO3; `0x25` = `0x2d`), passes `poweroff` and a DRAM buffer to the stub. On the warm entry after a power cut: the other cores' entry points (RVBAR), GIC and security setup again, PMIC IRQs and sleep registers back |
| stub `stub-0001` | the words PHY training overwrites to that buffer and the sum of BL31's 256 KiB, self-refresh, the clocks down as for WFI plus stock's super standby step (AHB and R_PRCM buses to the 16 MHz RC oscillator, RTC `0x160`: the 24 MHz oscillator on the hardware's request only), RTC handshake, `0x25` = `0x6d` over R_I2C. Still powered 2-3 s later (counted in RTC seconds): clocks back, handshake cleared, WFI as before |
| U-Boot `0009` | SPL: RTC magic set → DRAM init in resume mode (no SDRAM init, no size probe), the saved words back, BL31's sum checked (mismatch: boot instead, like stock's "dram crc error" but without the hang), jump to the warm entry. Every cold boot opens the RTC bus gate (R_PRCM `0x20c`), releases the DRAM pad hold and puts RTC `0x160` and the R_PRCM bus clocks back |
| Linux `0044`-`0051` | `sun50i_h616_standby.c`: saves the CCU (with the MBUS master gates at `0x804`: without them DMA, so audio, stalls), R_CCU and the PPU power gates, the NMI controller (sun9i layout: `0x04` enable, `0x08` pending), pin controllers, system control, DMA, timers/watchdog and PWM at the end of suspend and writes back what differs; the rest in the drivers: IOMMU, thermal sensors, DE33 mixer and plane mapping, DE33 clocks, TCON TOP, TCON dot clock, musb PHY |

RTC handshake (general purpose registers, `0x07000100`): `0x100` = `SSTB` (stub: resume pending) → `SSTR` (SPL:
resumed) → `SSTL` (TF-A: Linux, restore) → 0. `0x104` warm entry, `0x114` DRAM config, `0x118` the buffer, `0x11c` the sum. SPL replaces
`SSTB` with `SSTI` before trying, so a resume that hangs is followed by a cold boot. When it gives up (DRAM
init failed: `SSTD`; BL31's sum differs: `SSTC`) it has the AXP717 restart (REG27 bit 1: rails off and on,
its registers reset) rather than boot on: the rails Linux switched off to suspend would still be off, and
U-Boot would misread the PE6 panel strap (a black screen). At the next boot `senkos.init` reports any of
`SSTI`, `SSTR`, `SSTL`, `SSTD`, `SSTC` (kernel log, `/run/sleep-failed`) and clears it; the launcher then
offers light sleep: `/data/OS/config/light-sleep` (sysd's `sleep light|deep`, Settings → Debug → Deep
sleep) keeps the lid armed in `suspend`, so TF-A never picks super standby.

DRAM pad hold, RTC `0x1f4` bit 0 (`VDDOFF_GATING_SOF`): **clear holds the pads, set releases them**, the opposite
of the H616 manual. Clearing it with DRAM running hangs the SoC; left clear, DRAM init fails on every boot (SPL
panics, the board resets every ~0.5 s) until something sets it again. It lives in the RTC domain, so it survives
resets and power-offs; only removing the battery (USB unplugged) resets it. SPL sets it on every cold boot.

Verified 2026-10-07: same session after waking, all cores, Wi-Fi associated again, games with sound, sleep after
sleep, lid sleep woken by opening the lid or by the RTC alarm. A register dump of a fresh boot and one after
super standby (launcher idle) differ only in live counters and IOMMU invalidation scratch.
