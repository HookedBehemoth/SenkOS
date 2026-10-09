FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# The battery as "battery" in /sys/class/power_supply; the boot splash (boot.cmd hands it over)
SRC_URI += "file://0009-0124-battery-name.patch file://0052-drm-clients-add-a-boot-splash-client.patch file://splash.cfg"

# Developer builds (SENKOS_DEV, dev.yml): profiling and debugging too
SRC_URI += "${@'file://debug.cfg' if d.getVar('SENKOS_DEV') == '1' else ''}"

# The BSP's defconfig plus ours; the class runs olddefconfig on it
do_configure:prepend() {
    cat ${UNPACKDIR}/defconfig ${UNPACKDIR}/splash.cfg ${@'${UNPACKDIR}/debug.cfg' if d.getVar('SENKOS_DEV') == '1' else ''} > ${B}/.config
}
