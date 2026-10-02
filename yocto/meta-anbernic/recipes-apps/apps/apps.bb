SUMMARY = "Our apps, built in one go from the repo's apps/: the launcher, the media player and corehost, and common/ that they share"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"

# kas-container mounts the repo read-only at /repo: edit, `tools/deploy-launcher`, no recipe
# changes. The fetcher checksums every file in it.
FILESEXTRAPATHS:prepend := "${@'/repo' if os.path.isdir('/repo/apps') else os.path.abspath(d.getVar('THISDIR') + '/../../../..')}:"
SRC_URI = "file://apps/"
S = "${UNPACKDIR}/apps"

inherit meson pkgconfig deploy

# No SDL: common/ is the window (Wayland, xdg-shell) and the pad (evdev).
# common/ui: Vulkan, HarfBuzz, fontconfig, FreeType, the image decoders.
# launcher: Weston's capture protocol, ALSA, zlib. mediaplayer: GStreamer.
# corehost: Vulkan, EGL (virtual/libgles2 for headers only, the functions come
# from eglGetProcAddress), ALSA, zlib.
DEPENDS = "vulkan-loader vulkan-headers glslang-native harfbuzz fontconfig freetype \
    libpng libjpeg-turbo libwebp wayland wayland-native wayland-protocols alsa-lib zlib \
    gstreamer1.0 gstreamer1.0-plugins-base gstreamer1.0-plugins-bad virtual/egl virtual/libgles2"

# A package each, so each app brings what it needs at runtime
PACKAGES =+ "launcher mediaplayer corehost"
FILES:launcher = "${bindir}/launcher"
FILES:mediaplayer = "${bindir}/mediaplayer"
FILES:corehost = "${bindir}/corehost ${datadir}/corehost"
# The image's fallback UI needs a font to find
RDEPENDS:launcher = "liberation-fonts"
# playbin and what it plugs in besides the image's demuxers and v4l2codecs:
# audio decoders (and software video for what Cedrus can't do) from FFmpeg via gst-libav
RDEPENDS:mediaplayer = "liberation-fonts \
    gstreamer1.0-plugins-base-playback gstreamer1.0-plugins-base-typefindfunctions \
    gstreamer1.0-plugins-base-audioconvert gstreamer1.0-plugins-base-audioresample \
    gstreamer1.0-plugins-base-alsa gstreamer1.0-plugins-base-videoconvertscale \
    gstreamer1.0-plugins-good-audioparsers gstreamer1.0-plugins-good-avi \
    gstreamer1.0-libav"

# The bare binaries, for tools/deploy-launcher (and by hand) to copy to /data
do_deploy() {
    for app in launcher mediaplayer corehost; do
        install -m 0755 ${B}/$app/$app ${DEPLOYDIR}/$app
    done
}
addtask deploy after do_compile before do_build
