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
# PE6 is low on units with the BOE ("v2") panel (docs/straps.md); gpio input returns the level
setenv fdtfile sun50i-h700-anbernic-rg35xx-sp.dtb
if gpio input PE6; then setenv fdtfile sun50i-h700-anbernic-rg35xx-sp-v2-panel.dtb; fi
load ${devtype} ${devnum}:${rootpart} ${fdt_addr_r} /boot/${fdtfile}
# The kernel shows its splash (kernel patch 0052) until Weston takes the display over. With
# /uboot/fbdev present: the kernel console (fbcon) on the LCD instead.
setenv client splash
if test -e ${devtype} ${devnum}:${distro_bootpart} fbdev; then setenv client fbdev; fi
# ro: slots are squashfs (an ext4 slot still works, init remounts it rw)
setenv bootargs root=PARTUUID=${rootuuid} rootwait ro panic=10 console=tty0 drm_client_lib.active=${client} ab=${bslot}
booti ${kernel_addr_r} - ${fdt_addr_r}
