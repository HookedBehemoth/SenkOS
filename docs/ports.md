# Ports: declarative games from the PortMaster library

Status: `portrun` (apps/portrun), port folders in the launcher (until its window takes the focus:
the port's `splash.png`, else its cover, over "Running initial setup…" while its `setup.done` is
missing, else "Loading…"), fuse3 and squashfuse, and the ports repository (PortMaster-Decl:
converter, layers including our `love`, `tools`, `gmloader` and `compat`, the smoke-test ports
below) exist. The platform has the SDL2 family, Xwayland (Weston's, on `:0`) and PipeWire; armhf
multilib (32-bit in `/lib32`, `/usr/lib32`; ours in `/lib64`, `/usr/lib64`) is in the image.

PortMaster's ~1,400 ports are worth having; its packaging isn't. Every port is a bash script that
finds PortMaster through a hard-coded probe, sources CFW detection, `sudo`s `chmod`, `mount` and
`modprobe`, mounts runtimes itself, starts its own gamepad-to-keyboard daemon and branches on which
firmware it's running on. This proposal forks the library, turns each port into a manifest, and
moves everything that sets up a process into our runtime. Ports know nothing about the OS.

Numbers below are from PortMaster-New as of 2026-10-03: its 1,440 launch scripts, an ELF scan of
all 1,424 port folders (1,193 have native binaries, 231 run only on an engine runtime), and 12 of
its runtime squashfs files.

## What PortMaster's scripts do today

| Script does | Ports | Here |
|---|--:|---|
| `get_controls`, `SDL_GAMECONTROLLERCONFIG` | 99%, 69% | runtime: the OS's pad mapping |
| gptokeyb (pad → keyboard/mouse) | 93% (71% with a `.gptk` map) | runtime: `Keys=` |
| `$ESUDO` (sudo) | 84% | gone: nothing needs root |
| `exec > >(tee log.txt)` | 90% | runtime: stdout/stderr to a log |
| `LD_LIBRARY_PATH` | 75% | runtime: from the port's and its layers' lib dirs |
| `pm_platform_helper` (CFW hooks) | 62% | gone |
| `chmod +x` at every launch | 45% | installer: modes come from the archive |
| GameMaker via gmloader | 38% (553) | layer `gmloader` |
| `CFW_NAME` branches past the preamble | 32% (456) | gone: `libgl_${CFW}.txt`, ROCKNIX/muOS/knulli workarounds |
| writes into its own folder (`conf/`, saves) | 37% | runtime: overlay over the port, state outside it |
| runtime squashfs `mount` | 20% | runtime: layers |
| bind-mount/symlink a save dir into `$HOME` | 15% | runtime: HOME is the port's state |
| Godot (FRT 2/3, Godot 4) | 13% | layers |
| gl4es (desktop GL on GLES) | 12% | layer `gl4es`, only where Mesa's own GL won't do |
| uses `DISPLAY_WIDTH/HEIGHT` | 11% | runtime: `${WIDTH}`, `${HEIGHT}` in `Exec=`/`Env=` |
| first-run patching (`patcher.txt`, xdelta3, 7z, innoextract) | 10% | the converter, in the repo; `[Patch]` for xdelta3 and apk zips; `[Setup]` for the rest |
| `sed -i` its own files at launch | 7% | `[Setup]` (once), or `${…}` in `Exec=`/`Env=` |
| muOS splash, text-input mode, westonpack | 9%, 8%, 5% | launcher splash; `Keys=` text mode; Xwayland |
| `modprobe` (e.g. unload the touchscreen) | 9 ports | gone |

Engines (what a script finally runs): gmloader 553, native binary 496, Godot 149, LÖVE 92,
westonpack 75 (mostly Godot 4 and Java on X11), Mono/.NET 31, pyxel 13, box86 10, Ren'Py 8, rlvm 5,
box64 4, Java 2, Solarus 2. The 205 armhf-only ports are 183 GameMaker (32-bit gmloader with an
Android `libyoyo.so`), 10 box86, 4 native and 3 westonpack.

## Prior art

| | What it is | What we take |
|---|---|---|
| Flatpak | App on a runtime (a whole `/usr`), extensions (GL drivers matched to the host), a key-file `metadata`, permissions as `finish-args`, bubblewrap sandbox | key-file manifest; layers as extensions mounted at fixed paths; the sandbox; permissions declared |
| Steam Linux Runtime (pressure-vessel) | Games on a frozen Debian container; the host's graphics stack is pulled into it | graphics always come from the host; old ABIs come from a frozen layer, not the OS |
| AppImage | Bundle everything except an excludelist of host libraries | the platform *is* that list: short, stable sonames |
| Android NDK | A fixed list of stable system libraries apps may link | the platform is a contract |

We don't copy Flatpak's runtimes. A whole `/usr` per runtime means hundreds of MB on a 1 GB
device, and the image already is a fixed platform for one SoC. We take its extension model for
what's optional (engines, old ABIs, setup tools) and keep the base the host's.

## The model

```
port      /data/apps/ports/<id>/          the port: port.ini, its binaries, libs, assets
layers    /data/OS/layers/<name>.<arch>.squashfs + <name>.ini   engines, old ABIs, setup tools
state     /data/OS/saves/ports/<id>/      everything the port writes (HOME + its folder's changes)
runtime   portrun (image)                 reads port.ini, builds the process, runs it
```

### Manifest: `port.ini`

Key-file syntax like our `.desktop` and `.system` files (`apps/common/cfg.h`).

```ini
[Port]
Name=Wizard Orb
Exec=frt_3.5.2 --main-pack gamedata/index.pck --resolution ${WIDTH}x${HEIGHT} -f
Layers=frt_3.5.2;
Arch=aarch64;armhf
# wayland (default) or x11 (gets DISPLAY and Xwayland)
Display=wayland
# Where Exec runs, in the port's folder (default: the folder itself)
Dir=gamedata
Keys=wizardorb.keys
Env=FRT_NO_EXIT_SHORTCUTS=1;
# Files the player supplies (paid games); the launcher shows Instructions until they're there
Needs=gamedata/index.pck;
Instructions=Buy the game on Steam and copy its files into gamedata/.

[Patch]
# Once, after Needs are met, in order, into the port's state; the tools layer's programs, no shell
# file=old + diff: xdelta3; dir/=archive: 7-Zip unpacks it; archive=dir/: zip stores dir (an apk)
gamedata/game.droid=gamedata/data.win + gamedata/patch.xdelta3

[Setup]
# Once, after Needs are met, before the first run; again when they change. Its layers are the
# machine's architecture, whatever the game's is (tools, not the game)
Layers=gmtoolkit;tools;
Exec=tools/patchscript
```

PortMaster's `port.json` requirements (`analog_2`, `!lowres`, `2gb`, `power`) stay metadata: the
catalog hides what this device can't run. `Arch=` lists what the port ships; the installer takes
the first one the OS runs.

### Runtime: what `portrun` sets up

Every port gets the same process; nothing in the port configures it:

- **Namespaces**, unprivileged (user + mount + pid + ipc; network only with `Network=true`; `Display=x11`
  ports keep the session's IPC namespace: MIT-SHM hands Xwayland SysV shared memory):
  - `/port`: the port, read-only, under an overlayfs whose upper is in its state: games that
    write into their own folder work, and an update replaces the port without touching saves
  - `/home`: the port's state; `HOME`, `XDG_*_HOME` point into it
  - `/layers/<name>`: each layer, read-only
  - `/usr`, `/lib`, `/etc` (the platform), `/dev/dri`, `/dev/snd`, the pad's `/dev/input`, the
    Wayland socket (and the X socket with `Display=x11`), the PipeWire/Pulse sockets; a
    private `/tmp` and `/dev/shm`
- **Environment**: `PATH` and `LD_LIBRARY_PATH` from the port's `bin/`, `lib/` and each layer's
  entry in the catalog; `SDL_GAMECONTROLLERCONFIG` for this device's pad; `${WIDTH}`,
  `${HEIGHT}`, `${PORT}` expanded in `Exec=` and `Env=`. Never `LD_PRELOAD`.
- **Input**: `Keys=` maps the pad to keyboard and mouse through uinput, run by portrun outside
  the sandbox (gptokeyb's `.gptk` keys, so the conversion is mechanical). Menu + Start quits.
- **Process**: in `session/apps` like every app. The launcher's Quit (SIGTERM) is passed down to
  the game; when portrun goes, the namespaces' pid 1 and with it everything in them goes too.
  stdout/stderr to the state's `log.txt`
- **[Patch]**, then **[Setup]**, run once (the state's `setup.done`) in the same sandbox, with
  the tools layer and Setup's layers added. For files that ship with the port the converter does
  the script's first-run work in the repo (unpacking, renaming, removing the Windows leftovers) and
  keeps only the xdelta3 patches and apk zips as `[Patch]`: the original file and its patch stay

Nothing runs as root for a port, and a port sees neither the rest of `/data` nor other ports.

### Layers: squashfs files, PortMaster's own

PortMaster's runtimes work as layers unchanged. The 12 checked (FRT 3.5.2 aarch64 and armhf,
Godot 4.3, Mono 6.12, .NET 8, Zulu JRE 17, Python 3.11, Ren'Py 8.3.4, rlvm, AGS, Solarus,
gmtoolkit) are:

- **relocatable**: scripts mount them wherever they like (`$HOME/godot`, `/tmp/godot`), and none
  refers to a CFW path
- **gzip squashfs, 128 KiB blocks**, built against glibc 2.17–2.34
- **light on the system**: FRT needs SDL2 and zlib; Godot 4.3 and Mono nothing past glibc (Godot
  4.3 loads libwayland-client and libdecor itself, so it should run on Wayland without
  westonpack: to test);
  Ren'Py and Solarus the SDL2 family, OpenAL, vorbis, modplug, physfs; the JRE X11 client libs
  for AWT; Python 3.11 OpenSSL 1.1, ncurses, sqlite and SDL2 for its extension modules
- **in the repo**: `runtimes/` of PortMaster-New holds all 110 (3.1 GB), for aarch64, armhf and
  x86_64

So the fork keeps the files and adds what PortMaster encodes in each script: `<name>.ini` per
layer (`[Layer]` Name, Arch, `Path=` and `Libs=` relative to the layer, `Env=`; `Key[arch]=` for
one architecture, e.g. `Display[x86_64]=x11` for PortMaster's x86_64 FRT, an X11 Godot build).
portrun mounts `/dev/fuse` itself in the port's user namespace and hands the fd to
`squashfuse_ll`: no fusermount, nothing setuid.

Python layers keep their bytecode: Ren'Py's ships its `.pyc` (and ports their `.rpyc`, which
Ren'Py rewrites into `/port`, i.e. the state); Pyxel's writes into `/home/.cache/pycache`
(`PYTHONPYCACHEPREFIX`, which would make Python ignore shipped `__pycache__`, so not for
Ren'Py).

Mounting them without root:

| | How | Cost |
|---|---|---|
| squashfuse (recommended) | portrun mounts the layer inside the port's user namespace; FUSE allows that, kernel squashfs doesn't | `CONFIG_FUSE_FS` + squashfuse (~100 KB); decompression in userspace; to measure |
| a root mount service | like sleepd: loop-mounts layers read-only, `nosuid,nodev`, only files whose hash a signed catalog names, from a root-owned directory | `BLK_DEV_LOOP` + `SQUASHFS_ZLIB`; a signing key; root parses the image |
| unpack | `unsquashfs` once into a directory | 2.5–3.5× the space (Mono: 249 MB → 870 MB) |

Layers we need:

| Layer | For | Ports | From |
|---|---|--:|---|
| `gmloader` | GameMaker (gmloader-next: the build most ports carry on aarch64, since the 2026 ones fail on games linking libOpenSLES; the newest on armhf); the port links `lib/<ABI>` to the layer's Android libraries, which gmloader opens relative to its working directory | 553 | new, from the ports' copies |
| `frt_*`, `godot_*` | Godot 2–4 | ~220 | PortMaster |
| `love` 11 | LÖVE | 92 | new (ports bundle it) |
| `mono`, `dotnet` | XNA/FNA games | 31 | PortMaster |
| `zulu*` | Java | ~15 | PortMaster |
| `python`, `pyxel_*`, `renpy_*`, `rlvm`, `solarus`, `ags` | the rest | ~30 | PortMaster |
| `compat` | old sonames ports link: libjpeg.so.8, libFLAC.so.8, OpenSSL 1.1, gnutls, nghttp2, SDL 1.2 and its image/mixer, ICU 63/66, libffi.so.7, libzip.so.4, theora 1.1; not yet libnsl.so.1, Boost 1.67, OpenEXR/libraw; not FFmpeg 4 (left out, below) | ~85 | new, from the Debian 10/11 and Ubuntu 20.04 packages ports were built against, with what they need that the platform lacks (`tools/compat-layer.py`, ~33 MB) |
| `gl4es` | desktop GL games where Mesa's own GL isn't enough | (12%, to test) | ports' copies |
| `box64` | x86-64 Linux games | 4 | ports' copies |
| `gmtoolkit`, `tools` | `[Setup]`: UndertaleModTool CLI; bash, GNU coreutils/sed/grep/find, xdelta3, 7z, innoextract, unzip, dos2unix, ffmpeg | 150 | PortMaster; new |

The converter attaches `compat` to a port when one of its binaries needs a soname only that
layer has. `tools` is where PortMaster's shell requirements (GNU bash and coreutils, see below)
end up: only setup scripts need them.

### What a port brings

Its binaries and every library that isn't the platform's or a layer's (PortMaster's ports bundle
theirs in `libs.aarch64/` already), its assets or `Needs=`, `port.ini`, its keys, and a
`[Setup]` when the player's files need converting. Nothing that names a device or an OS.

## What has to be in the OS

What ports' binaries link that they don't bundle (ELF scan; 1,002 ports with aarch64 binaries,
259 with armhf ones, 68 with both):

| Platform added | aarch64 ports resolved | armhf |
|---|--:|--:|
| glibc, libstdc++, libgcc_s | 9% | 10% |
| + SDL2, SDL2_image/_mixer/_ttf/_net/_gfx | 31% | 18% |
| + ALSA, libpulse(-simple), OpenAL | 32% | 20% |
| + X11 client libs | 33% | 20% |
| + zlib (+19%), bzip2 (+17%), vorbisfile, modplug, png, freetype, curl, vorbis, cairo, expat, zstd, mpg123, theora, fontconfig, sqlite, uuid, glib, sndfile, ogg, opusfile, dbus | 91% | 94% |

Theora is 1.2 now (`libtheora.so.1`, `libtheoradec.so.2`, Yocto and Arch alike): ports that link
1.1's `.so.0`/`.so.1` bring them or get them from `compat`.

What's left is 100 ports (aarch64 and armhf together):

| | Ports | Here |
|---|--:|---|
| only old sonames: jpeg.so.8 44, FLAC.so.8 13, OpenSSL 1.1, gnutls/nghttp2 | 72 | `compat` |
| more old sonames: SDL 1.2 (rlvm, Ken's Labyrinth, Stunt Car Racer), ICU 63/66, libffi.so.7, libzip.so.4, libnsl.so.1, Boost 1.67, WildMidi 2, OpenEXR/libraw (Waking Mars) | 11 | `compat` |
| small stable libs the platform lacks: libXmu, libXpm (westonpack brought it), libXxf86dga, libGLU, libgthread, avahi-client | 10 | platform |
| the library is in its layer (libmonosgen in Mono) | 1 | nothing to do |
| a bundled copy of a platform library built for another CFW: SDL2 linked to Rockchip's librga (Jedi Academy, Jedi Outcast), OpenAL linked to sndio (Minecraft, Revenge of the Titans; their libjawt is the JRE layer's) | 4 | the fork drops the copy |
| links `libSDL2.so` without a version (Biolab) | 1 | the fork fixes the link (`patchelf --replace-needed`) |
| libraries the port takes from its CFW: JACK and fluidsynth 3 (Doom Engines; PipeWire's libjack and a fluidsynth cover them); OpenMW's bundled FFmpeg lacks 20 libraries | 2 | platform; OpenMW runs natively here |
| harmless: .NET's optional tracing provider (lttng), an Android library a loader stubs (libmediandk) | 2 | nothing to do |
| Qt 6 (Dust Racing 2D), the Odroid Go's libgo2 and libmali (old Moonlight) | 2 | left out |

An unresolved soname isn't always a broken port (a bundled library that's never loaded), and a
resolved one isn't always a working port (SDL and the GL libraries are loaded at runtime). The
fork's rule from this: a port doesn't bundle what the platform has; the converter drops those
copies unless the port needs a patched one.

GLES and EGL rarely show up in the scan because SDL loads
them at runtime; they are the platform all the same. Newest glibc symbol any port needs: 2.38.

| | What |
|---|---|
| kernel | `USER_NS`, `PID_NS`, `IPC_NS`, `NET_NS`, `OVERLAY_FS`, `FUSE_FS` (layers), `UINPUT` (on); `COMPAT` (armhf); `ZRAM` + `SWAP` (on by default, so no port runs `mkswap`) |
| platform, aarch64 and armhf (multilib) | glibc, libstdc++/libgcc_s, zlib, bzip2; Mesa (EGL, GLES, GL, Vulkan) with glvnd; libwayland-client/-egl, libxkbcommon, libudev; SDL2 and its image/mixer/ttf/net/gfx; ALSA, libpulse, OpenAL; X11 client libs (with Xmu, Xpm, Xxf86dga), GLU; png, freetype (with bzip2: ports call `FT_Stream_OpenBzip2`), fontconfig, ogg/vorbis/opus, FLAC, mpg123, modplug, theora, sndfile, curl, expat, zstd, sqlite, uuid, glib, cairo/pixman, dbus |
| display | Weston with Xwayland (`Display=x11`: westonpack's Godot 4 and Java ports, box64's GLX games, the JRE's AWT) |
| audio | PipeWire with pipewire-pulse, pipewire-alsa and its libjack: Pulse clients (9% link libpulse), ALSA and JACK clients share one mixer (armhf ALSA through a 32-bit build of just the pipewire-alsa plugin); fluidsynth |
| runtime | `portrun`, the key mapper, the installer (catalog, download, hash, unpack, `[Setup]`), squashfuse |

**Multilib in the OS** rather than a 32-bit layer keeps the installer OS-agnostic: an OS says
which architectures it runs, and the catalog's `Arch=` does the rest. armhf brings back 205 ports
(183 of them GameMaker) for `CONFIG_COMPAT` and the 32-bit set above. The armhf scan needs
OpenAL, freetype and bzip2 far more than aarch64 does (75% each: the 32-bit gmloader links them).

SDL2 picks X11 over Wayland when `DISPLAY` is set, so only `Display=x11` ports get it.

## PortMaster's CFW guide, checked

[PortMaster_CFW.md](https://gist.github.com/bmdhacks/f26dbb60dc0ff0b9bc8e043fac8b813b) lists
what a CFW must provide for the scripts. Against this plan:

| Guide says | Here |
|---|---|
| GNU bash 4 as `/bin/bash`, GNU coreutils/findutils/sed/grep, psmisc | `tools` layer, for `[Setup]` only; launching runs no shell |
| `mount`, `sudo`/`doas`, `systemctl`, `/dev/tty*` writable, `chmod 666 /dev/uinput` | gone: portrun sets up the process, uinput belongs to the key mapper |
| tar, unzip, gzip, xdelta3, 7z, innoextract, dos2unix, ffmpeg | the installer unpacks; the rest is `tools` |
| squashfs (zlib, lzo, xz, zstd) + loop, `max_loop=16` | squashfuse in the user namespace; PortMaster's runtimes are all gzip |
| uinput | yes, for the key mapper |
| tmpfs `/tmp`, `/dev/shm` | private ones per port |
| zram/swap, set up by ports | the OS's, always on |
| glibc ≥ 2.29, libstdc++, SDL2 + image/mixer/ttf, png, jpeg, freetype, fontconfig, OpenAL, ALSA | platform (the scan agrees, plus bzip2, which the guide misses and 22% need) |
| Debian 11 compat libs in the OS | `compat` layer (also Debian 10 and Ubuntu 20.04 sonames: ICU 63/66, Boost 1.67) |
| GLES2, gl4es bundled, Panfrost preferred, KMS optional | Mesa Panfrost on Wayland; `gl4es` layer |
| ALSA required, Pulse strongly recommended, PipeWire nice | PipeWire with both |
| exFAT for drag and drop | `/data` stays ext4: UMS, MTP and SMB give a PC access |
| armhf compat, box86/box64 bundled | multilib; box86 is armhf, so it works with multilib |
| Python 3 with lzma (harbourmaster) | not needed: our installer |
| `$DEVICE_*`, `$DISPLAY_*`, `$CFW_NAME`, `$ESUDO`, `$GPTOKEYB`, `$directory`, `$TASKSET` | `${WIDTH}`/`${HEIGHT}` and catalog filtering; the rest has no meaning here |
| no system `LD_PRELOAD` | portrun never sets it |
| CFW detection, `platform.py`, `mod_*.txt` | not applicable: we don't run PortMaster |

The guide's smoke tests make a good first test set: DevilutionX or VVVVVV (native), Spelunky
Classic HD (gmloader), 6 Feet Under (gmtoolkit + .NET), Mr. Rescue (LÖVE), Cats on Mars (FRT 3),
Slime 3K Demake (Godot 4, was westonpack), Mindustry (JRE 17), Megaball (pyxel), Cute Fame
(Ren'Py), Shards of God (AGS).

## The fork

- Fork PortMaster-New. Keep its zips, assets, runtimes and porter credits; replace each
  `<Port>.sh` with `port.ini` and each `.gptk` with keys, and drop bundled copies of what a layer
  provides (553 gmloaders, LÖVE, gl4es, box64).
- A converter does most of it: the gmloader (553), Godot (149) and LÖVE (92) scripts are nearly
  identical, so ~800 ports convert mechanically. The ~500 native ones get a manifest from their
  `Exec`, `LD_LIBRARY_PATH` and env lines, then a test run. westonpack ones become `Display=x11`.
- Upstream changes come in by re-running the converter on new and changed ports.
- Our catalog (ports and layers, like `ports.json`) is generated from the manifests and published
  with our releases.

## Testing off the device

The runtime is the same on any Linux, so a PC running another distribution tests that ports stay
OS-agnostic:

- **x86_64, natively**: 93 ports ship x86_64 binaries and 150 more run on a runtime PortMaster
  also builds for x86_64 (FRT, Godot 4, AGS, the JRE, pyxel). portrun itself runs on the PC
  (`portrun -q -s <state> -l <layers> <port>`, `-q` without sound devices; `PORTRUN_SIZE=640x480`;
  squashfuse_ll on `PATH`) in a headless nested kwin with Xwayland: Slime 3K Demake (Godot 4.3
  on Wayland), Turbo Table Tennis and Cats on Mars (FRT, X11), DevilutionX and Megaball (Pyxel)
  run; Wizard Orb stops at its `Needs=`. Shards of God's AGS layer needs 1.1's libtheora, which
  the PC doesn't have: the port brings it for aarch64 only.
- **aarch64 and armhf, emulated**: qemu-user with binfmt and an aarch64 + armhf sysroot with
  llvmpipe (`tools/ports-sysroot`: Debian trixie with the platform; the image's Mesa has only
  Panfrost). `portrun -r <sysroot>` takes the platform from there, and runs what the sysroot has
  loaders for. The GameMaker, aarch64- and armhf-only ports; slow, for correctness.
- **On the device**: Panfrost, speed, the pad. Debug tools (gdb, strace, ltrace, valgrind) as an
  overlay in `OS/overlay`, not in the image.

## Left out

- **PortMaster's GUI, harbourmaster, gptokeyb, control.txt, mod_*.txt, westonpack.** Not run,
  not shipped.
- **FFmpeg 4 (avcodec.so.58), for now.** Its ports (Aleph One's three Marathons, ctp2, both
  Moonlights) are to be rebuilt from source against a current FFmpeg instead. Ports that bundle
  their own FFmpeg (q2pro, nox-decomp, OpenEnroth, OpenChaos, songo5, tinamp) work as they are.
