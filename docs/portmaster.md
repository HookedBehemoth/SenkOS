# PortMaster

[PortMaster](https://portmaster.games)'s ports run as they are: bash scripts that source PortMaster's
`control.txt`, started by the launcher as the player (no root, no sudo) on Weston. The OS side is
[senkos-portmaster](../yocto/meta-senkos/recipes-core/senkos-portmaster).

- **Where:** `/roms` is `/data/roms`, so PortMaster's generic paths hold: PortMaster itself in
  `/roms/ports/PortMaster`, ports' folders and `.sh` scripts in `/roms/ports`. The **Ports** system
  (`ports.system`) lists the scripts; its emulator, `portmaster.desktop`, runs one with `portmaster`.
- **PortMaster app:** the image's `/usr/share/senkos/applications/portmaster.desktop` (one of the
  same name in the card's `apps/applications` replaces or hides it) runs `portmaster` with no
  argument. PortMaster itself is a pinned release in the image (`PORTMASTER_VERSION`,
  `/usr/share/senkos/portmaster/PortMaster.zip`); `portmaster` extracts it into `roms/ports` when
  `roms/ports/PortMaster/control.txt` isn't there (aside first, then moved in), no internet needed,
  and PortMaster updates itself from then on. Its GUI (pugwash) is Python over the image's SDL2.
  The folder is the ports' runtime as well: every port sources its `control.txt`. If the extract
  fails, the launcher shows why (an app that fails: its last stderr line).
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
