# Suspend on the H700: why custom firmware gets it wrong

TL;DR: your observation is correct for mainline-based CFW (ROCKNIX, Knulli-mainline) **until 2026-09-17**. Mainline TF-A had no PSCI `SYSTEM_SUSPEND` for H616/H700, so "sleep" was fake. A fix landed in ROCKNIX `next` 11 days ago and is still young. BSP-based CFW (muOS) does real suspend via Anbernic's blobs but adds its own policy.

## Why suspend is hard on this SoC

Linux can't put DRAM into self-refresh itself: code running *from* DRAM can't turn DRAM off. On arm64 that job belongs to the secure firmware (BL31) via PSCI `SYSTEM_SUSPEND`, which runs a small program from on-chip SRAM that:
1. puts DRAM into self-refresh, stops DDR PLL/controller/PHY clocks,
2. drops CPU clocks, waits in `WFI` for a wake IRQ (power key, lid/Hall, RTC),
3. **re-initialises the DRAM controller without wiping contents** (retrains PHY) and returns to Linux.

Step 3 needs the same board-specific DRAM parameters the SPL used at boot. Older sunxi SoCs (A64, H6) do this with **Crust** firmware on the AR100 management core — **the H616/H700 has no AR100**, so Crust can't help and there was no mainline path at all.

## Who does what

| Firmware | Kernel | What "sleep" actually is | Drain |
|---|---|---|---|
| **Stock Anbernic** | BSP 4.9 | Real `mem` → vendor BL31 (`monitor.fex`) + SRAM standby program. Two modes: *Normal* (lid/key wake, WFI + DRAM self-refresh) and *Super* (additionally PMIC sleep, rails cut, **boot0 restores DRAM and jumps back into the retained Linux session** via an RTC marker). | lowest (Super), unmeasured publicly |
| **muOS** (your card) | same BSP 4.9 + same vendor BL31 | Real `mem` (`/opt/muos/config/danger/state = mem`), but wrapped in a 618-line shell script ([raw/muos-init/suspend.sh](raw/muos-init/suspend.sh)): stops network + BT, **unloads/reloads kernel modules** (wifi, mali), rebinds input drivers, then **after `settings/power/shutdown = 600` s asleep it RTC-wakes and powers off**. Never sets the stock `os_sleep` knob → never reaches *Super* standby. | Normal-standby level, then off after 10 min |
| **ROCKNIX / Knulli (mainline) before 2026-09-17** | 6.x/7.x | Kernel offers only **s2idle** ("freeze"): CPUs idle, every rail/clock/DRAM stays up. For a while H700 even forced `suspendmode off` + ignored the power key (fake suspend = screen off, emulator paused). | **~3.5 %/h** → dead in ~a day |
| **ROCKNIX `next` since 2026-09-17** (commit `fca6ac51`) | 7.2 | Real `deep`: patched TF-A 2.15 + [h700-suspend-stub](https://github.com/Jacob-Matthew-Cook/h700-suspend-stub) (by kailashrs) in SRAM A1: LPDDR4/LPDDR3 self-refresh, WFI, DRAM re-init from U-Boot's own `dram_sun50i_h616.c`. Plus kernel PM fixes (display pipeline, codec, LCD pads, USB PHY clock, power-key wake event, MMC). | **~3 %/h** (commit message) |

## Why "only the official firmware does it right"

1. **Mainline had no deep sleep at all** (no AR100 → no Crust; TF-A lacked H616 `SYSTEM_SUSPEND`). This is the big one.
2. **Even the new mainline port is not OEM "Super standby".** The kailashrs analysis of stock boot0/BL31 ([OEM_STANDBY.md](https://github.com/kailashrs/H700_rocknix_enhancement/blob/main/docs/OEM_STANDBY.md)) shows OEM Super mode also cuts PMIC rails (AXP717 regs `0x25`, `0x28..0x2a`) and has **boot0 resume retained RAM** after a PMIC-level sleep. That's why stock drains less. Not ported yet; it would need SPL changes too.
3. **Peripheral PM on mainline was incomplete**: display pipeline/LCD pads not gated, codec not suspended, a leaked USB PHY clock, the wake keypress re-triggering sleep (logind), `rtw88` SDIO Wi-Fi flaky after resume.
4. **Userspace policy fights**: fake-suspend daemons + real logind suspend running at once (UI freeze / no sound — documented in kailashrs' [VALIDATION.md](https://github.com/kailashrs/H700_rocknix_enhancement/blob/main/docs/VALIDATION.md)); muOS's auto-shutdown after 10 min.

## Status of the new port (honest limits, from its own docs)

- Tested on RG35XX H and RG34XX/RG34XX-SP (LPDDR4); **not RG35XX-SP specifically**. LPDDR3 stub built but never run on hardware (TF-A refuses suspend rather than risk it).
- No overnight qualification, no measured stock-vs-port drain comparison.
- Occasional Wi-Fi not reconnecting after resume.
- It implements OEM *Normal* standby behaviour, not *Super*.

## What this means for our OS

Our kernel already carries the kernel half (patches `0157`–`0163`, `0210` from ROCKNIX). Missing is the firmware half:

1. Bump TF-A to 2.15.0 (bbappend) and add ROCKNIX `atf/patches/H700/001-allwinner-h616-psci-system-suspend.patch` + `000-use-gcc-for-asm.patch`.
2. New recipe `h700-suspend-stub`: build against our U-Boot source tree + defconfig (`anbernic_rg35xx_h700_defconfig`, LPDDR4) → `suspend_stub_lpddr4.bin`; TF-A built with `SUNXI_SYSTEM_SUSPEND=1 SUNXI_SUSPEND_STUB=<path>`.
3. Userspace: no logind/fake-suspend daemon. Our launcher writes `mem` to `/sys/power/state` on lid-close / power key; enable wake on power key, Hall and RTC; drop the rtw88 interface before sleep and bring it back after.
4. Verify: `cat /sys/power/mem_sleep` shows `s2idle [deep]`; `dmesg | grep "PM: suspend entry (deep)"`; measure drain overnight with `/sys/class/power_supply/*/capacity`.
5. Stretch goal: OEM Super standby (PMIC sleep + SPL retained-RAM resume). This is the only route to stock-level drain and nobody has done it in open source yet.

Sources: ROCKNIX commit [`fca6ac51` "H700: firmware deep sleep"](https://github.com/ROCKNIX/distribution/commit/fca6ac51),
[h700-suspend-stub](https://github.com/Jacob-Matthew-Cook/h700-suspend-stub),
[kailashrs/H700_rocknix_enhancement](https://github.com/kailashrs/H700_rocknix_enhancement) (DESIGN/OEM_STANDBY/VALIDATION docs),
[MustardOS suspend.sh](https://github.com/MustardOS/internal/blob/main/script/system/suspend.sh), muOS config on the card.
