# What muOS (MustardOS) is

muOS is a community firmware that **reuses Anbernic's stock Allwinner BSP stack** and replaces the userland:

- **Bootloader:** vendor `boot0` + `boot_package` (U-Boot + DTB as `.fex` blobs, see `device/<board>/package/` in [MustardOS/internal](https://github.com/MustardOS/internal)).
- **Kernel:** Allwinner BSP **Linux 4.9.170** (2018). Out-of-tree `mali_kbase`, vendor `rtl8821cs`, `cedar_ve`, `sunxi-mmc v3.35`, Android bits (`ashmem`). Hardware-wise it's the stock kernel.
- **Rootfs:** ext4 on `mmcblk0p5`, busybox-style init with **eudev**, **dbus**, **chrony**, **pipewire**, bluez.
- **Init:** `/opt/muos/script/init/sysinit` → `S10udev` → `S30dbus` → parallel `S75bluetooth`, `S80pipewire`, `S81mode` → `S99muos` (frontend). A second async group (`async/S00entropy … S09directlink`) runs in background: network, chrony, mount of the SD, battery, etc. Copies in [raw/muos-init/](raw/muos-init/).
- **Frontend:** `muxlaunch` & friends, LVGL-based C programs ([MustardOS/frontend](https://github.com/MustardOS/frontend)). Games run via RetroArch and standalone emulators, launched by shell wrappers.
- Lots of the system is **shell scripts** (`/opt/muos/script`, ~18k files in the repo incl. assets) that read config from many small files (`GET_VAR` → one file per setting).

## Why that's slow to boot

See [boot-analysis.md](boot-analysis.md). Short version: old BSP boot chain with vendor logo stage, 4.9 kernel probing a pile of unused drivers, then dozens of shell scripts, udev, dbus, pipewire, chrony and a config-cache build (`muvar build`, up to 10 s timeout) before the frontend paints.
