# Boot time analysis (muOS, from dmesg)

Raw logs: [raw/dmesg/](raw/dmesg/). muOS writes them from `S99muos.sh` *after* the frontend's first paint, so the kernel ring buffer has already wrapped — every log starts at ~1.05 s, the early kernel is lost.

## Timeline (kernel clock, boot of 2026-09-28 17:30)

| t (s) | Event |
|---|---|
| ?  | BROM → boot0 → vendor U-Boot → boot logo → kernel. **Not visible in dmesg.** Stopwatch it: power-on → first kernel line would need UART. |
| 1.90 | Kernel done, `Freeing unused kernel memory` → init |
| 1.94 | rootfs `mmcblk0p5` mounted (`barrier=0`) |
| 2.56 | eudev starts |
| 3.55 | `mali_kbase` loaded (out-of-tree) |
| 3.85–4.15 | `rtl8821cs` probe **fails** (`-123`, cmd 7 RTO), card re-enumerated, retried → ~0.3 s lost |
| 4.80–5.60 | exFAT ROMS partition mount (0.8 s) |
| 8.4–10.4 | pipewire/audio up, HDMI audio enabled |
| 14.1 | HDMI hotplug config released |
| ~21–23 | dmesg dumped (RTC set at 1.86 s = 17:29:53, log file named 17:30:14) → frontend had painted by then |

## Observations

1. **Kernel is not the problem** — ~1.9 s to userspace is fine for a 4.9 BSP kernel even with dozens of useless drivers (GRE, L2TP, IPsec, pktgen, 15 `ums-*` USB storage drivers, optee probing).
2. **Unclean shutdown costs 4 s.** On 2026-09-27 the rootfs mount took until 5.9 s: `4 orphan inodes deleted / recovery complete`. `barrier=0` makes this worse (and risks corruption on power loss).
3. **Userspace is the bulk**: ~2 s → frontend ≈ 15–20 s. Shell-script init, eudev coldplug, dbus, pipewire, bluetooth, chrony, `muvar build` config cache (timeout 10 s), exFAT mount, all before `FRONTEND start`.
4. **Wi-Fi probe fails once every boot** (SDIO power sequencing) — wasted time + driver churn.
5. Bootloader stage is unmeasured; vendor boot0/U-Boot with logo typically adds 2–4 s.

## Targets for our OS

| Stage | muOS | Goal |
|---|---|---|
| BROM → kernel | ~3 s? | < 1 s (mainline SPL, `bootdelay=0`, no logo, no env probing) |
| kernel → init | 1.9 s | < 1.5 s (trimmed config, built-in drivers, no initramfs) |
| init → UI | ~15–20 s | < 1 s (single static init that launches the frontend directly; everything else lazy) |

Measure with `initcall_debug` / `printk.time=1` on UART, and `bootchart`-style timestamps in our init.
