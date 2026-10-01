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

`boot.scr` ([boot.cmd](../yocto/meta-anbernic/recipes-bsp/u-boot/files/boot.cmd)) isn't part of a slot, so an OTA doesn't update it:
`tools/ssh 'cat > /uboot/boot.scr.new && mv /uboot/boot.scr.new /uboot/boot.scr && sync' < …/deploy/images/rg35xx-sp/boot.scr`.
It reads `ab.txt`:
- `slot=a|b` is the last known good slot.
- After an update, `tryslot=` and `tries=` are also set. `tryslot` gets 3 boots, and U-Boot writes the counter back to FAT before each one.

After 3 failed tries, U-Boot boots `slot` again and drops `tryslot`.

`touch /uboot/fbdev` turns on the kernel console on the LCD (fbdev emulation). It's off by default because it costs ~190 ms of boot.

Things that count as a failed try:
- A hang in U-Boot or the early kernel: the U-Boot-armed SoC watchdog (16 s) resets the board.
- A kernel panic: `panic=10` reboots.
- A userspace hang: busybox `watchdog` resets the board.
- A slot that boots but isn't committed within 120 s: `ab boot` reboots it. This catches a broken USB link, which would otherwise lock us out.

The commit comes from the host. Once ssh into the new slot works, `tools/ota` runs `ab commit`.

```
tools/ota                      # flash the latest build's slot image into the other slot, reboot, commit
tools/ota path/to/rootfs.squashfs-lz4
tools/ssh ab status
```

Device side: [ab](../yocto/meta-anbernic/recipes-core/anbernic-base/files/ab) with `status|mount|flash|commit|boot`.

## /data and the launcher

`/data` is the writable, non-system part: the image never touches it, so what lives there survives
`tools/ota`. wic can't know the card size, so the image has no p4. At boot, `ab mount` does this:
- If p4 is missing, it appends one that spans the rest of the card (`sfdisk`).
- It formats p4 only while `/uboot/data-new` marks the partition as just created by it.
- It mounts p4 at `/data`.

The OS's own overrides live in `/data/OS/` (`SD:/OS/`). Everything else on `/data` belongs to the user.

| Path | Used by |
|---|---|
| `OS/launcher` | `/etc/init.d/weston` runs it instead of the image's launcher |
| `OS/apps/{ports,applications,benchmarks}/` | the launcher's pages of the same names: `*.desktop` entries (or bare executables) |
| `roms/gba/` | the launcher's Game Boy Advance page, run with mGBA from `/data/mgba` |
| `OS/packs/<system>.pack` | game packs: titles, metadata and box art by ROM CRC32 (`tools/boxart-pack`) |
| `.cache/launcher/crc32` | the launcher's CRC32 of each ROM, by path, size and mtime |
| `OS/init.d/S*` | `/etc/init.d/data-init` (run by `/etc/init.d/weston`, rcS S01: after `/data` is mounted, before seatd and Weston) runs each executable one with `start`, in order. To fix a config on the read-only rootfs, bind-mount a file from `/data` over it. `touch /uboot/no-data-init` (FAT, editable from any PC) skips them all |
| `OS/profile.d/*.sh` | sourced last by `/etc/profile` and by `weston.init`: environment overrides (`VK_DRIVER_FILES`, `PANVK_DEBUG`, `MESA_*`) reach Weston, the launcher, apps and ssh shells |

The UI is [launcher/](../launcher) (SDL2 + Vulkan + HarfBuzz + fontconfig, immediate mode). `/etc/init.d/weston` runs
`/data/OS/launcher` if it exists, or else the image's `/usr/bin/launcher`. It restarts the launcher whenever it exits.
- `tools/deploy-launcher`: builds the launcher with bitbake, copies it to `/data/OS/launcher`, and restarts it. You don't need to reflash.
- `tools/ssh rm /data/OS/launcher`: goes back to the image's launcher.
- It shows one page per category, as tiles: Game Boy Advance (the ROMs in `/data/roms/gba`), Ports, Applications and Benchmarks (`/data/OS/apps/<category>/`), then System. An app is an XDG `.desktop` file (`Name`, `Exec`, `Path`, `Icon`, `Keywords`, `Categories`) or a bare executable. `Exec` runs under `sh -c`, in `Path` or else the folder. `Icon` is a PNG, JPEG, WebP or BMP, relative to the folder, and the extension is optional. Icons load on a background thread and upload on a transfer queue. Below 100 MB for the whole process, the icons that haven't been shown for a while get evicted. The page shown at exit is shown again at the next start (`$HOME/.local/state/launcher`). While an app runs, Menu pauses it (SIGSTOP) and shows Resume/Quit over a blur of its last frame. Quit sends SIGTERM (SDL apps quit cleanly). Menu or B resumes. While paused, the launcher grabs the pad (EVIOCGRAB), so the app never sees those presses.
- Apps carry `LAUNCHER_APP=<label>` in their environment. A launcher that (re)starts adopts an app the previous one left running (a session leader with that variable) and continues it if it was paused. Restarting the launcher (`tools/deploy-launcher`, a crash) doesn't end the game.
- Volume keys: Weston's `volume-keys.so` sets Line Out, and the launcher's `osd` window shows the level. The patched kiosk shell keeps that window on top without focus.
- [data/](../data) holds `/data` files that live in this repo: `tar cf - -C data . | tools/ssh tar xf - -C /data`. They include the
  GPU benchmarks (glmark2, vkmark), which show Mesa's HUD (`GALLIUM_HUD`, `VK_LAYER_MESA_overlay`)
  and write their scores to `/var/log`.
