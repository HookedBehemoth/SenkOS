SUMMARY = "Onion OS's 640x480 overlays for the handhelds' systems, for corehost (tools/border)"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=1ebbd3e34237af26da5dc08a4e440464"

# No release has them (Onion's RetroArch comes on its own): the commit archive, pinned by its checksum
ONION_REV = "07505ea58c7bba698d6b9220ff43946a43cac76b"
PV = "0+git${@d.getVar('ONION_REV')[:10]}"
# tools/border from the repo, as apps.bb takes apps/
FILESEXTRAPATHS:prepend := "${@'/repo' if os.path.isdir('/repo/tools') else os.path.abspath(d.getVar('THISDIR') + '/../../../..')}:"
SRC_URI = "https://github.com/OnionUI/Onion/archive/${ONION_REV}.tar.gz;downloadfilename=Onion-${ONION_REV}.tar.gz \
           file://tools/border"
SRC_URI[sha256sum] = "4caaa8ec1ab8eee99a5ebb4d514dd92f3339dff6e0ebc5ce769d576bf448cc1f"
ERROR_QA:remove = "src-uri-bad"
WARN_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/Onion-${ONION_REV}"
B = "${WORKDIR}/build"

inherit allarch python3native
DEPENDS = "python3-pillow-native"

# Left out: what needs RetroArch's CPU filters (Offset, Perfect_GBA), a palette file in the BIOS
# folder (Perfect_DMG-EX), grids over the whole screen and a GBC window of no scale
ONION_SKIP = "Perfect_GBA Perfect_DMG-EX gb_grid gb_grid_2 gbc_grid GBC-100 Nintendo-Game-Boy-Advance phosphor-gba"

python do_compile() {
    import glob
    import os
    import shutil
    import subprocess

    src = d.expand('${S}/static/build/RetroArch/.retroarch/overlay')
    out = d.expand('${B}/overlays')
    shutil.rmtree(out, ignore_errors=True)
    # Our systems (their folders' names, LAUNCHER_SYSTEM): Onion's folder, the native size
    systems = {'gba': ('GBA', 240, 160), 'gb': ('GB-GBC', 160, 144), 'gg': ('GG', 160, 144),
               'ngp': ('NGP', 160, 152), 'lynx': ('LYNX', 160, 102), 'ws': ('WS', 224, 144)}
    # What their READMEs recommend, as corehost's options and the cores'
    settings = {
        'Perfect_GBC': ['corehost_filter=Smooth', 'gambatte_gb_colorization=GBC',
                        'gambatte_gbc_color_correction=GBC only', 'gambatte_gbc_color_correction_mode=accurate',
                        'gambatte_gbc_frontlight_position=above screen', 'gambatte_mix_frames=mix'],
        'Perfect_GBP': ['corehost_filter=Smooth', 'gambatte_gb_colorization=internal',
                        'gambatte_gb_internal_palette=GB - Pocket', 'gambatte_mix_frames=mix'],
        'Perfect_GBA-centered': ['corehost_filter=Smooth', 'mgba_color_correction=OFF', 'mgba_interframe_blending=mix'],
    }
    skip = set(d.getVar('ONION_SKIP').split())
    for system, (folder, w, h) in systems.items():
        os.makedirs(os.path.join(out, system))
        for cfg in sorted(glob.glob(os.path.join(src, folder, '**', '*.cfg'), recursive=True)):
            name = os.path.basename(cfg)[:-4]
            if 'Offset' in cfg or 'Offest' in cfg or name in skip:
                continue
            subprocess.run(['python3', d.expand('${UNPACKDIR}/tools/border'), cfg, os.path.join(out, system, name),
                            str(w), str(h)] + settings.get(name, []), check=True)
    # The Game Boy's are the Color's too
    os.symlink('gb', os.path.join(out, 'gbc'))
}

do_install() {
    install -d ${D}${datadir}/corehost
    cp -r --no-preserve=ownership ${B}/overlays ${D}${datadir}/corehost/
}
FILES:${PN} = "${datadir}/corehost/overlays"
