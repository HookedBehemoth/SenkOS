# Working on the device over USB (no card swapping)

One flash of the full card image, then everything goes over the USB-C (OTG) cable.

## The link

`anbernic-base` sets up a configfs USB gadget at boot (`/etc/init.d/anbernic`):

| Function | Device side | Host side |
|---|---|---|
| NCM ethernet | `usb0` = `10.55.0.1/24`, `udhcpd` hands out `10.55.0.2` (no router/DNS) | new ethernet interface, NetworkManager takes the DHCP lease on its own |
| ACM serial | getty on `ttyGS0` | `/dev/ttyACM*` (`picocom /dev/ttyACM0`), fallback when the network doesn't come up |

Fixed MACs (`02:35:58:00:00:01` device, `…:02` host), so the host keeps the same connection across reboots.
`musb` is `dr_mode = "otg"` with the ID pin on PI4: a cable to a PC makes it a gadget, an OTG adapter (keyboard) makes it a host.

`tools/ssh [cmd]` runs ssh to `root@10.55.0.1` (dropbear, empty root password, bring-up only). It doesn't record host keys, because every flashed slot makes new ones.

## A/B slots

```
8 KiB   SPL + U-Boot (+ TF-A BL31)       not A/B; see "Bootloader" below
p1 FAT  64M  boot.scr, ab.txt            mounted at /uboot
p2 squashfs 512M slot a: rootfs, /boot/Image + dtb
p3 squashfs 512M slot b: same
p4 ext4  rest    /data, read-write; made by the first boot (`ab mount`), survives slot updates
```

`boot.scr` ([boot.cmd](../yocto/meta-anbernic/recipes-bsp/u-boot/files/boot.cmd)) reads `ab.txt`:
- `slot=a|b` is the last known good slot.
- After an update, `tryslot=` and `tries=` are also set. `tryslot` gets 3 boots, and U-Boot writes the counter back to FAT before each one.

After 3 failed tries, U-Boot boots `slot` again and drops `tryslot`.

Things that count as a failed try:
- A hang in U-Boot or the early kernel: the U-Boot-armed SoC watchdog (16 s) resets the board.
- A kernel panic: `panic=10` reboots.
- A userspace hang: busybox `watchdog` resets the board.
- A slot that boots but isn't committed within 120 s: `ab boot` reboots it. This catches a broken USB link, which would otherwise lock us out.

The commit comes from the host. Once ssh into the new slot works, `tools/ota` runs `ab commit`.

```
tools/ota                      # flash the latest build's rootfs.ext4 into the other slot, reboot, commit
tools/ota path/to/rootfs.ext4
tools/ssh ab status
```

Device side: [ab](../yocto/meta-anbernic/recipes-core/anbernic-base/files/ab) with `status|mount|flash|commit|boot`.

## /data and the launcher

`/data` is the writable, non-system part: the image never touches it, so what lives there survives
`tools/ota`. wic can't know the card size, so the image has no p4. At boot, `ab mount` does this:
- If p4 is missing, it appends one that spans the rest of the card (`sfdisk`).
- It formats p4 only while `/uboot/data-new` marks the partition as just created by it.
- It mounts p4 at `/data`.

The UI is [launcher/](../launcher) (SDL2 + HarfBuzz + fontconfig, immediate mode). `/etc/init.d/weston` runs
`/data/bin/launcher` if it exists, or else the image's `/usr/bin/launcher`. It restarts the launcher whenever it exits.
- `tools/deploy-launcher`: builds the launcher with bitbake, copies it to `/data/bin`, and restarts it. You don't need to reflash.
- `tools/ssh rm /data/bin/launcher`: goes back to the image's launcher.
- Its Apps page lists every executable in `/data/apps`. While one runs, Menu sends it SIGTERM (SDL apps quit cleanly).
- On the desktop: `meson setup build launcher && ninja -C build && build/launcher` (arrows, Enter, PgUp/PgDn).

mGBA (GBA) isn't in the image yet. Its recipe only deploys it: `kas build --target mgba`, then copy
`deploy/images/rg35xx-sp/mgba/` to `/data/mgba` and run it from there: mGBA looks for `portable.ini` in
the cwd and then keeps its config next to it. `config.ini` holds the gamepad bindings. Saves go next to the ROM (`/data/roms/gba`).

SuperTuxKart works the same way: `kas build --target supertuxkart`, copy `deploy/images/rg35xx-sp/supertuxkart/` (786 MB, too big
for a slot) to `/data/supertuxkart`. It finds `data/` next to the binary and keeps its config in `$HOME/.config` (`/data`).
Its shared libs (OpenAL Soft, vorbis, curl, ...) are in the image.

RetroArch is also on `/data` (only its libraries, e.g. libav*, are in the image). `kas build --target retroarch`
builds RetroArch, the cores and the menu assets into `deploy/images/rg35xx-sp/retroarch-data/`, laid out like
`/data`: `retroarch/` (binary, `cores/`, `info/`, `assets/`, `autoconfig/`, the skeleton `retroarch.cfg`) and
`bios/PPSSPP`. Copy it over: `tar cf - -C …/retroarch-data . | tools/ssh tar xf - -C /data`.
The first start copies `retroarch.cfg` to `/data/.config/retroarch/`; delete that to reset.
In game: hold Select + X for the menu, + L1/R1 load/save state, + R2 fast forward; Menu quits.

## Bootloader

SPL/U-Boot/TF-A at 8 KiB is shared by both slots. Updating it is the one step that can still need the card reader:
```
tools/ssh 'dd of=/dev/mmcblk0 bs=1k seek=8 && sync' < …/deploy/images/rg35xx-sp/u-boot-sunxi-with-spl.bin
```
Recovery: flash the full `.wic.zst` from a PC (see [yocto.md](yocto.md)).

## Kernel config iteration

Every `defconfig` edit reruns `do_unpack`, and `kernel.bbclass` cleans `${B}` there, so bitbake rebuilds the kernel from scratch every time.
To iterate faster, build out of tree with host clang, then swap the Image into the current slot image:

```
K=/mnt/multimedia/anbernic-yocto/kernel-dev    # src/ = patched source, out/ = build, bin/bc = Yocto's bc (host has none)
cp yocto/meta-anbernic/recipes-kernel/linux/files/defconfig $K/out/.config
(cd $K/src && PATH=$K/bin:$PATH make O=$K/out ARCH=arm64 LLVM=1 olddefconfig Image -j16)   # incremental
cd $K/test && podman unshare sh -c 'unsquashfs -d root <slot.squashfs-zst> && cp ../out/arch/arm64/boot/Image root/boot/Image-7.2.0 && mksquashfs root t.sqfs -comp zstd -noappend'
tools/ota $K/test/t.sqfs
```

When it works: `make savedefconfig` and copy `out/defconfig` back into the recipe (bitbake runs `olddefconfig`, so a minimal defconfig is enough).
