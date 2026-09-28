# SenkOS docs

Research dump + plan for a small, fast-booting OS for the Anbernic **RG35XX-SP** (Allwinner H700).

- [hardware.md](hardware.md) — SoC, peripherals, SD card layouts (stock vs ours)
- [muos.md](muos.md) — what muOS is and how it boots
- [boot-analysis.md](boot-analysis.md) — where muOS spends its boot time (from dmesg)
- [plan.md](plan.md) — the plan: mainline U-Boot + Linux 7.2 via Yocto
- [yocto.md](yocto.md) — how to build and flash our image
- [dev-workflow.md](dev-workflow.md) — USB link (ssh/serial), A/B slots, `tools/ota` updates without card swapping
- [suspend.md](suspend.md) — why sleep is broken on custom firmware, stock vs muOS vs mainline, how we fix it
- [fel.md](fel.md) — FEL USB boot mode: unbrickable recovery, boot U-Boot/kernel over USB

Raw material:
- [raw/dmesg/](raw/dmesg/) — dmesg logs pulled from the device (`mtp:/RG35XX-SP/SD1/MUOS/log/dmesg/`)
- [raw/muos-init/](raw/muos-init/) — muOS init scripts (from [MustardOS/internal](https://github.com/MustardOS/internal))
- [raw/rocknix/](raw/rocknix/) — ROCKNIX H700 U-Boot defconfig + patches for reference

Key external references:
- [ROCKNIX H700 device tree / patches](https://github.com/ROCKNIX/distribution/tree/next/projects/ROCKNIX/devices/H700)
- [linux-sunxi: RG35XX Plus](https://linux-sunxi.org/Anbernic_RG35XX_Plus), [RG35XX H](https://linux-sunxi.org/Anbernic_RG35XX_H)
- [RG35XXSP CFW gist](https://gist.github.com/MrJackSpade/faa9c284a1592c8d956b7acff7c9222b) (BSP boot chain; its "no mainline display" claim is outdated)
- [U-Boot RG35XX patch series](https://patchwork.ozlabs.org/project/uboot/patch/20240819145938.503221-10-macroalpha82@gmail.com/)
