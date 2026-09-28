# Yocto build

Everything lives in [`../yocto/`](../yocto/). The build runs in a container via [kas](https://kas.readthedocs.io) because the host (CachyOS, Python 3.14) is not a supported Yocto host.

```
cd yocto
./kas build rg35xx-sp.yml     # full build
./kas shell rg35xx-sp.yml     # bitbake shell
```

`./kas` wraps `kas-container` (podman) and puts `build/`, `downloads/`, `sstate-cache/` in **`/mnt/multimedia/anbernic-yocto`** (override with `ANBERNIC_BUILD_ROOT`). They can't live in /home: its btrfs has all space allocated to data chunks, so Yocto's millions of small files exhaust *metadata* space and every write in /home fails with ENOSPC while `df` still shows free space (happened 2026-09-28). Permanent fix for /home: `sudo btrfs balance start -dusage=50 /home`.

Output: `/mnt/multimedia/anbernic-yocto/build/tmp/deploy/images/rg35xx-sp/anbernic-image-rg35xx-sp.rootfs.wic.zst` (+ `.wic.bmap`).

## Layers

| Layer | Branch | Why |
|---|---|---|
| bitbake | 2.18 | |
| openembedded-core | wrynose (6.0 LTS) | U-Boot 2026.01, toolchain |
| meta-arm | wrynose | TF-A 2.14.1 (`sun50i_h616` BL31) |
| **meta-anbernic** | ours | machine, kernel, U-Boot tweaks, wks |

## meta-anbernic

```
conf/machine/rg35xx-sp.conf              machine: cortex-a53, dtbs, u-boot defconfig, wic
conf/distro/anbernic.conf                our distro (no poky): busybox+mdev, 6 DISTRO_FEATURES, no SPDX, no python in build graph
recipes-kernel/linux/linux-anbernic_7.2.bb   kernel.org 7.2 + 29 patches
recipes-kernel/linux/files/defconfig     ROCKNIX H700 config (initramfs off, firmware built-in)
recipes-kernel/linux/files/firmware/     panel init blobs + rtw88/rtl_bt fw → CONFIG_EXTRA_FIRMWARE
recipes-bsp/u-boot/                      BL31 into SPL, BOOTDELAY=0
recipes-bsp/trusted-firmware-a/          COMPATIBLE_MACHINE only
recipes-core/images/anbernic-image.bb    the image (busybox only)
recipes-devtools/opkg-utils/             drop bash RDEPENDS
files/wic/rg35xx-sp.wks                  SPL @8K, vfat boot, ext4 root (msdos, diskid 0x52473335)
files/wic/rg35xx-sp-extlinux.conf        kernel/fdt/cmdline
```

Kernel patches: ROCKNIX `devices/H700/patches/linux` applied on vanilla 7.2 and re-exported with `git format-patch` so they apply without fuzz. Skipped `0140`, `0144`, `0150` (need ROCKNIX's out-of-tree `rocknix-singleadc-joypad` driver — we keep mainline `gpio-keys`).
`0029` adds ROCKNIX's `sun50i-h700-anbernic-rg35xx-sp-v2-panel.dts`.

## Keeping it small

No Poky (Yocto's reference/demo distro). `anbernic.conf` sits on plain oe-core defaults and trims:
- `DISTRO_FEATURES_DEFAULTS = "ext2 vfat ipv4 ipv6 wifi alsa"`: oe-core's default adds ptest (dejagnu/expect/tcl), debuginfod (curl/gnutls), x11, wayland, vulkan, pulseaudio, nfs, 3g, nfc…
- `PACKAGECONFIG:pn-opkg-utils = "update-alternatives"`: every recipe build-depends on target opkg-utils, whose `python` config pulled python3 → openssl → perl, sqlite3, ca-certificates.
- `VIRTUAL-RUNTIME_update-alternatives = ""`, `INHERIT_DISTRO:remove = "create-spdx"`.

Image: our own `recipes-core/images/anbernic-image.bb` instead of `core-image-minimal`: base-files, base-passwd, busybox (init + mdev + shell), initscripts, inittab. `ROOTFS_BOOTSTRAP_INSTALL = ""` drops run-postinsts/util-linux, and an `opkg-utils` bbappend drops bash (its opkg-* scripts are never installed).

Result: build graph 150 → 128 recipes; on-device recipes about 100 → **18** (kernel, U-Boot, TF-A, glibc, gcc-runtime, busybox, initscripts, base files). The other 109 are the cross toolchain and host build tools (cached in sstate after the first build). Inspect with `bitbake -g anbernic-image`.

## Flashing

**Use a spare SD card** — the muOS card holds your saves. Or back it up first:
```
sudo dd if=/dev/sdX of=muos-backup.img bs=4M status=progress
```
Then:
```
sudo bmaptool copy /mnt/multimedia/anbernic-yocto/build/tmp/deploy/images/rg35xx-sp/anbernic-image-rg35xx-sp.rootfs.wic.zst /dev/sdX
```
Put it in **TF1**. Nothing on the device is modified; pull the card and muOS boots again.

## If it doesn't boot

- Black screen, no LED activity → SPL/DRAM. Try the ROCKNIX LPDDR3 defconfig or check UART.
- Backlight on, no picture → wrong panel: edit `extlinux/extlinux.conf` on the vfat partition to use `sun50i-h700-anbernic-rg35xx-sp-v2-panel.dtb`.
- UART: 115200 8N1 on the board test pads (`console=ttyS0`).

## Status

**2026-09-28: first successful boot** of `anbernic-image` from TF1: mainline SPL (LPDDR4 @ 672 MHz) → TF-A 2.14 → U-Boot 2026.01 → Linux 7.2 + ROCKNIX patches → busybox. LCD console works with `sun50i-h700-anbernic-rg35xx-sp.dtb` (original panel, not v2), with the 4-penguin boot logo; USB keyboard works. Root login: bring-up image features `empty-root-password allow-empty-password allow-root-login` (remove before shipping). U-Boot also carries `bringup-debug.cfg` (red LED + `uboot.env` boot counter), and the kernel cmdline has `panic=10`.
