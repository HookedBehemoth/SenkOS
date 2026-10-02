SUMMARY = "Video player for the pad: GStreamer playbin, Cedrus through v4l2codecs, waylandsink; the launcher drives it (launcher/appctl.h)"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"

# Built from the repo's mediaplayer/ like the launcher, with the launcher's appctl.h beside it
FILESEXTRAPATHS:prepend := "${@'/repo' if os.path.isdir('/repo/mediaplayer') else os.path.abspath(d.getVar('THISDIR') + '/../../../..')}:"
SRC_URI = "file://mediaplayer/ file://launcher/appctl.h"
S = "${UNPACKDIR}/mediaplayer"

inherit meson pkgconfig deploy

DEPENDS = "gstreamer1.0 gstreamer1.0-plugins-base gstreamer1.0-plugins-bad wayland wayland-native \
    wayland-protocols libsdl2 freetype fontconfig"
# playbin and what it plugs in besides the image's demuxers and v4l2codecs:
# audio decoders (and software video for what Cedrus can't do) from FFmpeg via gst-libav
RDEPENDS:${PN} = "liberation-fonts \
    gstreamer1.0-plugins-base-playback gstreamer1.0-plugins-base-typefindfunctions \
    gstreamer1.0-plugins-base-audioconvert gstreamer1.0-plugins-base-audioresample \
    gstreamer1.0-plugins-base-alsa gstreamer1.0-plugins-base-videoconvertscale \
    gstreamer1.0-plugins-good-audioparsers gstreamer1.0-plugins-good-avi \
    gstreamer1.0-libav"

do_deploy() {
    install -m 0755 ${B}/mediaplayer ${DEPLOYDIR}/mediaplayer
}
addtask deploy after do_compile before do_build
