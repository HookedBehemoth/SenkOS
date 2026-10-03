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
- It mounts p4 at `/data`, and when it just made it, copies the image's `/usr/share/anbernic/data` there (`anbernic-data`: the repo's [data/](../data) and the empty folders).

Ours on `/data` are `OS`, `apps`, `roms`, `music` and `videos`; the OS's own files are all in `OS/`. HOME is `/data`: what the launcher and apps keep for themselves stays in `.local/state` and `.cache`, but settings are in `OS/config` (`XDG_CONFIG_HOME`).

| Path | Used by |
|---|---|
| `apps/{ports,applications,benchmarks}/` | the launcher's pages of the same names: `*.desktop` entries (or bare executables) |
| `apps/emulators/` | emulators and players, `.desktop` files over the image's (`/usr/share/anbernic/emulators`): one of the same file name replaces the image's or hides it (`Hidden=true`). `MimeType` says which systems' games it runs (each system's `MimeType`), `%f` in `Exec` is the game |
| `apps/systems/` | game systems, `.system` files over the image's (`/usr/share/anbernic/systems`), replaced or hidden by file name like emulators: `[System]` `Name`, `Folder` (its page's, e.g. `roms/msx`), `Aliases` (folder names elsewhere, for Libraries), `Patterns` (`*.rom;*.mx2;`), `MimeType` (what its emulators take), `Pack`, `Order`. A new system is a `.system` here plus an emulator in `apps/emulators` |
| `roms/<system>/` | the launcher's system pages |
| `videos/` | the launcher's Videos page (`.mp4 .mkv .webm .mov .m4v .avi .ts`), played by `mediaplayer` |
| `OS/assets/thumbnails/<system>.pack` | game packs: titles, metadata and box art by ROM CRC32 (`tools/boxart-pack`) |
| `OS/assets/emulator/` | the cores' BIOS and asset files (`COREHOST_SYSTEM`; PPSSPP's come with a new card) |
| `OS/saves/<core>/` | the cores' saves: save RAM, states, PPSSPP's memory stick |
| `OS/config/` | settings: `corehost/<core>.opt`, `mediaplayer.opt`, `launcher-cores` (the emulator picked per game), `launcher-folders` (Libraries), `usb` (Storage) |
| `OS/overlay/` | rootfs overlays: `OS/overlay/usr/bin/launcher` replaces `/usr/bin/launcher` and so on (`overlay`, at boot: copied into RAM, an overlayfs per top-level directory, so `/data` stays free to unmount; run it again after a change) |
| `OS/dropbear/`, `OS/log/` | the ssh host key; the logs of the last 5 boots |
| `.cache/launcher/crc32` | the launcher's CRC32 of each ROM, by path, size and mtime |
| `OS/init.d/S*` | `/etc/init.d/data-init` (run by `/etc/init.d/weston`, rcS S01: after `/data` is mounted, before seatd and Weston) runs each executable one with `start`, in order. To change a file of the read-only rootfs, put it in `OS/overlay`. `touch /uboot/no-data-init` (FAT, editable from any PC) skips them all |
| `OS/profile.d/*.sh` | sourced last by `/etc/profile` and by `weston.init`: environment overrides (`VK_DRIVER_FILES`, `PANVK_DEBUG`, `MESA_*`) reach Weston, the launcher, apps and ssh shells |

`OS/init.d`, `OS/overlay`, `OS/profile.d` and `OS/dropbear` are root's (`weston.init` makes them so at every boot): root runs or trusts them, and apps run as `player`. `/data` and `/data/OS` are `root:player 1775`, so the player creates files there but can't rename or delete root's. Change them as root (`tools/ssh`) or over UMS.

