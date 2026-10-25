SUMMARY = "SenkOS image: kernel + busybox (init, mdev, shell), USB gadget + ssh, A/B slots, lid sleep, launcher"
LICENSE = "MIT"

inherit image

# The machine's own packages (Wi-Fi driver and firmware: MACHINE_EXTRA_RRECOMMENDS)
# Wi-Fi (senkos-base's wifi): wpa_supplicant, the regulatory database
# downloadd: system updates and game packs over the network (TLS: ca-certificates)
# libsdl2: for what lives in /data; our apps don't use it
# libasound, libpng, libzstd: emulator runtime (SDL dlopens libasound); GLES is in libglvnd, zlib/lzma come anyway
# openal-soft, libvorbis, libjpeg-turbo, libcurl, libcrypto: game runtime (games in /data)
# libbz2 ... opusfile: the games' platform; the SDL2 family (SDL2_gfx our own), libmodplug and opusfile from meta-openembedded
# libav*, libgomp: the libretro cores (PPSSPP's FFmpeg, OpenMP); vulkan-loader, xkbcommon etc. come anyway
# fusermount3, libfuse2: AppImages mount themselves. gmp ... fribidi: what AppImages leave to the host
# (AppImage's excludelist) and we'd otherwise lack
# The libretro cores: run by corehost, as the emulators in senkos-data (senkos-base) say; corehost's
# shader presets and overlays
# senkos-portmaster: PortMaster's ports as they are (bash, Python, /roms)
IMAGE_INSTALL = " \
    base-files \
    base-passwd \
    ${VIRTUAL-RUNTIME_init_manager} \
    ${VIRTUAL-RUNTIME_dev_manager} \
    ${VIRTUAL-RUNTIME_initscripts} \
    busybox-inittab \
    kernel-image \
    kernel-devicetree \
    ${MACHINE_EXTRA_RDEPENDS} ${MACHINE_EXTRA_RRECOMMENDS} \
    senkos-base \
    senkos-portmaster \
    sysd \
    downloadd \
    wpa-supplicant wpa-supplicant-cli wireless-regdb-static iw \
    ca-certificates \
    tzdata \
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
    weston-xwayland \
    fusermount3 libfuse2 gmp libgpg-error libusb1 fribidi \
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
    libretro-stella2014 libretro-handy libretro-gpsp libretro-freej2me libretro-mupen64plus-next libretro-flycast libretro-ppsspp \
    libretro-shaders libretro-overlays \
"
# The rest of the games' platform (binaries built on Debian), and all of it again for armhf (multilib, senkos.conf)
PORTS_PLATFORM = "libstdc++ libgcc zlib libgallium libegl-mesa libglvnd mesa-vulkan-drivers vulkan-loader \
    wayland libxkbcommon libudev libsdl2 libsdl2-image libsdl2-mixer \
    libsdl2-ttf libsdl2-net libsdl2-gfx libasound libpulse libpulse-simple openal-soft libxmu libxpm libxxf86dga libglu \
    libxi libxcursor libxrandr libxinerama libxfixes libxtst libxscrnsaver \
    libpng freetype fontconfig libogg libvorbis libopus libflac mpg123 libmodplug opusfile libtheora libsndfile1 \
    libcurl libcrypto expat libzstd libbz2 libjpeg-turbo libsqlite3 util-linux-libuuid glib-2.0 cairo dbus-lib"
IMAGE_INSTALL += "${PORTS_PLATFORM}"
IMAGE_INSTALL += "${@'' if not d.getVar('MULTILIBS') else ' '.join('lib32-' + p for p in d.getVar('PORTS_PLATFORM').split()) + ' \
    lib32-pipewire-alsa lib32-pipewire-modules-client-device lib32-pipewire-modules-adapter lib32-pipewire-modules-metadata \
    lib32-pipewire-modules-session-manager lib32-pipewire-spa-plugins-audioconvert'}"
