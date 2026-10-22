# Working on the device

One flash of the full card image (see [yocto.md](yocto.md)); after that everything goes over the USB-C (OTG) cable.

## The USB link

`senkos-base` sets up a configfs USB gadget at boot (`/etc/init.d/senkos`):

| Function | Device side | Host side |
|---|---|---|
| NCM ethernet | `usb0` = `10.55.0.1/24`, `udhcpd` hands out `10.55.0.2` (no router/DNS) | new ethernet interface, NetworkManager takes the lease on its own |
| ACM serial | getty on `ttyGS0` | `/dev/ttyACM*` (`picocom /dev/ttyACM0`), for when the network doesn't come up |

The MACs are fixed (`02:35:58:00:00:01` device, `…:02` host), so the host keeps one connection across reboots.
`musb` is `dr_mode = "otg"` with the ID pin on PI4: a cable to a PC makes it a gadget, an OTG adapter (keyboard) a host.

`tools/ssh [cmd]` is ssh to `root@10.55.0.1` (dropbear, empty root password, bring-up only) without recording host
keys, as every flashed slot makes new ones. dropbear listens on every interface, Wi-Fi too; Settings › Debug › SSH
stops it (sysd's `ssh off`), and `/data/OS/dropbear/disabled` keeps it off at boot.

## Deploying

| What | How |
|---|---|
| A whole image | `tools/ota [rootfs.squashfs-lz4]`: the latest build's slot image (or the one given) into the other slot, reboot, commit |
| The launcher | `tools/deploy-launcher`: builds it, puts it in `/data/OS/overlay/usr/bin/launcher`, runs `overlay` and restarts it |
| Weston's shell | `apps/shell` builds with the apps (`senkos-shell.so` in the deploy dir): into `/data/OS/overlay/usr/lib64/weston/` by a copy and a rename, then reboot (Weston has it mapped; `overlay` would write over it) |
| corehost, mediaplayer, a core | copy it to the same path under `/data/OS/overlay` (`OS/overlay/usr/lib64/libretro/` for a core), then run `overlay` |
| `boot.scr` ([boot.cmd](../yocto/meta-senkos/recipes-bsp/u-boot/files/boot.cmd)), the splash | `tools/ssh 'cat > /uboot/boot.scr.new && mv /uboot/boot.scr.new /uboot/boot.scr && sync' < …/deploy/images/rg35xx-sp/boot.scr`; not in a slot, so an OTA doesn't update them |
| SPL/U-Boot/TF-A | `tools/ssh 'dd of=/dev/mmcblk0 bs=1k seek=8 && sync' < …/deploy/images/rg35xx-sp/u-boot-sunxi-with-spl.bin`; shared by both slots. Recovery: flash the full `.wic.gz` from a PC |
| [data/](../data) (files a new card gets: GPU benchmarks, …) | `tar cf - -C data . \| tools/ssh tar xf - -C /data` |

An overlay goes back to the image's file with `tools/ssh rm /data/OS/overlay/<path>` and a reboot. When the control
channel's layout ([appctl.h](../apps/common/appctl.h)) changes, deploy the launcher and corehost together.

## A/B slots

```
8 KiB   SPL + U-Boot (+ TF-A BL31)       not A/B
p1 FAT  64M  boot.scr, ab.txt            mounted at /uboot
p2 squashfs 512M slot a: rootfs, /boot/Image + dtb
p3 squashfs 512M slot b: same
p4 ext4  rest    /data, read-write; made by the first boot (`ab mount`), survives slot updates
```

`boot.scr` reads `ab.txt`: `slot=a|b` is the last known good slot. After an update `tryslot=` and `tries=` are set
too: `tryslot` gets 3 boots (U-Boot writes the counter back to FAT before each), then U-Boot boots `slot` again and
drops `tryslot`. A failed try is:
- a hang in U-Boot or the early kernel: the U-Boot-armed SoC watchdog (16 s) resets the board;
- a kernel panic: `panic=10` reboots;
- a userspace hang: busybox `watchdog` resets the board;
- a slot not committed within 120 s: `ab boot` reboots it. This catches a broken USB link, which would lock us out.

The commit comes from the host: once ssh into the new slot works, `tools/ota` runs `ab commit`. Device side:
[ab](../yocto/meta-senkos/recipes-core/senkos-base/files/ab) `status|mount|flash|commit|boot`.

## Boot

The kernel shows a splash (kernel patch 0052, `drm_splash`) from ~0.5 s until Weston takes the display over without a
modeset. It lights the panel from a work item, so the panel's ~290 ms of init sleeps hold up nothing. The image is
`/uboot/splash.lz4`, or `/uboot/splash-light.lz4` while `/uboot/light` is there (light mode, sysd's `theme
light|dark`; the apps read the same file); `splash=<file>` in `/uboot/splash.txt` picks another file there. U-Boot
unpacks it into memory it reserves and passes its place in `/chosen`; the kernel frees it once drawn.
`tools/mksplash.py` makes both from `splash-art.png` (transparent, any size, drawn centred) on the UI's background,
storing only the box around the art; the image's copies are in the U-Boot recipe. `touch /uboot/fbdev` shows the
kernel console (fbcon) instead.

Userspace starts with the UI: rcS S01 (`/etc/init.d/weston`) mounts `/data`, runs `OS/init.d` and starts seatd,
Weston and the launcher. PipeWire and the rest of rcS (udev's coldplug, the USB gadget, the watchdog) wait for the
launcher's first frame (3 s at most). A udevd runs for Weston's start meanwhile, so libinput still gets the input
devices from the coldplug. Once rcS is done, init starts sysd and the USB network's udhcpd (inittab `respawn`).

sysd ([sysd.c](../yocto/meta-senkos/recipes-core/sysd/files/sysd.c)) is root's side of the UI: lid, power key and
suspend (see [suspend.md](suspend.md)), and what the unprivileged launcher asks for over `/run/sysd.sock`
(`SOCK_SEQPACKET`, a request a connection, shell or root only by `SO_PEERCRED`). It replies once the request is done,
`ok` or `err WHY`, and the launcher shows a refusal on the OSD. Requests map to commands in a table (`cmds`), run
without a shell; the scripts check their own arguments.

Kept across power-off:
- Settings › Debug › Hold power to turn on (128 ms, 512 ms, 1 s or 2 s): sysd keeps it in `OS/config/power-on-ms`;
  `save-settings` writes it into the AXP717 (register `0x26`, the pek driver's `startup` attribute, kernel patch
  0061), which keeps it while off.
- The brightness: `/uboot/brightness.txt`, written by `save-settings` at shutdown and before suspend (when changed).
  U-Boot sets the backlight node's `default-brightness-level` from it (kernel patch 0054 makes pwm-backlight honour it
  without a levels table), so the panel lights at that level. The LED driver stays dark when started below about 5%
  but goes down to 1% once lit, so kernel patch 0057 lights it at 10% for 50 ms first whenever it comes on from off
  below that: at boot, on resume, after a blank.

## Wi-Fi

The RTL8821CS (SDIO) uses the vendor `rtl8821cs` module (the BSP recipe pins `u-osmi/rtl8821cs-arm64` and carries
the Linux 7.2 patch; firmware embedded), with `cfg80211`, loaded only when Wi-Fi is on. Bluetooth firmware is built
into the kernel; the regulatory database is `wireless-regdb-static`'s. `/etc/modprobe.d/wifi.conf` keeps both the
vendor and the mainline SDIO drivers from loading on their own and the vendor's idle power saving off
(`rtw_power_mgnt=0`, `rtw_ips_mode=0`). The kernel still builds mainline rtw88, but the image loads only the vendor
module; modules can't be unloaded, so another driver takes a reboot.

[wifi](../yocto/meta-senkos/recipes-core/senkos-base/files/wifi) (sysd runs `wifi on|off` for the launcher's Wi-Fi
screen, and `wifi` once the launcher is up, out of the boot's way) loads it and starts `wpa_supplicant` on `wlan0` and
`wpa_cli -a wifi`, whose CONNECTED runs `udhcpc`; a lease runs `clock net` (`udhcpc.d/90ntp`, see Time).
`/data/OS/wifi` (root's alone, 0700) holds `wpa_supplicant.conf` (the networks, written by `SAVE_CONFIG`) and
`enabled`; `/run/wifi` says on or off. The launcher talks to `wpa_supplicant` over `/run/wpa_supplicant/wlan0`
(group player) as `wpa_cli` would; the status bar's symbol is `SIGNAL_POLL`'s RSSI.

```
tools/ssh 'wpa_cli -i wlan0 status; wpa_cli -i wlan0 scan_results'
```

## Time

[clock](../yocto/meta-senkos/recipes-core/senkos-base/files/clock) (sysd runs `clock ...` for Settings) keeps
`/data/OS/time` (root's, readable): `localtime`, a link into `/usr/share/zoneinfo` that `/etc/localtime` links to
(none: UTC); `chosen` when the zone was picked; `manual` when NTP is off. Every DHCP lease sets the clock by NTP
(`ntpd -q`, then the RTC) unless `manual`; a new one (a network joined, not a renewal) also guesses the zone unless
`chosen`: `downloadd zone` asks ipapi.co where the address is. The launcher sets `TZ` whenever the link changes
(glibc doesn't read `/etc/localtime` again by itself), so what it starts afterwards has it too.

```
tools/ssh 'clock zone Europe/Berlin; clock zone auto; clock ntp off; clock set $(date +%s)'
```

## /data

`/data` is everything writable; the image never touches it, so it survives `tools/ota`. wic can't know the card size,
so the image has no p4. At boot `ab mount` appends one spanning the rest of the card when it's missing (`sfdisk`),
formats it only while `/uboot/data-new` says it just made it, mounts it, and on a new one copies the image's
`/usr/share/senkos/data` there (`senkos-data`: the repo's [data/](../data), the empty folders, assets from recipes).

Ours on `/data` are `OS`, `apps`, `roms`, `music` and `videos`; the OS's own files are all in `OS/`. HOME is `/data`:
what programs keep for themselves goes in `.local/state` and `.cache`, settings in `OS/config` (`XDG_CONFIG_HOME`).

| Path | What |
|---|---|
| `apps/{applications,benchmarks}/` | the launcher's pages of those names: `.desktop` files or bare executables |
| `apps/emulators/` | emulators and players, `.desktop` files over the image's (`/usr/share/senkos/emulators`); one of the same file name replaces the image's or hides it (`Hidden=true`). `MimeType` lists the systems' types it runs, `%f` in `Exec` is the game |
| `apps/systems/` | game systems, `.system` files over the image's (`/usr/share/senkos/systems`), replaced or hidden by file name likewise: `[System]` `Name`, `Folder` (its page's, e.g. `roms/msx`), `Aliases` (folder names elsewhere, for Libraries), `Patterns` (`*.rom;*.mx2;`), `MimeType` (what its emulators take), `Pack`, `Order`, `Emulator` (the default emulator's `.desktop` file name). A new system is a `.system` here plus an emulator in `apps/emulators` |
| `roms/<system>/` | the system pages |
| `videos/` | the Videos page (`.mp4 .mkv .webm .mov .m4v .avi .ts`) |
| `OS/assets/thumbnails/<system>.pack` | game packs: titles, metadata and box art by ROM CRC32 |
| `OS/assets/emulator/` | the cores' system directory, BIOS and assets (`COREHOST_SYSTEM`) |
| `OS/saves/<core>/` | the cores' saves (`COREHOST_SAVE`) |
| `OS/config/` | settings: `corehost/<core>.opt`, `mediaplayer.opt`, `rockbox.org/` (Rockbox), `power-on-ms`, `usb` (Storage) |
| `music/` | Rockbox's start directory |
| `OS/launcher/` | the launcher's home (it runs as `shell`): `config/` (`launcher-cores`: emulator per game, `launcher-matches`: pack game per ROM, `launcher-folders`: Libraries, `launcher-power`, `launcher-perf`), `state/` (`launcher`: the page shown, `launcher-ignored`), `cache/` (`launcher/crc32`, thumbnails) |
| `OS/overlay/` | rootfs overlays: `OS/overlay/usr/bin/launcher` replaces `/usr/bin/launcher` and so on. `overlay` (at boot, again after a change) copies them into RAM, an overlayfs per top-level directory, so `/data` stays free to unmount |
| `OS/init.d/S*` | run in order with `start` by `/etc/init.d/data-init` (from rcS S01, after `/data` is mounted, before seatd and Weston). `touch /uboot/no-data-init` (FAT, editable from any PC) skips them |
| `OS/profile.d/*.sh` | sourced last by `/etc/profile` and `weston.init`: environment (`VK_DRIVER_FILES`, `PANVK_DEBUG`, `MESA_*`) for Weston, the launcher, apps and ssh shells |
| `OS/time/`, `OS/wifi/` | see Time and Wi-Fi |
| `OS/dropbear/`, `OS/log/` | the ssh host key; the logs of the last 5 boots |
| `OS/launcher/cache/launcher/crc32` | each ROM's CRC32 (past an iNES, fwNES, Lynx or copier header) and a jar's own name, by path, size and mtime |

`OS/init.d`, `OS/overlay`, `OS/profile.d` and `OS/dropbear` are root's (`weston.init` makes them so at every boot):
root runs or trusts them, apps run as `player`. `/data` and `/data/OS` are `root:player 1775`, so the player creates
files there but can't rename or delete root's.

## The launcher

[apps/launcher/](../apps/launcher) is the UI (Wayland + Vulkan + HarfBuzz + fontconfig, immediate mode).
`/etc/init.d/weston` runs it and restarts it whenever it exits.

- **Pages:** one per category, as tiles: the systems (each `.system`'s folder, shown when it exists), Videos,
  Applications, Benchmarks. An app is a `.desktop` file (`Name`, `Exec`, `Path`, `Icon`, `Keywords`, `Categories`)
  or a bare executable; `Exec` runs under `sh -c` in `Path`, else the folder. `Icon` is a PNG, JPEG, WebP or BMP
  relative to the folder, the extension optional; icons load on a thread, upload on a transfer queue, and those not
  shown for a while go when the process passes 100 MB. The page shown at exit comes back at the next start. The focused text scrolls when it doesn't fit.
- **Games:** a system page runs a game with the emulator whose `MimeType` has the system's type, in the game's
  folder: the `.system`'s `Emulator`, else the first by name. Select on a game picks another (kept per ROM in
  `launcher-cores`). A system with no emulator says so.
- **Running apps:** several at once; launching one that runs brings it back. A running app's tile has a dot. Apps get
  SIGTERM through `PR_SET_PDEATHSIG` when the launcher dies, and nothing is adopted when it starts again, so
  restarting it ends the games (after they save). An app that exits non-zero or dies from a signal (not our SIGTERM)
  is logged and gets a dialog at home; "Ignore for this app" adds it to `launcher-ignored`.
- **Pause menu:** Menu pauses the app and shows a menu over a blur of its last frame; Menu or B resumes. Menu acts on
  release, and only when no other button or volume key was pressed meanwhile (Menu plus a button is the app's
  hotkey, plus a volume key the backlight). While paused the launcher grabs the pad (EVIOCGRAB), taking and giving it back only once nothing is held, so neither side misses a release. An app with a control
  channel ([appctl.h](../apps/common/appctl.h): a `SOCK_SEQPACKET` socketpair with fixed-size requests and a memfd
  holding the options, nothing serialized), like corehost, gets Resume, Save state, Load state, Settings, Restart,
  Home and Quit. Load state, Restart and Quit ask first, Quit not when the `.desktop` has `X-SenkOS-AskQuit=false`.
  One with `APPCTL_MENU_START` keeps Menu for itself and opens the menu on Menu + Start
  ([padtest](../apps/padtest/main.c): SDL's view of the pad and its rumble, copied to `/data/apps/benchmarks`).
  Others are paused with SIGSTOP and get Resume, Settings, Home and Quit. Quit is SIGTERM. Home leaves the app paused
  on the Running page; Menu at home brings back the last one. The menu comes back as it was left.
- **Settings:** the app's categories with options (System, Graphics, Audio, Input; Left/Right change one), then Power
  and Debug. Power ([power.h](../apps/launcher/power.h)): CPU and GPU clocks (Auto, or a step of the kernel's
  tables: the CPU's a cap, the GPU's pinned; per ROM, app folder or command in `launcher-power`, set while the app has
  the screen, Auto at home), the refresh rate (one for everything: one of the panel's modes, 60, 120 Hz; Weston's shell
  switches it (the pixel clock alone, no modeset: kernel patch 0064), keeps it in `OS/config/refresh-rate` and sets
  it again at start; corehost re-paces to it; whether the panel shows every frame:
  [refreshtest](../apps/refreshtest/main.c), copied to `/data/apps/benchmarks`), the backlight (also Menu + volume keys
  anywhere), and Automatic calibration: pick a frame rate (30 to the refresh rate), and back in the app the CPU's clock goes down a step while every second of five holds it, then the
  GPU's, keeping the last that held (about a minute). Frames are counted by Weston's shell
  (`$XDG_RUNTIME_DIR/frames`, bumped at each repaint's latch), so any app can be calibrated; sysd sets the clocks.
  Debug: the performance overlay ([perf.h](../apps/launcher/perf.h)), its corner and lines: fps, CPU load and clock,
  GPU load (kernel patch 0058's `busy_ns`) and clock, video decoder load (0059), memory, SD throughput, temperature.
- **Titles and box art:** from the system's game pack by the ROM's CRC32 (a zip's: its first file's, from the
  directory), computed once in the background. When it finds nothing or the wrong game, Select › Title and box art
  picks one of the pack's games by hand, ranked by words shared with the file name (or its initials: `aos.gba` finds
  Aria of Sorrow) or with what Search typed; kept in `launcher-matches` (`ROM<tab>CRC`); By its contents goes back.
  An image of the same name next to the ROM wins over both. A J2ME jar names itself and its icon is its art
  (MIDlet-Name and MIDlet-Icon in its manifest, [jar.h](../apps/common/ui/jar.h)). Packs come from `tools/boxart-pack "Nintendo - Game Boy
  Advance" <libretro-thumbnails>/Named_Boxarts out.pack`; the format is in [romdb.h](../apps/launcher/romdb.h).
- **Menu icons** are SVG strokes in `apps/common/ui/icons` (Lucide's and ours in its style); `tools/icon-paths` turns
  them into `apps/common/ui/icons.inc`. FreeType strokes them at the size drawn, into the glyph atlas.

## corehost

[corehost](../apps/corehost) runs one libretro core with one game, with no UI of its own; the cores are in the image
(`/usr/lib64/libretro`) and an emulator is a `.desktop` running `corehost /usr/lib64/libretro/<core>_libretro.so %f`.
How it renders, paces and resamples is in [main.c](../apps/corehost/main.c)'s header and comments.

- **Saves:** `OS/saves/<core>/ROM.srm` (save RAM), `ROM.state0`, and what a core saves itself. With no `.srm` there
  yet, corehost reads the save RAM next to the ROM (`ROM.srm`, else `ROM.sav`) and never writes it.
- **Options:** `OS/config/corehost/<core>.opt` over the image's defaults
  ([corehost/defaults](../apps/corehost/defaults)). Core options v2 categories map to Graphics, Audio and Input, the
  rest to System. corehost adds scaling and filter, fast forward speed, and under Input Rumble (the motor, the pad's
  `FF_RUMBLE`; shown when the core asks for rumble) and a binding per input the core
  describes (SET_INPUT_DESCRIPTORS, by its names; a stick is a row per direction), then the hotkeys. With a left
  stick and no stick on the pad, the stick defaults to the D-pad and the core's D-pad to Menu + D-pad. Y makes an
  option or binding the game's own (blue label, `<core>/<ROM name>.opt`); Y again gives it back. A core that reads
  its options only at start (mupen) gets them by a state saved to `ROM.state99`, corehost exec'ing itself, and the
  state loaded.
- **Bindings:** a button combo. A on a row records, letting go of the first button pressed sets it ("R2 + Start +
  A"), X clears it, five seconds without a press gives up. Of two held bindings that overlap, the bigger wins.
- **Hotkeys:** fast forward (toggle Menu + R1; held L2 or R2 where the game doesn't use them; System › Fast forward
  speed, 2× to 8×), rewind (hold Menu + L1; whole states in 32 MB, none for cores whose states don't fit a second of
  it), save and load state (slot 0; Menu + R2 / L2). An overlay top left shows notices and fast forward or rewind.

## Media player and sound

`mediaplayer` ([apps/mediaplayer](../apps/mediaplayer)) plays the Videos page with GStreamer's playbin3: Cedrus
decodes H.264, H.265 (10-bit too), VP8, VP9 and MPEG-2 through v4l2codecs and waylandsink hands the dmabufs to Weston;
gst-libav decodes the audio, and the video when Cedrus can't or fails. Controls: A or Start pause; Left/Right seek
10 s, L1/R1 60 s; Up/Down previous/next video; X, Y or Select show the OSD; B quits. Graphics has the scaling,
Audio the track. Where each video was left is kept in `.local/state/mediaplayer/resume`; past 95 % counts as seen.

Music is Rockbox, built as an application (recipes-multimedia/rockbox: upstream plus our patches, target
"Retro Handheld" 208). It isn't in the image: the recipe deploys `rockbox.zip` (`rockbox.desktop` and
`rockbox/`), unpacked into the card's `apps/applications`; configure's `--relocatable` (0007) finds codecs, plugins
and themes next to `rockbox/bin/rockbox`. The pad is an SDL game controller, the card's `music/` the start
directory, its settings in `OS/config/rockbox.org` (defaults: `rockbox/share/rockbox/config.cfg`).

Media players speak MPRIS on the player's session bus (`unix:path=/run/user/1000/bus`, started by `weston.init`
after the first frame; `session.d/senkos.conf` lets the launcher in): Rockbox serves it. The launcher's
quick menu (Start at home) shows the track of the one playing, else the last one, with previous, play/pause and
next; a new track while its app isn't in front shows on the OSD. An app whose player is playing isn't stopped
behind the launcher (an appctl app still gets its pause and decides).

Sound goes through PipeWire (ALSA's default is PipeWire; Pulse clients use pipewire-pulse; SDL2 apps through its own
PipeWire backend, `SDL_AUDIODRIVER=pipewire,alsa`). WirePlumber owns the
volume (the default sink's drives the codec's Line Out) and restores it at start; nothing may write Line Out directly.
The launcher reads the volume keys (evdev, whatever has the focus), steps the volume with `wpctl` and shows it in its
`osd` window, an overlay of Weston's shell: on top, never focused. A paused app writes nothing.

## On the desktop

`meson setup build apps && ninja -C build && LAUNCHER_DATA=apps/launcher/testdata build/launcher/launcher` (arrows,
Enter, PgUp/PgDn). `apps/launcher/testdata` holds fake apps and ROMs with icons in every format; its GBA ROMs carry
real games' CRC32s, so they match a GBA pack in `OS/assets/thumbnails`, and `apps/emulators/fake.desktop` stands in
for the image's emulators. The media player takes the arrow keys, Space, Esc, Tab and PgUp/PgDn for the pad.

## Logs

`/var/log` is `/data/OS/log/0` (this boot); `1`..`4` are the boots before: `messages` (syslogd/klogd), `dmesg` (the
boot's kernel log), `weston.log`, `launcher.log` (with the apps' output). A hard panic doesn't reach the card: DRAM
doesn't survive a reset on this board, so there's no ramoops.

## SD card in TF2

Any filesystem on it is mounted at `/run/media/mmcblk1pN` when it's inserted and unmounted when it's pulled.

## Kernel config iteration

bitbake rebuilds the kernel from scratch on every `defconfig` edit (`kernel.bbclass` cleans `${B}` in `do_unpack`).
To iterate faster, build out of tree with host clang, then swap the Image into the current slot image:

```
K=/mnt/multimedia/anbernic-yocto/kernel-dev    # src/ = patched source, out/ = build, bin/bc = Yocto's bc (host has none)
cat yocto/meta-anbernic/recipes-kernel/linux/files/defconfig yocto/meta-senkos/dynamic-layers/anbernic/recipes-kernel/linux/files/splash.cfg > $K/out/.config
(cd $K/src && PATH=$K/bin:$PATH make O=$K/out ARCH=arm64 LLVM=1 olddefconfig Image -j16)   # incremental
cd $K/test && podman unshare sh -c 'unsquashfs -d root <slot.squashfs-lz4> && cp ../out/arch/arm64/boot/Image root/boot/Image-7.2.10 && mksquashfs root t.sqfs -comp lz4 -Xhc -noappend'
tools/ota $K/test/t.sqfs
```

When it works: `make savedefconfig` and copy `out/defconfig` back into the recipe, minus the `splash.cfg` lines
(meta-senkos adds them); bitbake runs `olddefconfig`, so a minimal defconfig is enough.
