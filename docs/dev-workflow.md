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
p2 ext4 512M slot a: rootfs, /boot/Image + dtb
p3 ext4 512M slot b: same
```

`boot.scr` ([boot.cmd](../yocto/meta-anbernic/recipes-bsp/u-boot/files/boot.cmd)) reads `ab.txt`:
- `slot=a|b` is the last known good slot.
- After an update, `tryslot=` and `tries=` are also set. `tryslot` gets 3 boots, and U-Boot writes the counter back to FAT before each one.

After 3 failed tries, U-Boot boots `slot` again and drops `tryslot`.

Things that count as a failed try:
- A hang in U-Boot or the early kernel: the U-Boot-armed SoC watchdog (16 s) resets the board.
- A kernel panic: `panic=10` reboots.
- A userspace hang: busybox `watchdog` resets the board.
- A slot that boots but isn't committed within 120 s: `ab boot` reboots it. This catches a broken USB link, which would otherwise lock us out.

The commit comes from the host. Once ssh into the new slot works, `tools/ota` runs `ab commit`.

```
tools/ota                      # flash the latest build's rootfs.ext4 into the other slot, reboot, commit
tools/ota path/to/rootfs.ext4
tools/ssh ab status
```

Device side: [ab](../yocto/meta-anbernic/recipes-core/anbernic-base/files/ab) with `status|mount|flash|commit|boot`.

## Bootloader

SPL/U-Boot/TF-A at 8 KiB is shared by both slots. Updating it is the one step that can still need the card reader:
```
tools/ssh 'dd of=/dev/mmcblk0 bs=1k seek=8 && sync' < …/deploy/images/rg35xx-sp/u-boot-sunxi-with-spl.bin
```
Recovery: flash the full `.wic.zst` from a PC (see [yocto.md](yocto.md)).