The UI is [apps/launcher/](../apps/launcher) (Wayland + Vulkan + HarfBuzz + fontconfig, immediate mode; no SDL in our apps). `/etc/init.d/weston` runs
`launcher` (`/data/OS/overlay/usr/bin/launcher` over the image's when there is one). It restarts the launcher whenever it exits.
- `tools/deploy-launcher`: builds the launcher with bitbake, copies it to `/data/OS/overlay/usr/bin/launcher`, runs `overlay` and restarts it. You don't need to reflash.
- `tools/ssh rm /data/OS/overlay/usr/bin/launcher`, then a reboot: back to the image's launcher.
- It shows one page per category, as tiles: the systems (the ROMs in `/data/roms/<system>`), Videos, Ports, Applications and Benchmarks (`/data/apps/<category>/`). An app is an XDG `.desktop` file (`Name`, `Exec`, `Path`, `Icon`, `Keywords`, `Categories`) or a bare executable. `Exec` runs under `sh -c`, in `Path` or else the folder. `Icon` is a PNG, JPEG, WebP or BMP, relative to the folder, and the extension is optional. Icons load on a background thread and upload on a transfer queue. Below 100 MB for the whole process, the icons that haven't been shown for a while get evicted. The page shown at exit is shown again at the next start (`$HOME/.local/state/launcher`). While an app runs, Menu pauses it and shows a menu over a blur of its last frame. Quit sends SIGTERM (ours and SDL apps quit cleanly). Menu or B resumes. While paused, the launcher grabs the pad (EVIOCGRAB), so the app never sees those presses.
- Apps die with the launcher. Each one gets SIGTERM through `PR_SET_PDEATHSIG`, even if the launcher crashes, and corehost also quits when its control socket closes. Nothing is adopted when the launcher starts again, so restarting it (`tools/deploy-launcher`) ends the games, after they have saved.
- Several apps can run at once. Each one gets a control channel ([appctl.h](../apps/common/appctl.h)): a `SOCK_SEQPACKET` socketpair with fixed-size requests and a memfd holding a fixed struct of options. Neither side serializes anything. An app that uses the channel ([corehost](../apps/corehost)) is paused through it, and its pause menu also has Save state, Load state and its settings, an entry per category that has any: System, Graphics, Audio and Input (Left/Right change an option). Home leaves the app paused in the background, on the Running page; Menu at home brings back the last one. Apps that don't use the channel are paused with SIGSTOP and get Resume, Home and Quit. Menu pauses on its release, and only when no other button was pressed while it was held: Menu plus a button is the app's hotkey. A tile whose app is running has a dot. An app that exits non-zero or dies from a signal (other than our SIGTERM) is logged and gets a dialog at home; "Ignore for this app" adds it to `$XDG_STATE_HOME/launcher-ignored`. Launching something that is already running brings it back instead of starting a second copy, which would share its save. The launcher keeps its page and focus while an app has the screen.
- Videos play in `mediaplayer` (in the image; another build goes in `OS/overlay/usr/bin/mediaplayer`; `apps/emulators` has its `.desktop` with the video types). It uses GStreamer's playbin3 (playbin's decodebin makes v4l2codecs refuse dmabufs). Cedrus decodes H.264, H.265, VP8, VP9 and MPEG-2 through v4l2codecs, and waylandsink hands the decoded dmabufs to Weston without a copy. gst-libav decodes the audio, and the video too when Cedrus can't (MPEG-4 ASP, AV1) or decoding fails on it: the file is then opened again in software. 10-bit HEVC and VP9 decode on Cedrus too, to P010, which Weston takes as a dmabuf as well. The OSD is a CPU-drawn subsurface above the video's: title, time and a progress bar, for 3 s after each press and for as long as playback is paused. Controls: A or Start pause; Left/Right seek 10 s and L1/R1 60 s (hold to repeat); Up/Down go to the previous/next video in the folder; X, Y or Select show the OSD; B quits. Menu opens the launcher's pause menu, where Graphics has the scaling (Fit/Stretch, kept in `OS/config/mediaplayer.opt`) and Audio has the audio track, when the file has more than one. Where each video was left is kept in `$HOME/.local/state/mediaplayer/resume`, and playback starts there; a video past 95 % counts as seen. At the end the next video in the folder plays. No subtitles yet. On the desktop, the arrow keys, Space, Esc, Tab and PgUp/PgDn stand in for the pad.
- Games run in `corehost` (in the image; another build goes in `OS/overlay/usr/bin/corehost`): one libretro core, one game, no UI of its own. The cores are in the image (`/usr/lib/libretro`); each system page (each `.system`'s folder, shown only when it exists) runs a game with the emulator `.desktop` whose `MimeType` has the system's type (`corehost /usr/lib/libretro/<core>_libretro.so %f`), in the game's folder. A system with none says so instead. Cores that render with GL (N64, Dreamcast, PSP) get a GLES context from corehost and draw into a framebuffer it blits to the window, scaled like the others. Their BIOS and asset files come from `/data/OS/assets/emulator` (`COREHOST_SYSTEM`; PPSSPP's assets in `PPSSPP/` come with a new card). N64 runs at full speed with GLideN64's threaded renderer and native resolution (`mupen64plus-ThreadedRenderer=True`, `mupen64plus-EnableNativeResFactor=1` in the options), as ROCKNIX sets it. Where a system has more than one emulator (Game Boy, Game Boy Color: Gambatte, mGBA; PPSSPP and PPSSPP (Vulkan); Flycast likewise), the first by name runs a game and Select on it picks another, kept per ROM in `OS/config/launcher-cores`. Saves are in `/data/OS/saves/<core>/` (`COREHOST_SAVE`): `ROM.srm` (save RAM), `ROM.state0`, and what a core saves itself (PPSSPP's memory stick, `PSP/`). When there is no `.srm` there yet, corehost reads the save RAM next to the ROM (`ROM.srm`, else `ROM.sav`: another emulator's or OS's) and never writes it. Options live in `OS/config/corehost/<core>.opt`, read over the image's defaults in `/usr/share/corehost/<core>.opt` ([corehost/defaults](../apps/corehost/defaults); the N64's bindings: stick on the D-pad, A/B on A/B, L/R on L1/R1, Z on L2, the C buttons on R2 + X/B/Y/A and the D-pad on R2 + D-pad). corehost asks cores for core options v2, which carry a category: libretro's video, audio and input become Graphics, Audio and Input, everything else (system, performance) System. corehost adds scaling and filter (Graphics) and, under Input, bindings for the game's inputs the core describes (SET_INPUT_DESCRIPTORS, by its names for them: a GBA has no X, Y, L2 or R2), then the hotkeys. Descriptors that come after the rows are made (mupen sends them once its emulation thread starts) make them again, saved values reloaded. A row is the console's input and the binding what presses it here; a stick is a row per direction at full tilt. When the core has a left stick and the pad has none, the stick defaults to the D-pad and the core's D-pad to Menu + D-pad. The N64 core is patched to one row per N64 button (independent C buttons, no RetroPad C-button options or deadzone). mupen reads its options only when the game starts: back in the game after changing one, corehost saves a state to `ROM.state99`, execs itself in place (same pid and control channel) and loads it once the core has booted. The hotkeys: fast forward (toggle, 3×; Menu + R1), rewind (hold; Menu + L1; whole states in 32 MB, a few seconds for GBA; no row for cores whose states don't fit a second of it, N64 and up), save state and load state (slot 0; Menu + R2 / L2). A binding is a button combo: A on its row records, the buttons show while held, and letting go of the first one pressed sets it ("R2 + Start + A"); X clears it, five seconds without a press gives up. Y makes a binding the game's own (its label turns blue; kept in `<core>/<ROM name>.opt` beside `<core>.opt`), Y again gives it back the core's combo. When two held bindings overlap, the bigger one wins, so L1 + A doesn't also press what A is bound to. A paused app's pause screen comes back as it was left, in Settings and on the same row; each settings category keeps its own row. Restart (below Resume, for apps with save states) resets the game. corehost draws a small overlay top left over every core's picture: notices (state saved/loaded, 2 s), fast forward and rewind while on, and with Graphics → Show FPS the frames run per second and the share of the time the core took. It is a built-in 5×7 font, drawn by each video path: on GL a scissored glClear per run of font pixels (only the scissor box of the core's GL state is touched, and restored); on Vulkan written on the CPU at one texel a font pixel into a host-visible buffer, copied into a small image and blitted out scaled, one copy and one blit (PanVK makes a render pass of every blit region). Software cores go the Vulkan way too: each frame is copied into a host-visible buffer, then into an image in the core's own format (RGB565, XRGB1555, XRGB8888), and blitted like a Vulkan core's. GL cores get EGL on the window, the pad is read from evdev. Pacing is RetroArch's default: vsync sets the speed, taken from Weston's frame callbacks in corehost's poll loop rather than a blocking present (mailbox, swap interval 0), so a window that gets no callbacks (hidden) never hangs it, it just goes on after two refreshes; and a core within 5% of the refresh rate (or of half of it, then two swaps a frame) runs at it, its sound stretched to fit (GBA's 59.73 fps runs 0.45% fast at 60 Hz) by our linear resampler to ALSA's `default` at 48 kHz. Dynamic rate control (±0.5%) keeps the 64 ms buffer half full against the panel's and the codec's clock drift. A core further off (PAL at 60 Hz) isn't stretched and its blocking writes pace it.
- PSP and Dreamcast also offer `PPSSPP (Vulkan)` and `Flycast (Vulkan)` in the per-ROM core picker (Select). These set `COREHOST_RENDER=vulkan`; leave the core's Backend option on `auto` to follow that preference. Backend changes require quitting and relaunching the game. Deploy both the launcher and corehost when updating their control table: v4 has 128 options, enough for PPSSPP's 76 plus host graphics and input bindings. The launcher also accepts v3 clients such as the image's media player. Vulkan startup logs `corehost: Vulkan: Mali-G31 MC1, instance API ..., 640x480` in `/var/log/launcher.log`; loaded GLES/EGL libraries alone do not identify the active backend because a core can link both. Corehost requests the loader's supported instance API: PPSSPP's metadata says 1.0, but its context uses newer promoted functions, which would otherwise be missing.
- The PPSSPP recipe carries fixes for live Vulkan output resizing/cropping and empty movie reads with system FFmpeg. Deploy the rebuilt `ppsspp_libretro.so` (the image's, or `OS/overlay/usr/lib/libretro/`) as well as corehost; the frontend alone cannot fix these. Regression checks: change Rendering Resolution 1×→2×→3×→1× while playing, toggle Crop to 16x9, save/load a state, and let the post-character-creation movie end naturally. Resized output must fill the same display area without repeated edges, and the movie must return to gameplay without a decoder spin.
- Menu icons are SVG strokes in `apps/common/ui/icons` (Lucide's, and ours in its style). `tools/icon-paths` turns them into `apps/common/ui/icons.inc`; run it after adding or changing one. FreeType strokes them at the size drawn, into the glyph atlas.
- Sound goes through ALSA's dmix (`/etc/asound.conf`, 48 kHz), so a game paused in the background doesn't keep the next one silent. A paused app writes nothing. Per-app volume would need a sound server (PipeWire); nothing needs it yet.
- Volume keys: Weston's `volume-keys.so` sets Line Out, and the launcher's `osd` window shows the level. The patched kiosk shell keeps that window on top without focus.
- [data/](../data) holds `/data` files that live in this repo (a new card gets them): `tar cf - -C data . | tools/ssh tar xf - -C /data`. They include the
  GPU benchmarks (glmark2, vkmark), which show Mesa's HUD (`GALLIUM_HUD`, `VK_LAYER_MESA_overlay`)
  and write their scores to `/var/log`.
- ROMs get their title and box art from the system's game pack, matched by the CRC32 of the ROM (of a zip: of its first file, from the zip's directory). The launcher computes each CRC once in the background and caches it. An image of the same name next to the ROM wins over the pack's. To build a pack, use `tools/boxart-pack "Nintendo - Game Boy Advance" <libretro-thumbnails>/Named_Boxarts out.pack`. It downloads the No-Intro DAT and metadata (genre, developer, publisher, franchise, year, players) from libretro-database. It deduplicates box art by perceptual hash, keeps the best source of each group, and stores the art at 128 px as WebP (GBA: 2.4 GB of PNGs becomes 7.5 MB). The format is in [romdb.h](../apps/launcher/romdb.h).
- On the desktop: `meson setup build apps && ninja -C build && LAUNCHER_DATA=apps/launcher/testdata build/launcher/launcher` (arrows, Enter, PgUp/PgDn). `apps/launcher/testdata` holds fake apps and ROMs with made-up icons in every format. The apps only sleep for 2 s. Its GBA ROMs carry the CRC32s of real games, so they match a GBA pack in `OS/assets/thumbnails`; `apps/emulators/fake.desktop` stands in for the image's emulators.

SuperTuxKart is deployed rather than in the image: `kas build --target supertuxkart`, copy `deploy/images/rg35xx-sp/supertuxkart/` (786 MB, too big
for a slot) to `/data/supertuxkart`. It finds `data/` next to the binary and keeps its config in `$XDG_CONFIG_HOME` (`/data/OS/config`).
Its shared libs (OpenAL Soft, vorbis, curl, ...) are in the image.

The [bindless-vk](../demos/bindless) Vulkan demo (1000 rects, textures from a descriptor-indexed sampler array) too:
`kas build --target bindless-demo`, copy `deploy/images/rg35xx-sp/bindless/` to `/data/bindless`.

The libretro cores are in the image (`/usr/lib/libretro`). A core's rebuild goes in as an overlay: `tools/ssh` it to `/data/OS/overlay/usr/lib/libretro/`, then run `overlay`.

## Bootloader

SPL/U-Boot/TF-A at 8 KiB is shared by both slots. Updating it is the one step that can still need the card reader:
```
tools/ssh 'dd of=/dev/mmcblk0 bs=1k seek=8 && sync' < …/deploy/images/rg35xx-sp/u-boot-sunxi-with-spl.bin
```
Recovery: flash the full `.wic.zst` from a PC (see [yocto.md](yocto.md)).

## Logs

`/var/log` is `/data/OS/log/0` (this boot); `/data/OS/log/1`..`4` are the boots before. syslogd/klogd write
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
