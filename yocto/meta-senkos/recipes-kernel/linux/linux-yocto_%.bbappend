# qemu (qemu.yml): what the image needs built in, the image has no kernel modules. A feature, not
# a plain fragment: the machine's features (cfg/virtio, cfg/sound: =m) come after SRC_URI's
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append:qemuall = " file://senkos-qemu.scc file://senkos-qemu.cfg"
KERNEL_FEATURES:append:qemuall = " senkos-qemu.scc"
