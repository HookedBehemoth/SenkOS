# PortMaster

[PortMaster](https://portmaster.games)'s ports run as they are: bash scripts that source PortMaster's
`control.txt`, started by the launcher as the player (no root, no sudo) on Weston. The OS side is
[senkos-portmaster](../yocto/meta-senkos/recipes-core/senkos-portmaster).

- **Where:** `/roms` is `/data/roms`, so PortMaster's generic paths hold: PortMaster itself in
  `/roms/ports/PortMaster`, ports' folders and `.sh` scripts in `/roms/ports`. The **Ports** system
  (`ports.system`) lists the scripts; its emulator, `portmaster.desktop`, runs one with `portmaster`.
- **PortMaster app:** `apps/applications/portmaster.desktop` runs `portmaster` with no argument. The
  first start downloads the latest `PortMaster.zip` from GitHub (no progress shown, ~24 MB); then
  it updates itself. Its GUI (pugwash) is Python over the image's SDL2.
- **`portmaster`** links `mod_SenkOS.txt` into PortMaster's folder (the hook file `control.txt`
  users source by `CFW_NAME`, os-release's `NAME`), sets `HOME=/data/OS/saves/ports` (ports' dot
  folders, runtime mount points), runs the script with bash, and afterwards unmounts what the port
  left mounted.
- **`mod_SenkOS.txt`:** `PM_CAN_MOUNT=N` (bind mounts become symlinks), `CUR_TTY=/dev/null`, the
  built-in pad as the OS's `SDL_GAMECONTROLLERCONFIG` (gamecontrollerdb.txt doesn't have it), Godot's
  resolution options, pysdl2's library path, and `/usr/libexec/portmaster` first on `PATH`.
- **Runtimes:** ports `mount` PortMaster's squashfs runtimes (Godot, Mono, ...). The `mount` there
  serves a squashfs image with `squashfuse_ll` (FUSE, `fusermount3` is setuid) and passes anything
  else to the real one; `umount` goes through `fusermount3`. FUSE servers are sessions of their own
  that the launcher's Quit doesn't reach, which is why `portmaster` unmounts them.
- 32-bit ports (gmloader and the like) use the image's multilib (`/usr/lib32`), audio through
  pipewire-alsa.

Not there: RetroArch (ports that use a libretro core), `sudo`.
