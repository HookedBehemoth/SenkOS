# Plan: our own small OS for the RG35XX-SP

Goal: power-on → game menu in a few seconds, minimal moving parts, reproducible build.

## Decision: mainline, not BSP

The BSP path (keep vendor boot0/boot_package + 4.9 kernel, swap rootfs) is what muOS does — it inherits the slow parts and a 2018 kernel.
Mainline is viable now: **ROCKNIX ships Linux 7.2 + ~30 patches on H700 with working LCD, panfrost GPU, audio, PWM backlight, Wi-Fi.** Real deep suspend only since 2026-09-17 (see [suspend.md](suspend.md)). We reuse their patch set and DTs ([raw/rocknix/](raw/rocknix/) for the U-Boot bits).

| Layer | Choice |
|---|---|
| Build system | Yocto **wrynose** (6.0 LTS) via **kas** in a container (host is Arch/CachyOS + Python 3.14 → not a supported Yocto host) |
| Layers | `openembedded-core`, `bitbake 2.18`, `meta-arm` (TF-A), our `meta-anbernic` |
| Boot | TF-A 2.14 `sun50i_h616` BL31 → mainline **U-Boot 2026.01** `anbernic_rg35xx_h700_defconfig` (LPDDR4) + `BOOTDELAY=0` |
| Kernel | kernel.org **7.2** + ROCKNIX H700 patches, DT `sun50i-h700-anbernic-rg35xx-sp` (or `-v2-panel` for newer units) |
| Rootfs | ext4, busybox + mdev or plain `/sbin/init` script; no systemd, no dbus, no udev daemon |
| Graphics | KMS/DRM + Mesa panfrost (GLES), SDL2 on KMSDRM — no Wayland/X |
| Frontend | later: a small SDL2 launcher (or port muOS's LVGL frontend), RetroArch w/ selected cores |

## Phases

1. **Boot to shell** (current): SD image with SPL/U-Boot, kernel, busybox rootfs. Verify over UART (pads on board, 115200 8N1) or on-screen console (`console=tty0`).
2. **Hardware bring-up check**: LCD + backlight, buttons (`evtest`), audio (`speaker-test`), battery (`/sys/class/power_supply`), lid, Wi-Fi (`rtw88_8821cs` mainline driver + firmware), TF2 slot.
3. **Fast boot**: trim kernel config (only what's in [hardware.md](hardware.md)), everything built-in, `quiet`, measure with `initcall_debug`; ro rootfs (squashfs) + writable data partition → no fsck/journal recovery.
4. **UI**: Mesa panfrost + SDL2 kmsdrm, launcher that starts in <1 s, RetroArch.
5. **Polish**: real deep suspend on lid close (TF-A SYSTEM_SUSPEND + SRAM stub, see [suspend.md](suspend.md)), OTA-free update (just reflash / copy image), ROMS on exFAT partition compatible with muOS layout.

## Risks / unknowns

- **DRAM type**: vendor DT says LPDDR4 @ 672 MHz (see [hardware.md](hardware.md)). A wrong SPL just fails to boot; pull the card and muOS is back.
- **Panel revision** (v1 vs v2): BSP names it `fog_fj035fhd05_v1`; mapping to ROCKNIX v1/v2 unconfirmed, both DTBs ship in the image.
- ROCKNIX patches are tuned to their tree; some depend on their out-of-tree `rocknix-joypad` driver — we skip those and use mainline `gpio-keys`.
- Disk: host has ~49 GB free. Build uses `rm_work`; a minimal image should fit in ~35 GB.

## Next data to collect (needs the SD card in a PC reader, not MTP)

MTP only exposes the exFAT partition. With the card in a reader:
```
sudo dd if=/dev/sdX of=muos-backup.img bs=4M status=progress   # full backup FIRST
sudo dd if=/dev/sdX bs=1024 skip=16400 count=20464 of=docs/raw/boot_package.img
sudo sfdisk -d /dev/sdX > docs/raw/partitions.txt
```
From `boot_package` we extract the vendor DTB → panel + DRAM parameters.
