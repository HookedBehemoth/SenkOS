SUMMARY = "RG35XX-SP image: kernel + busybox (init, mdev, shell), USB gadget + ssh, A/B slots, lid sleep, launcher"
LICENSE = "MIT"

inherit image

# libsdl2: for what lives in /data (the bindless demo, SuperTuxKart, ports); our apps don't use it
# libasound, libpng, libzstd: emulator runtime (SDL dlopens libasound); GLES is in libglvnd, zlib/lzma come anyway
# openal-soft, libvorbis, libjpeg-turbo, libcurl, libcrypto: game runtime (SuperTuxKart in /data and ports like it)
# libbz2 ... opusfile: the ports' platform (docs/ports.md); the SDL2 family (SDL2_gfx our own), libmodplug and opusfile from meta-openembedded
# libav*, libgomp: the libretro cores (PPSSPP's FFmpeg, OpenMP); vulkan-loader, xkbcommon etc. come anyway
# The libretro cores: run by corehost, as the emulators in anbernic-data (anbernic-base) say
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
    portrun \
    weston-xwayland \
    pipewire \
    pipewire-pulse \
    pipewire-alsa \
    wireplumber \
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
    libbz2 libflac libtheora mpg123 libsndfile1 \
    libsdl2-image libsdl2-mixer libsdl2-ttf libsdl2-net libsdl2-gfx libmodplug opusfile cairo \
    libavcodec libavformat libavdevice libavfilter libavutil libswresample libswscale \
    libgomp \
    libretro-fceumm libretro-snes9x libretro-gambatte libretro-mgba libretro-genesis-plus-gx \
    libretro-beetle-pce-fast libretro-pcsx-rearmed libretro-fbneo libretro-beetle-ngp libretro-beetle-wswan \
    libretro-stella2014 libretro-handy libretro-mupen64plus-next libretro-flycast libretro-ppsspp \
"
# The rest of the ports' platform (docs/ports.md), and all of it again for armhf (multilib, anbernic.conf)
PORTS_PLATFORM = "libstdc++ libgcc zlib libgallium libegl-mesa libglvnd mesa-vulkan-drivers vulkan-loader \
    wayland libxkbcommon libudev libsdl2 libsdl2-image libsdl2-mixer \
    libsdl2-ttf libsdl2-net libsdl2-gfx libasound libpulse libpulse-simple openal-soft libxmu libxpm libxxf86dga libglu \
    libxi libxcursor libxrandr libxinerama libxfixes libxtst libxscrnsaver \
    libpng freetype fontconfig libogg libvorbis libopus libflac mpg123 libmodplug opusfile libtheora libsndfile1 \
    libcurl libcrypto expat libzstd libbz2 libjpeg-turbo libsqlite3 util-linux-libuuid glib-2.0 cairo dbus-lib"
IMAGE_INSTALL += "${PORTS_PLATFORM} \
    ${@' '.join('lib32-' + p for p in d.getVar('PORTS_PLATFORM').split())} \
    lib32-pipewire-alsa lib32-pipewire-modules-client-device lib32-pipewire-modules-adapter lib32-pipewire-modules-metadata \
    lib32-pipewire-modules-session-manager lib32-pipewire-spa-plugins-audioconvert"
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
	sed -i 's#^DROPBEAR_RSAKEY_DIR=.*#DROPBEAR_RSAKEY_DIR=/data/OS/dropbear#' ${IMAGE_ROOTFS}/etc/default/dropbear
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

