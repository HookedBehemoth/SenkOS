SUMMARY = "RetroArch as an application: the libretro frontend, for the image's cores"
DESCRIPTION = "Not in the image: retroarch.zip (retroarch.desktop, retroarch/) in the deploy directory, unpacked into the card's apps/applications"
HOMEPAGE = "https://www.retroarch.com"
LICENSE = "GPL-3.0-or-later & CC-BY-4.0"
ASSETS_PV = "1.22.0"
LIC_FILES_CHKSUM = "file://COPYING;md5=d32239bcb673463ab874e80d47fae504 \
    file://../retroarch-assets-${ASSETS_PV}/COPYING;md5=7bd61880991ed797753fcc00acae2c51"
SRC_URI = "https://github.com/libretro/RetroArch/releases/download/v${PV}/retroarch-sourceonly-${PV}.tar.xz \
    https://github.com/libretro/libretro-core-info/archive/refs/tags/v${PV}.tar.gz;downloadfilename=libretro-core-info-${PV}.tar.gz;name=info \
    https://github.com/libretro/retroarch-assets/archive/refs/tags/v${ASSETS_PV}.tar.gz;downloadfilename=retroarch-assets-${ASSETS_PV}.tar.gz;name=assets \
    file://0001-wayland-GLES2-contexts-in-GLES3-builds.patch \
    file://retroarch.cfg file://run file://anbernic-gamepad.cfg file://retroarch.desktop file://retroarch.png"
SRC_URI[sha256sum] = "2a8b1713f7f4d2b53bad3e2297e48d78f5666098cf00d583d3e08f3c213f8aa6"
SRC_URI[info.sha256sum] = "91b543fe636b4a0dc320c258b04f7b5f76cc26637e8f81cb9658536e8979b563"
SRC_URI[assets.sha256sum] = "183c638bf2e9273c975e1f46c2d19eff2f63529312856a984c0a777f973672ac"
# core-info and the assets have no release archives: tag archives, pinned by their checksums
ERROR_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/retroarch-sourceonly-${PV}"
B = "${S}"

inherit pkgconfig deploy

DEPENDS = "virtual/egl virtual/libgles2 wayland wayland-native wayland-protocols libxkbcommon udev \
    alsa-lib pipewire zlib freetype vulkan-loader vulkan-headers zip-native"

# qb configure (not autotools): CC/CXX/CFLAGS come from the environment. GLES 3.1 and Vulkan
# (builtin glslang/SPIRV-Cross for slang shaders); Wayland keyboard, udev pads, PipeWire. Only
# libraries the image has: no FFmpeg (the movie player core, recording), SDL, X11, Qt.
do_configure() {
    PKG_CONF_PATH=pkg-config ./configure --prefix=${prefix} \
        --enable-wayland --enable-egl --enable-opengles --enable-opengles3 --enable-opengles3_1 \
        --enable-vulkan --enable-slang --enable-builtinglslang --enable-spirv_cross \
        --enable-udev --enable-pipewire --enable-alsa --enable-freetype --enable-zlib \
        --disable-ffmpeg --disable-x11 --disable-kms --disable-sdl --disable-sdl2 --disable-pulse \
        --disable-jack --disable-oss --disable-qt --disable-v4l2 --disable-libusb \
        --disable-caca --disable-sixel --disable-discord --disable-libdecor --disable-opengl1 \
        --disable-vulkan_display --disable-videocore --disable-mali_fbdev
}

do_compile() {
    oe_runmake
}

# ~30 of 290 MB of menu assets: no wallpapers, other XMB themes, console ports' menus, SVG sources
ASSET_DIRS = "ozone glui rgui pkg fonts sounds xmb/monochrome"

# The folder the zip holds, next to its .desktop: retroarch/{run, retroarch, retroarch.cfg,
# icon.png, info, assets, autoconfig/udev}. ${B}/retroarch stays unstripped (gdb).
do_install() {
    Z=${WORKDIR}/zip/retroarch
    rm -rf ${WORKDIR}/zip
    install -d $Z/info $Z/assets $Z/autoconfig/udev
    ${STRIP} -o $Z/retroarch ${B}/retroarch
    install -m 0755 ${UNPACKDIR}/run $Z/run
    install -m 0644 ${UNPACKDIR}/retroarch.cfg $Z/retroarch.cfg
    install -m 0644 ${UNPACKDIR}/retroarch.png $Z/icon.png
    install -m 0644 ${UNPACKDIR}/anbernic-gamepad.cfg $Z/autoconfig/udev/
    install -m 0644 ${UNPACKDIR}/libretro-core-info-${PV}/*.info $Z/info/
    for d in ${ASSET_DIRS}; do
        install -d $Z/assets/$(dirname $d)
        cp -r --no-preserve=ownership ${UNPACKDIR}/retroarch-assets-${ASSETS_PV}/$d $Z/assets/$d
    done
    install -m 0644 ${UNPACKDIR}/retroarch.desktop ${WORKDIR}/zip/
}

do_deploy() {
    cd ${WORKDIR}/zip
    rm -f ${DEPLOYDIR}/retroarch-${PV}.zip
    zip -qr ${DEPLOYDIR}/retroarch-${PV}.zip retroarch.desktop retroarch
    ln -sf retroarch-${PV}.zip ${DEPLOYDIR}/retroarch.zip
}
addtask deploy after do_install before do_build

# Nothing for the image
PACKAGES = ""
