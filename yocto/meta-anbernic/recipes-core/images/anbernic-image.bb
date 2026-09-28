SUMMARY = "RG35XX-SP image: kernel + busybox (init, mdev, shell), USB gadget + ssh, A/B slots, lid sleep, GPU demo"
LICENSE = "MIT"

inherit image

IMAGE_INSTALL = " \
    base-files \
    base-passwd \
    ${VIRTUAL-RUNTIME_init_manager} \
    ${VIRTUAL-RUNTIME_dev_manager} \
    ${VIRTUAL-RUNTIME_initscripts} \
    busybox-inittab \
    kernel-image \
    kernel-devicetree \
    anbernic-base \
    sleepd \
    kmscube \
    libgallium \
    libegl-mesa \
    libglvnd \
    mesa-vulkan-drivers \
    vulkan-loader \
    vulkan-tools \
    gstreamer1.0 \
    gstreamer1.0-plugins-bad-v4l2codecs \
    gstreamer1.0-plugins-bad-kms \
    gstreamer1.0-plugins-bad-waylandsink \
    gstreamer1.0-plugins-bad-videoparsersbad \
    gstreamer1.0-plugins-good-isomp4 \
    gstreamer1.0-plugins-good-matroska \
    gstreamer1.0-plugins-base-videoconvertscale \
    wireless-regdb-static \
"
IMAGE_LINGUAS = ""
# Weston is started by anbernic-base (root + seatd, kiosk shell); no fonts (no Weston clients)
BAD_RECOMMENDATIONS += "weston-init liberation-fonts"
# Bring-up only: passwordless root on the LCD, serial and USB (ssh) consoles. Remove before anything ships.
IMAGE_FEATURES = "empty-root-password allow-empty-password allow-root-login"
# Slots are squashfs (zstd): smaller, and decompressing beats the SD card's read speed.
# Writable: /var/volatile (tmpfs), /uboot (FAT, A/B state). A data partition comes later.
IMAGE_FEATURES += "read-only-rootfs"

# No deferred first-boot postinst scripts; don't build run-postinsts (-> util-linux)
ROOTFS_BOOTSTRAP_INSTALL = ""
