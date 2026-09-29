# SenkOS docs

A small, fast-booting OS for the Anbernic **RG35XX-SP** (Allwinner H700).

| Layer | What |
|---|---|
| Boot | mainline SPL (DRAM init) + TF-A BL31 + U-Boot, A/B slot choice in `boot.scr` |
| Kernel | Linux 7.2 + H700 board patches, everything built in, no initramfs |
| Root fs | read-only squashfs (zstd) per slot; `/data` ext4 for everything writable |
| Userspace | busybox init, eudev, Weston (kiosk shell) on DRM, Mesa panfrost (GL/GLES) + PanVK (Vulkan) |
| UI | [launcher/](../launcher) (SDL2); RetroArch, mGBA and games live on `/data` |
| Build | Yocto (oe-core + meta-arm + meta-anbernic) via kas in a container |

- [hardware.md](hardware.md): SoC, peripherals, how they map to devices
- [yocto.md](yocto.md): building, flashing a card, the layer
- [dev-workflow.md](dev-workflow.md): USB link (ssh/serial), A/B slots, OTA updates, `/data`, kernel iteration
- [suspend.md](suspend.md): how suspend to RAM works
- [fel.md](fel.md): FEL USB boot mode for recovery
