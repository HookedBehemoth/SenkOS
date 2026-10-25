# SenkOS docs

A small, fast-booting OS for the Anbernic **RG35XX-SP** (Allwinner H700).

[Downloads](https://senkos.dev) · [Source](https://github.com/HookedBehemoth/SenkOS) · [Discord](https://discord.gg/JgN9gedHHQ)

| Layer | What |
|---|---|
| Boot | mainline SPL (DRAM init) + TF-A BL31 + U-Boot, A/B slot choice in `boot.scr` |
| Kernel | Linux 7.2 + H700 board patches, everything built in, no initramfs |
| Root fs | read-only squashfs (lz4) per slot; `/data` ext4 for everything writable |
| Userspace | busybox init, eudev, Weston (our shell, [apps/shell](../apps/shell/shell.c)) on DRM, Mesa panfrost (GL/GLES) + PanVK (Vulkan) |
| UI | [apps/launcher/](../apps/launcher) (Wayland, Vulkan); [apps/mediaplayer/](../apps/mediaplayer) (GStreamer, Cedrus); Rockbox for music; the libretro cores run in corehost; mGBA and games live on `/data` |
| Build | Yocto (oe-core + meta-arm + meta-senkos) via kas in a container |

- [hardware.md](hardware.md): SoC, peripherals, how they map to devices
- [straps.md](straps.md): board straps (DRAM type, panel revision), how the stock boot0 picks its DRAM parameters
- [yocto.md](yocto.md): building, flashing a card, the layer
- [dev-workflow.md](dev-workflow.md): USB link (ssh/serial), A/B slots, OTA updates, `/data`, kernel iteration
- [suspend.md](suspend.md): how suspend to RAM works
- [fel.md](fel.md): FEL USB boot mode for recovery
- [updates.md](updates.md): system updates and box art packs: the server and its files, publishing, downloadd, Wi-Fi
- [portmaster.md](portmaster.md): PortMaster ports: what the OS gives their scripts, runtimes over FUSE
