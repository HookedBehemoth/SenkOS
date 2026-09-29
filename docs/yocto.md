# Yocto build

Everything lives in [`../yocto/`](../yocto/). The build runs in a container via [kas](https://kas.readthedocs.io),
because the host (Arch, Python 3.14) isn't a supported Yocto host.

```
cd yocto
./kas build rg35xx-sp.yml                     # image
./kas build --target retroarch rg35xx-sp.yml  # one recipe
./kas shell rg35xx-sp.yml                     # bitbake shell
```

`./kas` wraps `kas-container` (podman) and puts `build/`, `downloads/` and `sstate-cache/` in
`/mnt/multimedia/anbernic-yocto` (override with `ANBERNIC_BUILD_ROOT`). They can't live in /home: its btrfs
runs out of metadata space under Yocto's many small files.

Output in `build/tmp/deploy/images/rg35xx-sp/`:
- `anbernic-image-rg35xx-sp.rootfs.wic.zst` (+ `.wic.bmap`): a whole card
- `anbernic-image-rg35xx-sp.rootfs.squashfs-zst`: one slot, what `tools/ota` flashes

## Layers

| Layer | Branch | For |
|---|---|---|
| bitbake | 2.18 | |
| openembedded-core | wrynose | toolchain, U-Boot, most packages |
| meta-arm | wrynose | TF-A |
| **meta-anbernic** | ours | everything below |

```
conf/machine/rg35xx-sp.conf        cortexa53-crypto tune, dtbs, U-Boot/TF-A, image types
conf/distro/anbernic.conf          no poky: busybox init + eudev, trimmed DISTRO_FEATURES and PACKAGECONFIGs
files/wic/rg35xx-sp.wks.in         card layout (SPL, FAT, two slots)
recipes-bsp/                       U-Boot (A/B boot.scr, watchdog, squashfs), TF-A (+ suspend stub)
recipes-kernel/linux/              Linux 7.2 + patches, savedefconfig, built-in firmware
recipes-core/images/               anbernic-image
recipes-core/anbernic-base/        init glue: USB gadget, A/B slots + /data, logs, watchdog, Weston, SD automount
recipes-core/sleepd/               suspend on lid / power key
recipes-graphics/                  launcher, Weston trims + kiosk-shell OSD layer, volume keys module, SDL2/vulkan-tools tweaks
recipes-emulators/, recipes-games/ RetroArch + cores, mGBA, SuperTuxKart: deployed to /data, not in the image
```

## Flashing a card

```
sudo bmaptool copy …/anbernic-image-rg35xx-sp.rootfs.wic.zst /dev/sdX
```
Put it in **TF1**. The first boot creates `/data` from the rest of the card. After that, update over USB
([dev-workflow.md](dev-workflow.md)).

## If it doesn't boot

- No LED, no screen: SPL/DRAM. The device may also be wedged from FEL experiments (see [fel.md](fel.md)).
- Backlight but no picture: the unit has the other panel. `boot.cmd` loads `sun50i-h700-anbernic-rg35xx-sp.dtb`;
  the slots also carry `…-rg35xx-sp-v2-panel.dtb`.
- UART: 115200 8N1 on the board's test pads.
