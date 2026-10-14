# Yocto build

Everything lives in [`../yocto/`](../yocto/). The build runs in a container via [kas](https://kas.readthedocs.io),
because the host (Arch, Python 3.14) isn't a supported Yocto host.

```
cd yocto
./kas build rg35xx-sp.yml                     # image
./kas build --target apps rg35xx-sp.yml       # one recipe
./kas shell rg35xx-sp.yml                     # bitbake shell
```

`./kas` wraps `kas-container` (podman) and puts `build/`, `downloads/` and `sstate-cache/` in
`/mnt/multimedia/anbernic-yocto` (override with `ANBERNIC_BUILD_ROOT`). They can't live in /home: its btrfs
runs out of metadata space under Yocto's many small files.

Output in `build/tmp/deploy/images/rg35xx-sp/`:
- `senkos-image-rg35xx-sp.rootfs.wic.gz` (+ `.wic.bmap`): a whole card
- `senkos-image-rg35xx-sp.rootfs.squashfs-lz4`: one slot, what `tools/ota` flashes

## Layers

| Layer | Branch | For |
|---|---|---|
| bitbake | 2.18 | |
| openembedded-core | wrynose | toolchain, U-Boot, most packages |
| meta-arm | wrynose | TF-A |
| **meta-anbernic** | ours | BSP: the H700 and Anbernic's boards |
| **meta-senkos** | ours | the distro |

meta-anbernic, what any distro needs to boot the board:
```
conf/machine/include/h700.inc      SoC: cortexa53-crypto tune, TF-A, U-Boot/SPL, kernel, Mesa's GPU driver
conf/machine/rg35xx-sp.conf        board: dtbs, U-Boot defconfig, consoles, Wi-Fi
recipes-bsp/                       U-Boot 2026.07 (sunxi/H616 patches: DRAM straps, super standby, off on a plug-in), TF-A (+ suspend stub)
recipes-kernel/linux/              linux-anbernic: Linux 7.2 + patches, savedefconfig, built-in firmware
recipes-graphics/mesa/             Mesa main snapshot + Mali-G31 fixes
```

meta-senkos, the distro, also its policy on the BSP's recipes (bbappends; kernel ones in
dynamic-layers/anbernic, read only with meta-anbernic):
```
conf/distro/senkos.conf            no poky: busybox init + eudev, trimmed DISTRO_FEATURES and PACKAGECONFIGs
files/wic/senkos.wks.in            card layout (SPL, FAT, two slots)
recipes-bsp/u-boot/                A/B boot.scr, splash, silent/fast boot, watchdog, squashfs
dynamic-layers/anbernic/           kernel: boot splash client, battery name
recipes-core/images/               senkos-image (image types, card layout)
recipes-core/senkos-base/          init glue: USB gadget, A/B slots + /data, logs, watchdog, Weston, SD automount
recipes-core/sysd/                 root's side of the UI: suspend, the launcher's requests
recipes-graphics/                  launcher and Weston's shell (apps/), Weston trims, Mesa build flags, SDL2/vulkan-tools tweaks
recipes-emulators/, recipes-games/ libretro cores (in the image)
```

## Flashing a card

```
sudo bmaptool copy …/senkos-image-rg35xx-sp.rootfs.wic.gz /dev/sdX
```
Put it in **TF1**. The first boot creates `/data` from the rest of the card. After that, update over USB
([dev-workflow.md](dev-workflow.md)).

## If it doesn't boot

- No LED, no screen: SPL/DRAM. The device may also be wedged from FEL experiments (see [fel.md](fel.md)).
- Backlight but no picture: the unit has the other panel. `boot.cmd` loads `sun50i-h700-anbernic-rg35xx-sp.dtb`;
  the slots also carry `…-rg35xx-sp-v2-panel.dtb`.
- UART: 115200 8N1 on the board's test pads.