# Libraries by their development names too (libSDL2.so, libopenal.so, libGL.so): what dlopens them by
# those (SoLoud's SDL2 backend; engines and runtimes in /data) finds no -dev packages here. Each
# installed recipe's -dev package's .so links, taken from its ipk, where their targets are installed.
# After read-only-rootfs's removals (shadow's libsubid), while the package list is still there.
ROOTFS_POSTUNINSTALL_COMMAND:append = " senkos_dev_names"
python senkos_dev_names () {
    import glob
    import json
    import subprocess
    import oe.packagedata
    from oe.rootfs import image_list_installed_packages

    rootfs = d.getVar('IMAGE_ROOTFS')
    pkgdata = d.getVar('PKGDATA_DIR')
    # Each installed package's -dev and its recipe's (gcc-runtime has libstdc++-dev)
    devs = set()
    for p in image_list_installed_packages(d):
        reverse = os.path.join(pkgdata, 'runtime-reverse', p)
        devs |= {os.path.basename(os.readlink(reverse)) + '-dev', oe.packagedata.read_pkgdatafile(reverse)['PN'] + '-dev'}
    links = {}
    for dev in sorted(devs):
        info = os.path.join(pkgdata, 'runtime', dev)
        if not os.path.exists(info):
            continue
        data = oe.packagedata.read_pkgdatafile(info)
        if not any(f.endswith('.so') for f in json.loads(data.get('FILES_INFO:' + dev, '{}'))):
            continue
        name = data.get('PKG:' + dev, dev)
        ipks = glob.glob(os.path.join(d.getVar('DEPLOY_DIR_IPK'), '*', '%s_%s-%s_*.ipk' % (name, data['PKGV'], data['PKGR'])))
        if len(ipks) != 1:
            bb.fatal('senkos_dev_names: %s: %d ipks' % (name, len(ipks)))
        listing = subprocess.run('ar p "%s" data.tar.zst | zstd -dc | tar -tv' % ipks[0], shell=True,
                                 check=True, capture_output=True, text=True).stdout
        for line in listing.splitlines():
            if line.startswith('l') and ' -> ' in line:
                path, target = line.split(None, 5)[5].split(' -> ')
                links['/' + os.path.normpath(path)] = target
    # Where the target is (in the image; a link may point at another of these: libSDL2.so)
    def installed(path, target):
        t = target if target.startswith('/') else os.path.join(os.path.dirname(path), target)
        return os.path.exists(os.path.join(rootfs, t.lstrip('/'))) or \
            (os.path.normpath(t) in links and os.path.lexists(os.path.join(rootfs, os.path.normpath(t).lstrip('/'))))
    added = True
    while added:
        added = False
        for path, target in links.items():
            link = os.path.join(rootfs, path.lstrip('/'))
            if path.endswith('.so') and not os.path.lexists(link) and installed(path, target):
                os.symlink(target, link)
                added = True
}

# Developer builds (SENKOS_DEV, dev.yml): debuggers and tracers
IMAGE_INSTALL += "${@'gdb gdbserver strace perf trace-cmd' if d.getVar('SENKOS_DEV') == '1' else ''}"
IMAGE_LINGUAS = ""
# Weston is started by senkos-base (as player, via seatd; our shell, apps/shell)
BAD_RECOMMENDATIONS += "weston-init"
# The system bus: sysd's org.senkos.System1 (init starts it, busybox-inittab). The 32-bit libdbus
# recommends a daemon of its own; by the package's name (debian renaming): opkg matches that
IMAGE_INSTALL += "dbus"
BAD_RECOMMENDATIONS += "lib32-dbus-1"
# Bring-up only: passwordless root on the LCD, serial and USB (ssh) consoles. Remove before anything ships.
IMAGE_FEATURES = "empty-root-password allow-empty-password allow-root-login"
# Slots are squashfs (lz4): cheap to decompress for U-Boot (the kernel) and Linux alike.
# Writable: /var/volatile (tmpfs), /uboot (FAT, A/B state), /data (ext4, rest of the card, see `ab`).
IMAGE_FEATURES += "read-only-rootfs"

# No deferred first-boot postinst scripts; don't build run-postinsts (-> util-linux)
ROOTFS_BOOTSTRAP_INSTALL = ""

# read-only-rootfs puts the dropbear host key on tmpfs: a new RSA key (1.6 s) on every boot.
# /data is mounted in rcS (S40senkos), before dropbear: generate it once per card instead.
ROOTFS_POSTPROCESS_COMMAND:append = " dropbear_key_on_data"
dropbear_key_on_data () {
	sed -i 's#^DROPBEAR_RSAKEY_DIR=.*#DROPBEAR_RSAKEY_DIR=/data/OS/dropbear#' ${IMAGE_ROOTFS}/etc/default/dropbear
	# The launcher's Debug (sysd's "ssh ..."): SSH off, or on every network rather than the USB
	# cable's alone (senkos.init gives it 10.55.0.1 before rc5 starts dropbear). Ifs, not &&: the
	# init script sources this under set -e, and a false last command would end it.
	cat >> ${IMAGE_ROOTFS}/etc/default/dropbear <<-EOF
		if [ -e /data/OS/dropbear/disabled ]; then NO_START=1; fi
		if [ ! -e /data/OS/dropbear/everywhere ]; then DROPBEAR_PORT=10.55.0.1:22; fi
	EOF
}

# /etc/init.d/weston (senkos-base) starts seatd right before Weston; its own script would
# start a second one later (and sleeps 0.1 s doing it). It starts the bus too, init bluetoothd
# (busybox-inittab): not their own scripts.
ROOTFS_POSTPROCESS_COMMAND:append = " no_daemon_initscripts"
no_daemon_initscripts () {
	rm -f ${IMAGE_ROOTFS}${sysconfdir}/rc*.d/[SK]??seatd
	rm -f ${IMAGE_ROOTFS}${sysconfdir}/rc*.d/[SK]??dbus-1
	rm -f ${IMAGE_ROOTFS}${sysconfdir}/rc*.d/[SK]??bluetooth
}

