# A/B boot. ab.txt next to this script holds slot=<a|b> (last known good) and, after
# `ab flash`, tryslot=<a|b> tries=<n>. tryslot gets 3 boots; `ab commit` in Linux makes
# it the new slot, otherwise we fall back. Slot a = partition 2, b = partition 3.
setenv slot a
setenv tryslot
setenv tries 0
if load ${devtype} ${devnum}:${distro_bootpart} ${scriptaddr} ab.txt; then
	env import -t ${scriptaddr} ${filesize}
fi
setenv bslot ${slot}
if test -n "${tryslot}"; then
	if test ${tries} -lt 3; then
		setexpr tries ${tries} + 1
		setenv bslot ${tryslot}
		env export -t ${scriptaddr} slot tryslot tries
	else
		echo "slot ${tryslot} failed 3 times, back to ${slot}"
		env export -t ${scriptaddr} slot
	fi
	fatwrite ${devtype} ${devnum}:${distro_bootpart} ${scriptaddr} ab.txt ${filesize}
fi
if test "${bslot}" = b; then setenv rootpart 3; else setenv rootpart 2; fi
part uuid ${devtype} ${devnum}:${rootpart} rootuuid
# 2 MB aligned, where booti wants the kernel: from the default 0x40080000 it moves all of it there
setenv kernel_addr_r 0x40200000
load ${devtype} ${devnum}:${rootpart} ${kernel_addr_r} /boot/Image
# The board by its straps (docs/straps.md); gpio input returns the level, so `if` is low.
# PE8 is L3, pulled up on the board where the sticks click; elsewhere it floats: read with
# the pull-down on (PE pull register 0, PE8 = 2 << 16), then the pulls as they were. The
# panel is the kernel's to tell apart (by its ID).
setexpr.l pulls *0x0300B0AC
mw.l 0x0300B0AC 0x20000
setenv board rg40xx-h
if gpio input PE8; then setenv board rg35xx-sp; fi
mw.l 0x0300B0AC ${pulls}
setenv fdtfile sun50i-h700-anbernic-${board}.dtb
load ${devtype} ${devnum}:${rootpart} ${fdt_addr_r} /boot/${fdtfile}
fdt addr ${fdt_addr_r}
fdt resize
# brightness=<level>, from save-settings
setenv brightness
if load ${devtype} ${devnum}:${distro_bootpart} ${scriptaddr} brightness.txt; then
	env import -t ${scriptaddr} ${filesize} brightness
fi
# splash=<file>, optional; else the light one while /uboot/light is there (the UI's light mode)
setenv splash splash.lz4
if test -e ${devtype} ${devnum}:${distro_bootpart} light; then setenv splash splash-light.lz4; fi
if load ${devtype} ${devnum}:${distro_bootpart} ${scriptaddr} splash.txt; then
	env import -t ${scriptaddr} ${filesize} splash
fi
if test -n "${brightness}"; then
	fdt set /backlight default-brightness-level <${brightness}>
fi
# Splash for the kernel in reserved memory; /uboot/fbdev: console instead
setenv client splash
if test -e ${devtype} ${devnum}:${distro_bootpart} fbdev; then
	setenv client fbdev
elif load ${devtype} ${devnum}:${distro_bootpart} ${ramdisk_addr_r} ${splash} && unlz4 ${ramdisk_addr_r} 0x43800000 0x200000; then
	fdt rsvmem add 0x43800000 0x${filesize}
	fdt set /chosen splash <0x43800000 0x${filesize}>
fi
# ro: slots are squashfs (an ext4 slot still works, init remounts it rw). abi.cp15_barrier=2: the
# CPU runs ARMv6's CP15 barriers (32-bit programs built for ARMv6, like Raspberry Pi builds) itself
setenv bootargs root=PARTUUID=${rootuuid} rootwait ro panic=10 console=tty0 drm_client_lib.active=${client} ab=${bslot} sysctl.abi.cp15_barrier=2
booti ${kernel_addr_r} - ${fdt_addr_r}
