SUMMARY = "RG35XX-SP image: kernel + busybox (init, mdev, shell), nothing else"
LICENSE = "MIT"

inherit image

IMAGE_INSTALL = " \
    base-files \
    base-passwd \
    ${VIRTUAL-RUNTIME_init_manager} \
    ${VIRTUAL-RUNTIME_dev_manager} \
    ${VIRTUAL-RUNTIME_initscripts} \
    busybox-inittab \
"
IMAGE_LINGUAS = ""
# Bring-up only: passwordless root on the LCD/serial console. Remove before anything ships.
IMAGE_FEATURES = "empty-root-password allow-empty-password allow-root-login"

# No deferred first-boot postinst scripts; don't build run-postinsts (-> util-linux)
ROOTFS_BOOTSTRAP_INSTALL = ""
