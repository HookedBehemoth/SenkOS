SUMMARY = "Our apps, built in one go from the repo's apps/: the launcher, Weston's shell, the media player, corehost, downloadd, sysd, refreshtest, and common/ and bus/ (our D-Bus interfaces) that they share"
# Ours; the icons are Lucide's (ISC), corehost's libretro headers MIT, the shell (from
# Weston's kiosk-shell) MIT. Listed so their
# texts land in the image with ours (the launcher's Licenses screen).
LICENSE = "AGPL-3.0-only & ISC & MIT"
LIC_FILES_CHKSUM = "file://common/ui/icons/LICENSE;md5=5d494583c484398d800c8e0f74da272b \
    file://corehost/libretro.h;beginline=1;endline=21;md5=f3c64a286d68cc94b919ae6feb1fca53"

# kas-container mounts the repo read-only at /repo: edit, `tools/deploy-launcher`, no recipe
# changes. The fetcher checksums every file in it.
FILESEXTRAPATHS:prepend := "${@'/repo' if os.path.isdir('/repo/apps') else os.path.abspath(d.getVar('THISDIR') + '/../../../..')}:"
SRC_URI = "file://apps/"
S = "${UNPACKDIR}/apps"

inherit meson pkgconfig deploy

# No SDL: common/ is the window (Wayland, xdg-shell) and the pad (evdev).
# common/ui: Vulkan, HarfBuzz, fontconfig, FreeType, the image decoders.
# shell: libweston. launcher: Weston's capture protocol, ALSA, zlib, libpulse (the volume keys). mediaplayer: GStreamer.
# downloadd: curl, zstd, OpenSSL's SHA-256. D-Bus is our own (apps/bus).
# corehost: Vulkan, EGL (virtual/libgles2 for headers only, the functions come
# from eglGetProcAddress), ALSA, zlib.
DEPENDS = "weston vulkan-loader vulkan-headers glslang-native harfbuzz fontconfig freetype \
    libpng libjpeg-turbo libwebp wayland wayland-native wayland-protocols alsa-lib zlib pulseaudio curl zstd openssl \
    gstreamer1.0 gstreamer1.0-plugins-base gstreamer1.0-plugins-bad virtual/egl virtual/libgles2"

# A package each, so each app brings what it needs at runtime
PACKAGES =+ "launcher senkos-shell mediaplayer corehost downloadd sysd refreshtest"
FILES:launcher = "${bindir}/launcher"
FILES:senkos-shell = "${libdir}/weston/senkos-shell.so"
RDEPENDS:senkos-shell = "weston"
FILES:downloadd = "${bindir}/downloadd ${datadir}/dbus-1/system.d/org.senkos.Updates1.conf"
# Root's side of the UI on the system bus (init starts both, busybox-inittab)
FILES:sysd = "${sbindir}/sysd ${sbindir}/suspend ${datadir}/dbus-1/system.d/org.senkos.System1.conf"
RDEPENDS:sysd = "dbus"
# A test pattern, not in the image: copied to /data/apps/benchmarks to try a refresh rate
FILES:refreshtest = "${bindir}/refreshtest"
# TLS: the server
RDEPENDS:downloadd = "ca-certificates dbus"
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
    for app in launcher mediaplayer corehost downloadd sysd; do
        install -m 0755 ${B}/$app/$app ${DEPLOYDIR}/$app
    done
    install -m 0755 ${B}/shell/senkos-shell.so ${DEPLOYDIR}/senkos-shell.so
}
addtask deploy after do_compile before do_build
