FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Battery named "battery"; boot splash
SRC_URI += "file://0009-0124-battery-name.patch file://0052-drm-clients-add-a-boot-splash-client.patch file://splash.cfg"

# SENKOS_DEV (dev.yml): profiling and debugging
SRC_URI += "${@'file://debug.cfg' if d.getVar('SENKOS_DEV') == '1' else ''}"

# BSP defconfig + ours; olddefconfig runs after
do_configure:prepend() {
    cat ${UNPACKDIR}/defconfig ${UNPACKDIR}/splash.cfg ${@'${UNPACKDIR}/debug.cfg' if d.getVar('SENKOS_DEV') == '1' else ''} > ${B}/.config
}
