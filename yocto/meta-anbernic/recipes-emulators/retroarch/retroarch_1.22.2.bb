SUMMARY = "RetroArch libretro frontend: Wayland, GLES/Vulkan, udev pads, ALSA"
# Not in the image (its libraries are): deployed with the cores and assets as retroarch-data/,
# which mirrors /data. `kas build --target retroarch`, then copy retroarch-data/ over /data.
LICENSE = "GPL-3.0-or-later"
LIC_FILES_CHKSUM = "file://COPYING;md5=d32239bcb673463ab874e80d47fae504"
SRC_URI = "https://github.com/libretro/RetroArch/releases/download/v${PV}/retroarch-sourceonly-${PV}.tar.xz \
           https://github.com/libretro/libretro-core-info/archive/refs/tags/v${PV}.tar.gz;downloadfilename=libretro-core-info-${PV}.tar.gz;name=info \
           file://0001-wayland-GLES2-contexts-in-GLES3-builds.patch \
           file://retroarch.cfg \
           file://gpio-keys-gamepad.cfg"
SRC_URI[sha256sum] = "2a8b1713f7f4d2b53bad3e2297e48d78f5666098cf00d583d3e08f3c213f8aa6"
SRC_URI[info.sha256sum] = "91b543fe636b4a0dc320c258b04f7b5f76cc26637e8f81cb9658536e8979b563"
# core-info has no release archive; its tag archive is pinned by the checksum above
ERROR_QA:remove = "src-uri-bad"
WARN_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/retroarch-sourceonly-${PV}"
B = "${S}"

inherit pkgconfig deploy
DEPENDS = "virtual/egl virtual/libgles2 wayland wayland-native wayland-protocols libxkbcommon udev alsa-lib zlib freetype ffmpeg vulkan-loader vulkan-headers"

# qb configure (not autotools): CC/CXX/CFLAGS come from the environment.
# GLES 3.1 ("gl"/"glcore") and Vulkan (dlopen'd loader, builtin glslang/SPIRV-Cross for slang shaders).
# FFmpeg: movie player core, recording. Input: Wayland keyboard + udev pads (evdev); no SDL, X11, Pulse, Qt, KMS.
do_configure() {
	PKG_CONF_PATH=pkg-config ./configure --prefix=${prefix} --sysconfdir=/data/retroarch \
		--enable-wayland --enable-egl --enable-opengles --enable-opengles3 --enable-opengles3_1 \
		--enable-vulkan --enable-slang --enable-builtinglslang --enable-spirv_cross \
		--enable-udev --enable-alsa --enable-freetype --enable-zlib --enable-ffmpeg \
		--disable-x11 --disable-kms --disable-sdl --disable-sdl2 --disable-pulse --disable-pipewire \
		--disable-jack --disable-oss --disable-qt --disable-v4l2 --disable-libusb \
		--disable-caca --disable-sixel --disable-discord --disable-libdecor --disable-opengl1 \
		--disable-vulkan_display --disable-videocore --disable-mali_fbdev
}

do_compile() {
	oe_runmake
}

do_install() {
	install -Dm0755 ${B}/retroarch ${D}${bindir}/retroarch
}

do_deploy() {
	R=${DEPLOYDIR}/retroarch-data/retroarch
	install -d $R/info $R/autoconfig/udev
	${STRIP} -o $R/retroarch ${B}/retroarch
	# Skeleton for the user config (HOME=/data: /data/.config/retroarch/retroarch.cfg) on first start
	install -m0644 ${UNPACKDIR}/retroarch.cfg $R/retroarch.cfg
	install -m0644 ${UNPACKDIR}/gpio-keys-gamepad.cfg $R/autoconfig/udev/
	install -m0644 ${UNPACKDIR}/libretro-core-info-${PV}/*.info $R/info/
}
addtask deploy after do_install before do_build

# `bitbake retroarch` builds the whole retroarch-data/ tree: cores (software-rendered up to PS1/arcade,
# GLES/Vulkan for N64/Dreamcast/PSP) and the menu assets
RETROARCH_DATA = "retroarch-assets libretro-fceumm libretro-snes9x libretro-gambatte mgba libretro-genesis-plus-gx \
    libretro-beetle-pce-fast libretro-pcsx-rearmed libretro-fbneo libretro-beetle-ngp libretro-beetle-wswan \
    libretro-stella2014 libretro-handy libretro-mupen64plus-next libretro-flycast libretro-ppsspp"
do_build[depends] += "${@' '.join(p + ':do_deploy' for p in d.getVar('RETROARCH_DATA').split())}"