# The launcher's Licenses screen reads /usr/share/anbernic/licenses as is (apps/launcher/licenses.h):
# a row per recipe in the image, by name, with its texts: the files its LIC_FILES_CHKSUM names
# (the project's own notices, with their holders), then the full text of each license its
# packages here declare (Yocto's generic_*). A generic text is left out when the recipe's own
# files have nearly all its words already (the project ships that license: Yocto's are the SPDX
# texts, laid out and punctuated differently from the copies projects carry), or when it's a
# notice template ("Copyright <year> <owner>": MIT, BSD, HPND) and the recipe has a notice of
# its own. Every paragraph is stored once, compared without its line breaks: the GPLs differ
# between projects only in a header or the FSF's address. Comment marks (/* * */ //) of notices
# cut from source files go.
#
#     u16 program count, license count, run count, string count      (all little-endian)
#     programs: u16 name, license count, run count      (a program's licenses and runs follow
#                                                        the ones before it's)
#     licenses: u16 string      (the declared license strings, "MIT | Apache-2.0")
#     runs: u16 first string, count      (a heading per file, then its paragraphs)
#     the strings, UTF-8, each ended by NUL: paragraphs in the order first used, then names
ROOTFS_POSTPROCESS_COMMAND:append = " anbernic_licenses"
# What LIC_FILES_CHKSUM names but isn't a license: libunistring's whole manual
ANBERNIC_LICENSE_SKIP = "libunistring/libunistring.texi"
# Generic texts that are a holder's notice rather than a template: left out like the templates
# (Yocto's ISC names the Internet Systems Consortium)
ANBERNIC_LICENSE_NOTICES = "ISC"
python anbernic_licenses () {
    import collections
    import re
    import struct
    import oe.license
    import oe.packagedata
    from oe.rootfs import image_list_installed_packages

    # Every package's declared license, by recipe
    declared = {}
    for pkg in image_list_installed_packages(d):
        info = os.path.join(d.getVar('PKGDATA_DIR'), 'runtime-reverse', pkg)
        data = oe.packagedata.read_pkgdatafile(info)
        name = os.path.basename(os.readlink(info))
        declared.setdefault(data['PN'], set()).add(' '.join((data.get('LICENSE:' + name) or data['LICENSE']).split()))

    def words(ps):
        return collections.Counter(w for p in ps for w in re.findall(r'[a-z0-9]+', p.lower()))

    def paragraphs(path):
        raw = open(path, 'rb').read()
        try:
            text = raw.decode('utf-8')
        except UnicodeDecodeError:
            text = raw.decode('latin-1')
        # The GPL's form feeds and the like would show as boxes
        text = text.replace('\r', '').replace('\f', '').replace('\t', '    ').replace('\0', '')
        # Notices cut from source files: their comment marks go, rule lines (/*****, */) whole
        if re.search(r'^\s*(/\*|//)', text, re.M):
            text = re.sub(r'^[ ]*[/*]+[ ]*$', '', text, flags=re.M)
            text = re.sub(r'^[ ]*(/\*+|\*+/|\*|//+)[ ]?', '', text, flags=re.M)
            text = re.sub(r'[ ]*\*+/[ ]*$', '', text, flags=re.M)
        text = '\n'.join(line.rstrip() for line in text.split('\n'))
        return [p.strip('\n') for p in re.split(r'\n\n+', text) if p.strip()]

    template = re.compile(r'<(year|yyyy|copyright|owner)', re.IGNORECASE)
    skip = set((d.getVar('ANBERNIC_LICENSE_SKIP') or '').split())
    notices = set((d.getVar('ANBERNIC_LICENSE_NOTICES') or '').split())
    archs = d.getVar('SSTATE_ARCHS').split()
    # A paragraph's first text, by its words: the one stored
    first = {}
    programs = []
    for pn in sorted(declared):
        licdir = next((p for p in (os.path.join(d.getVar('LICENSE_DIRECTORY'), a, pn) for a in reversed(archs))
                       if os.path.isdir(p)), None)
        if licdir is None:
            bb.fatal('anbernic_licenses: no license files for %s' % pn)
        # A package's license string that another's names in full adds nothing
        ids = {s: set(oe.license.list_licenses(s)) for s in declared[pn]}
        strings = sorted(s for s in ids if not any(ids[s] < ids[o] or (ids[s] == ids[o] and s > o) for o in ids))
        texts = [(f, paragraphs(os.path.join(licdir, f))) for f in sorted(os.listdir(licdir))
                 if f != 'recipeinfo' and not f.startswith('generic_') and pn + '/' + f not in skip]
        own = words(p for _, ps in texts for p in ps)
        for lic in sorted({oe.license.canonical_license(d, i) for s in strings for i in ids[s]}):
            path = os.path.join(licdir, 'generic_' + lic)
            if not os.path.exists(path):
                continue
            ps = paragraphs(path)
            need = words(ps)
            if sum((need & own).values()) >= 0.9 * sum(need.values()) or (own and (lic in notices or template.search('\n'.join(ps[:3])))):
                continue
            texts.append((lic, ps))
        pars = [first.setdefault(' '.join(p.split()), p) for heading, ps in texts for p in [heading] + ps]
        programs.append((pn, strings, pars))

    # Strings by first use, paragraphs before names: a program's paragraphs are mostly runs
    index = {}
    for s in [p for _, _, ps in programs for p in ps] + [s for pn, ls, _ in programs for s in [pn] + ls]:
        index.setdefault(s, len(index))
    head, names, runs = [], [], []
    for pn, strings, pars in programs:
        mine = []
        for i in (index[p] for p in pars):
            if mine and mine[-1][0] + mine[-1][1] == i:
                mine[-1][1] += 1
            else:
                mine.append([i, 1])
        head += [index[pn], len(strings), len(mine)]
        names += [index[s] for s in strings]
        runs += [x for r in mine for x in r]
    table = [len(programs), len(names), len(runs) // 2, len(index)] + head + names + runs
    if max(table) > 0xffff:
        bb.fatal('anbernic_licenses: more than u16 holds')
    out = d.expand('${IMAGE_ROOTFS}${datadir}/anbernic/licenses')
    bb.utils.mkdirhier(os.path.dirname(out))
    with open(out, 'wb') as f:
        f.write(struct.pack('<%dH' % len(table), *table))
        f.write(''.join(s + '\0' for s in index).encode('utf-8'))
}