- ROMs get their title and box art from the system's game pack, matched by the CRC32 of the ROM (of a zip: of its first file, from the zip's directory). The launcher computes each CRC once in the background and caches it. An image of the same name next to the ROM wins over the pack's. To build a pack, use `tools/boxart-pack "Nintendo - Game Boy Advance" <libretro-thumbnails>/Named_Boxarts out.pack`. It downloads the No-Intro DAT and metadata (genre, developer, publisher, franchise, year, players) from libretro-database. It deduplicates box art by perceptual hash, keeps the best source of each group, and stores the art at 128 px as WebP (GBA: 2.4 GB of PNGs becomes 7.5 MB). The format is in [romdb.h](../launcher/romdb.h).
- On the desktop: `meson setup build launcher && ninja -C build && LAUNCHER_DATA=launcher/testdata build/launcher` (arrows, Enter, PgUp/PgDn). `launcher/testdata` holds fake apps and ROMs with made-up icons in every format. The apps only sleep for 2 s. Its GBA ROMs carry the CRC32s of real games, so they match a GBA pack in `OS/packs`.

mGBA (GBA) isn't in the image yet. Its recipe only deploys it: `kas build --target mgba`, then copy
`deploy/images/rg35xx-sp/mgba/` to `/data/mgba` and run it from there: mGBA looks for `portable.ini` in
the cwd and then keeps its config next to it. `config.ini` holds the gamepad bindings. Saves go next to the ROM (`/data/roms/gba`).

SuperTuxKart works the same way: `kas build --target supertuxkart`, copy `deploy/images/rg35xx-sp/supertuxkart/` (786 MB, too big
for a slot) to `/data/supertuxkart`. It finds `data/` next to the binary and keeps its config in `$HOME/.config` (`/data`).
Its shared libs (OpenAL Soft, vorbis, curl, ...) are in the image.

The [bindless-vk](../demos/bindless) Vulkan demo (1000 rects, textures from a descriptor-indexed sampler array) too:
`kas build --target bindless-demo`, copy `deploy/images/rg35xx-sp/bindless/` to `/data/bindless`.

RetroArch is also on `/data` (only its libraries, e.g. libav*, are in the image). `kas build --target retroarch`
builds RetroArch, the cores and the menu assets into `deploy/images/rg35xx-sp/retroarch-data/`, laid out like
`/data`: `retroarch/` (binary, `cores/`, `info/`, `assets/`, `autoconfig/`, the skeleton `retroarch.cfg`) and
`bios/PPSSPP`. Copy it over: `tar cf - -C …/retroarch-data . | tools/ssh tar xf - -C /data`.
The first start copies `retroarch.cfg` to `/data/.config/retroarch/`; delete that to reset.
In game: hold Select + X for the menu, + L1/R1 load/save state, + R2 fast forward; Menu pauses.

## Bootloader

SPL/U-Boot/TF-A at 8 KiB is shared by both slots. Updating it is the one step that can still need the card reader:
```
tools/ssh 'dd of=/dev/mmcblk0 bs=1k seek=8 && sync' < …/deploy/images/rg35xx-sp/u-boot-sunxi-with-spl.bin
```
Recovery: flash the full `.wic.zst` from a PC (see [yocto.md](yocto.md)).

## Logs

`/var/log` is `/data/log/0` (this boot); `/data/log/1`..`4` are the boots before. syslogd/klogd write
`messages`, plus `dmesg` (boot kernel log), `weston.log`, `launcher.log`. A hard panic doesn't reach the card:
DRAM doesn't survive a reset on this board, so there's no ramoops.

## SD card in TF2

Any filesystem on it is mounted at `/run/media/mmcblk1pN` when it's inserted and unmounted when it's pulled.

## Kernel config iteration

bitbake rebuilds the kernel from scratch on every `defconfig` edit (`kernel.bbclass` cleans `${B}` in `do_unpack`).
To iterate faster, build out of tree with host clang, then swap the Image into the current slot image:

```
K=/mnt/multimedia/anbernic-yocto/kernel-dev    # src/ = patched source, out/ = build, bin/bc = Yocto's bc (host has none)
cp yocto/meta-anbernic/recipes-kernel/linux/files/defconfig $K/out/.config
(cd $K/src && PATH=$K/bin:$PATH make O=$K/out ARCH=arm64 LLVM=1 olddefconfig Image -j16)   # incremental
cd $K/test && podman unshare sh -c 'unsquashfs -d root <slot.squashfs-lz4> && cp ../out/arch/arm64/boot/Image root/boot/Image-7.2.0 && mksquashfs root t.sqfs -comp lz4 -Xhc -noappend'
tools/ota $K/test/t.sqfs
```

When it works: `make savedefconfig` and copy `out/defconfig` back into the recipe (bitbake runs `olddefconfig`, so a minimal defconfig is enough).