# Bluetooth audio (SBC); pairings on the card
IMAGE_INSTALL += "bluez5 pipewire-spa-plugins-bluez5"
ROOTFS_POSTPROCESS_COMMAND:append = " bluetooth_on_data"
bluetooth_on_data () {
	rm -rf ${IMAGE_ROOTFS}${localstatedir}/lib/bluetooth
	ln -sfn /data/OS/bluetooth ${IMAGE_ROOTFS}${localstatedir}/lib/bluetooth
}

# The build, for system updates (downloadd asks the server for a newer one; tools/publish-os
# reads it out of the slot image): UTC, YYYYMMDDHHMMSS, so newer sorts higher
ROOTFS_POSTPROCESS_COMMAND:append = " senkos_build"
senkos_build () {
	date -u +%Y%m%d%H%M%S > ${IMAGE_ROOTFS}${sysconfdir}/build
}

# The time zone is the settings' (`clock`, senkos-base); UTC until one is set
ROOTFS_POSTPROCESS_COMMAND:append = " senkos_localtime"
senkos_localtime () {
	rm -f ${IMAGE_ROOTFS}${sysconfdir}/timezone
	ln -sfn /data/OS/time/localtime ${IMAGE_ROOTFS}${sysconfdir}/localtime
}

# `smb` (senkos-base) starts avahi with the SMB server
ROOTFS_POSTPROCESS_COMMAND:append = " no_avahi_initscript"
no_avahi_initscript () {
	rm -f ${IMAGE_ROOTFS}${sysconfdir}/rc*.d/[SK]??avahi-daemon
}

# The launcher's Licenses screen reads /usr/share/senkos/licenses as is (apps/launcher/licenses.h):
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
ROOTFS_POSTPROCESS_COMMAND:append = " senkos_licenses"
# What LIC_FILES_CHKSUM names but isn't a license: libunistring's whole manual
SENKOS_LICENSE_SKIP = "libunistring/libunistring.texi"
# Generic texts that are a holder's notice rather than a template: left out like the templates
# (Yocto's ISC names the Internet Systems Consortium)
SENKOS_LICENSE_NOTICES = "ISC"
python senkos_licenses () {
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
    skip = set((d.getVar('SENKOS_LICENSE_SKIP') or '').split())
    notices = set((d.getVar('SENKOS_LICENSE_NOTICES') or '').split())
    archs = d.getVar('SSTATE_ARCHS').split()
    # A paragraph's first text, by its words: the one stored
    first = {}
    programs = []
    for pn in sorted(declared):
        licdir = next((p for p in (os.path.join(d.getVar('LICENSE_DIRECTORY'), a, pn) for a in reversed(archs))
                       if os.path.isdir(p)), None)
        if licdir is None:
            bb.fatal('senkos_licenses: no license files for %s' % pn)
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
        bb.fatal('senkos_licenses: more than u16 holds')
    out = d.expand('${IMAGE_ROOTFS}${datadir}/senkos/licenses')
    bb.utils.mkdirhier(os.path.dirname(out))
    with open(out, 'wb') as f:
        f.write(struct.pack('<%dH' % len(table), *table))
        f.write(''.join(s + '\0' for s in index).encode('utf-8'))
}

# Card layout (senkos.wks.in): wic is the whole card; squashfs-lz4 is one A/B slot (also what
# `tools/ota` flashes)
IMAGE_FSTYPES = "wic.gz wic.bmap squashfs-lz4"
IMAGE_TYPEDEP:wic += "squashfs-lz4"
# High compression costs build time only: lz4 decompresses as fast either way
EXTRA_IMAGECMD:squashfs-lz4 = "-Xhc"
WKS_FILE = "senkos.wks.in"
# Only what senkos.wks.in needs (the oe-core default also builds grub-efi, systemd-boot, cdrtools, btrfs/squashfs/erofs tools)
WKS_FILE_DEPENDS = "wic-native e2fsprogs-native dosfstools-native mtools-native bmaptool-native u-boot"
# Kernel + dtbs live in each slot's /boot; the FAT partition has the boot script and splash
IMAGE_BOOT_FILES = "boot.scr splash.lz4 splash-light.lz4"

# qemu (qemu.yml, tools/qemu): the same slots and /data on a virtio disk; qemu loads the kernel,
# no U-Boot. No device tree on x86; cores that are arm64-only (dynarecs, the aarch64 JRE) left out.
# senkos-qemu: the pad from the keyboard, the network for ssh.
IMAGE_INSTALL:remove:qemuall = "kernel-devicetree libretro-flycast libretro-mupen64plus-next libretro-gpsp libretro-freej2me"
IMAGE_INSTALL:append:qemuall = " senkos-qemu"
IMAGE_FSTYPES:qemuall = "wic squashfs-lz4"
WKS_FILE:qemuall = "senkos-qemu.wks.in"
WKS_FILE_DEPENDS:qemuall = "wic-native e2fsprogs-native dosfstools-native mtools-native"
IMAGE_BOOT_FILES:qemuall = ""
