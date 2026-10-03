SUMMARY = "RG35XX-SP image: kernel + busybox (init, mdev, shell), USB gadget + ssh, A/B slots, lid sleep, launcher"
LICENSE = "MIT"

inherit image

# libsdl2: for what lives in /data (mGBA, the bindless demo, SuperTuxKart, ports); our apps don't use it
# libasound, libpng, libzstd: emulator runtime (SDL dlopens libasound); GLES is in libglvnd, zlib/lzma come anyway
# openal-soft, libvorbis, libjpeg-turbo, libcurl, libcrypto: game runtime (SuperTuxKart in /data and ports like it)
# libav*, libgomp: RetroArch and its cores in /data (retroarch recipe); vulkan-loader, xkbcommon etc. come anyway
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
    libsdl2 \
    vulkan-tools \
    glmark2 \
    vkmark \
    gstreamer1.0 \
    gstreamer1.0-plugins-bad-v4l2codecs \
    gstreamer1.0-plugins-bad-kms \
    gstreamer1.0-plugins-bad-waylandsink \
    gstreamer1.0-plugins-bad-videoparsersbad \
    gstreamer1.0-plugins-good-isomp4 \
    gstreamer1.0-plugins-good-matroska \
    gstreamer1.0-plugins-base-videoconvertscale \
    mediaplayer \
    liberation-fonts \
    ttf-bitstream-vera \
    libasound \
    libpng \
    libzstd \
    openal-soft \
    libvorbis \
    libjpeg-turbo \
    libcurl \
    libcrypto \
    libavcodec libavformat libavdevice libavfilter libavutil libswresample libswscale \
    libgomp \
"
IMAGE_LINGUAS = ""
# Weston is started by anbernic-base (as player, via seatd; kiosk shell)
BAD_RECOMMENDATIONS += "weston-init"
# Bring-up only: passwordless root on the LCD, serial and USB (ssh) consoles. Remove before anything ships.
IMAGE_FEATURES = "empty-root-password allow-empty-password allow-root-login"
# Slots are squashfs (lz4): cheap to decompress for U-Boot (the kernel) and Linux alike.
# Writable: /var/volatile (tmpfs), /uboot (FAT, A/B state), /data (ext4, rest of the card, see `ab`).
IMAGE_FEATURES += "read-only-rootfs"

# No deferred first-boot postinst scripts; don't build run-postinsts (-> util-linux)
ROOTFS_BOOTSTRAP_INSTALL = ""

# read-only-rootfs puts the dropbear host key on tmpfs: a new RSA key (1.6 s) on every boot.
# /data is mounted in rcS (S40anbernic), before dropbear: generate it once per card instead.
ROOTFS_POSTPROCESS_COMMAND:append = " dropbear_key_on_data"
dropbear_key_on_data () {
	sed -i 's#^DROPBEAR_RSAKEY_DIR=.*#DROPBEAR_RSAKEY_DIR=/data/dropbear#' ${IMAGE_ROOTFS}/etc/default/dropbear
}

# /etc/init.d/weston (anbernic-base) starts seatd right before Weston; its own script would
# start a second one later (and sleeps 0.1 s doing it)
ROOTFS_POSTPROCESS_COMMAND:append = " no_seatd_initscript"
no_seatd_initscript () {
	rm -f ${IMAGE_ROOTFS}${sysconfdir}/rc*.d/[SK]??seatd
}

# `smb` (anbernic-base) starts avahi with the SMB server
ROOTFS_POSTPROCESS_COMMAND:append = " no_avahi_initscript"
no_avahi_initscript () {
	rm -f ${IMAGE_ROOTFS}${sysconfdir}/rc*.d/[SK]??avahi-daemon
}
