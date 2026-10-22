SUMMARY = "libretro's slang shader presets for the handhelds, built for corehost (tools/slangc)"
# Each shader's own, from its header; for the few without one, where it comes from
# (libretro-shaders-provenance); stock.slang is a passthrough.
LICENSE = "GPL-1.0-or-later & GPL-2.0-or-later & GPL-3.0-or-later & MIT & ISC & MPL-2.0 & CC0-1.0 & BSD-2-Clause & PD"
LIC_FILES_CHKSUM = " \
    file://handheld/shaders/bevel.slang;beginline=3;endline=22;md5=2288e3761a7476517e74e283bb1149e1 \
    file://handheld/shaders/gameboy/shader-files/gb-pass0.slang;beginline=11;endline=53;md5=e5ec2d81afc761476a030796edc9fb2e \
    file://handheld/shaders/retro-v2.slang;beginline=3;endline=22;md5=e61928a4992260aec56c83efd1e9c93b \
    file://handheld/shaders/zfast_lcd.slang;beginline=3;endline=17;md5=3bee1f181c7df0938c23d414edabaea1 \
    file://handheld/shaders/simpletex_lcd/simpletex_lcd.slang;beginline=3;endline=46;md5=4047b541780c4b85c0facb9fd37f5aa7 \
    file://handheld/shaders/sameboy-lcd.slang;beginline=3;endline=27;md5=8bb7661e4531456e2dda1e1f3d72b8ab \
    file://handheld/shaders/authentic_gbc/authentic_gbc.slang;beginline=3;endline=24;md5=73b8fc870c3565a06786724ade4bf855 \
    file://handheld/shaders/mgba/agb001.slang;beginline=3;endline=12;md5=62ae089a2a1f965b87f4671282a01a08 \
    file://handheld/shaders/mgba/ags001.slang;beginline=3;endline=12;md5=b4414f0e22aeaaefc9bd7c1ebea08a80 \
    file://pixel-art-scaling/shaders/pixellate.slang;beginline=3;endline=17;md5=61e522f191fe9cf13e2b56ecf3e021a0 \
    file://pixel-art-scaling/shaders/aann.slang;beginline=3;endline=5;md5=94b8f99c53f109b50d6cd8a03f5375a1 \
    file://crt/shaders/zfast_crt/zfast_crt_impl.inc;beginline=1;endline=19;md5=2bcae4ebf421eb56f9aedd9687ac3e86 \
    file://crt/shaders/crt-geom.slang;beginline=65;endline=85;md5=c765d3a0e562c297ee46006d4e9b31b9 \
    file://crt/shaders/crt-easymode.slang;beginline=61;endline=63;md5=dcad18aed06f764b25c0d33478916cf3 \
    file://${UNPACKDIR}/libretro-shaders-provenance;md5=39fb25cb2fb84baad11dd763f7c25473 \
    file://${UNPACKDIR}/libretro-shaders-bsd;md5=737718f6a25c4779170b26f8dd493e5c \
"

# No releases: the commit archive, pinned by its checksum
SLANG_SHADERS_REV = "e1d75632a205c70f14a4cc947c46d5abb7b3f7f1"
PV = "0+git${@d.getVar('SLANG_SHADERS_REV')[:10]}"
# tools/slangc from the repo, as apps.bb takes apps/
FILESEXTRAPATHS:prepend := "${@'/repo' if os.path.isdir('/repo/tools') else os.path.abspath(d.getVar('THISDIR') + '/../../../..')}:"
SRC_URI = "https://github.com/libretro/slang-shaders/archive/${SLANG_SHADERS_REV}.tar.gz;downloadfilename=slang-shaders-${SLANG_SHADERS_REV}.tar.gz \
           file://tools/slangc file://libretro-shaders-provenance file://libretro-shaders-bsd"
SRC_URI[sha256sum] = "b8bc2fbb31a754ed41ee4ab309ece63c08123ada8e5e7c5142621461f165bbd9"
ERROR_QA:remove = "src-uri-bad"
WARN_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/slang-shaders-${SLANG_SHADERS_REV}"
B = "${WORKDIR}/build"

inherit allarch python3native
DEPENDS = "glslang-native"

# Cheap enough for the G31 at 640x480, for the systems on the device (the launcher's Shader
# option lists them by these paths)
SHADER_PRESETS = " \
    handheld/lcd-grid-v2 handheld/zfast-lcd handheld/dot handheld/sameboy-lcd handheld/authentic_gbc_fast \
    handheld/gameboy handheld/gameboy-pocket handheld/gameboy-light handheld/lcd1x handheld/lcd3x \
    handheld/gb-palette-dmg handheld/gb-palette-pocket handheld/retro-v2 handheld/simpletex_lcd handheld/bevel handheld/agb001 handheld/ags001 \
    handheld/color-mod/gba-color handheld/color-mod/gbc-color handheld/color-mod/sp101-color handheld/color-mod/NSO-gba-color handheld/color-mod/NSO-gbc-color handheld/color-mod/gba-fast-color-fix \
    pixel-art-scaling/sharp-bilinear-simple pixel-art-scaling/sharp-shimmerless pixel-art-scaling/pixellate \
    pixel-art-scaling/aann crt/zfast-crt crt/crt-geom crt/crt-easymode crt/fakelottes \
"

do_compile() {
    rm -rf ${B}/shaders
    python3 ${UNPACKDIR}/tools/slangc ${S} ${B}/shaders ${@' '.join(p + '.slangp' for p in d.getVar('SHADER_PRESETS').split())}
}

do_install() {
    install -d ${D}${datadir}/corehost/shaders
    cp -r --no-preserve=ownership ${B}/shaders/. ${D}${datadir}/corehost/shaders/
}
FILES:${PN} = "${datadir}/corehost/shaders"
