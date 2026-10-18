SUMMARY = "Rockbox as an application: the music player, with MPRIS for the launcher's media controls"
DESCRIPTION = "Not in the image: rockbox.zip (rockbox.desktop, rockbox/) in the deploy directory, unpacked into the card's apps/applications"
HOMEPAGE = "https://www.rockbox.org"
# Rockbox GPL-2.0+; the D-Bus connection copied in from apps/ (ours)
LICENSE = "GPL-2.0-or-later & AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://docs/COPYING;md5=9ac2e7cff1ddaf48b6eab6028f23ef88"

# Rockbox has no recent releases: a commit's archive, pinned by its sha256
ROCKBOX_COMMIT = "183518be730fdf76d8a38383a965ba97d2596a3b"
PV = "4.0+git${@d.getVar('ROCKBOX_COMMIT')[:10]}"
# apps/bus and str from the repo (as apps.bb takes apps/)
FILESEXTRAPATHS:prepend := "${@'/repo' if os.path.isdir('/repo/apps') else os.path.abspath(d.getVar('THISDIR') + '/../../../..')}:"
SRC_URI = "https://github.com/Rockbox/rockbox/archive/${ROCKBOX_COMMIT}.tar.gz;downloadfilename=rockbox-${ROCKBOX_COMMIT}.tar.gz \
    file://0001-Retro-Handheld-SDL-application-target-for-Linux-game.patch \
    file://0002-Retro-Handheld-MPRIS-on-the-session-bus.patch \
    file://0003-configure-SDL_CONFIG-names-the-sdl2-config-to-use.patch \
    file://0004-build-the-dependency-list-through-a-file-and-xargs.patch \
    file://0005-hosted-settings-in-XDG_CONFIG_HOME-rockbox.org-when-.patch \
    file://0006-sdl-pcm-don-t-close-the-audio-device-holding-audio_l.patch \
    file://0007-hosted-configure-relocatable-finds-the-installation-.patch \
    file://0008-playlist-catalog-accept-a-new-playlist-s-name-under-.patch \
    file://0009-sdl-pcm-don-t-sleep-in-the-audio-callback.patch \
    file://config.cfg file://rockbox.desktop file://rockbox.png \
    file://apps/bus/ file://apps/common/str.c file://apps/common/str.h"
SRC_URI[sha256sum] = "265e60f4fa1ac3c07867236670bd3b9cbf2682af7b18d473b09f0d3ecbb8a424"
ERROR_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/rockbox-${ROCKBOX_COMMIT}"
B = "${WORKDIR}/build"

inherit pkgconfig deploy

# splash(HZ, ID2P(LANG_...)): Rockbox's strings are formats by design
SECURITY_STRINGFORMAT = ""

# perl: buildzip.pl installs; zip: the voice strings; python3: gen.py
DEPENDS = "libsdl2 perl-native zip-native python3-native"

# A fresh tree each time: make.dep is made once, by the first build
do_configure[cleandirs] = "${B}"
do_configure() {
    # Not a git tree: the version from here
    echo ${ROCKBOX_COMMIT} | cut -c1-10 > ${S}/docs/VERSION
    # MPRIS' D-Bus: our connection and the interfaces' code (apps/bus/gen.py)
    bus=${S}/apps/senkos-bus
    mkdir -p $bus
    cp ${UNPACKDIR}/apps/bus/conn.c ${UNPACKDIR}/apps/bus/conn.h ${UNPACKDIR}/apps/bus/wire.h \
        ${UNPACKDIR}/apps/common/str.c ${UNPACKDIR}/apps/common/str.h $bus/
    for i in org.mpris.MediaPlayer2.Player org.freedesktop.DBus.Properties; do
        python3 ${UNPACKDIR}/apps/bus/gen.py ${UNPACKDIR}/apps/bus/$i.xml $bus
    done
    rm -f $bus/*-client.c
    # configure asks sdl2-config, which Yocto turns off: pkg-config answers (0003)
    printf '#!/bin/sh\n[ "$1" = --version ] && exec pkg-config --modversion sdl2\nexec pkg-config "$1" sdl2\n' \
        > ${WORKDIR}/sdl2-config
    chmod +x ${WORKDIR}/sdl2-config
    # configure runs ${CROSS_COMPILE}gcc and friends as they are (its checks too: sigaltstack
    # for its threads): ours, with the sysroot. An application's objcopy only strips (the
    # codecs' and plugins' .elf into what's installed): a copy, Yocto strips into -dbg.
    cross=${WORKDIR}/cross
    mkdir -p $cross
    printf '#!/bin/sh\nexec ${CC} "$@"\n' > $cross/gcc
    printf '#!/bin/sh\nexec ${CPP} "$@"\n' > $cross/cpp
    printf '#!/bin/sh\nexec ${LD} "$@"\n' > $cross/ld
    printf '#!/bin/sh\nexec ${AS} "$@"\n' > $cross/as
    printf '#!/bin/sh\nexec ${AR} "$@"\n' > $cross/gcc-ar
    printf '#!/bin/sh\nexec ${RANLIB} "$@"\n' > $cross/gcc-ranlib
    printf '#!/bin/sh\n# -S -x IN OUT\n[ "$3" = "$4" ] || exec cp "$3" "$4"\n' > $cross/objcopy
    chmod +x $cross/*
    # 208: Retro Handheld (0001), as an application found next to its binary (0007)
    cd ${B}
    CROSS_COMPILE=$cross/ SDL_CONFIG=${WORKDIR}/sdl2-config ${S}/tools/configure --target=208 --type=N --relocatable
    grep -q '^#define HAVE_SIGALTSTACK_THREADS' autoconf.h || bbfatal "configure picked SDL threads"
    # Yocto's flags: buildpaths, GNU hash; the host's compiler for the tools
    sed -i -e 's|^export HOSTCC=.*|export HOSTCC=${BUILD_CC}|' \
        -e 's|^export GCCOPTS=\(.*\)|export GCCOPTS=\1 ${DEBUG_PREFIX_MAP}|' \
        -e 's|^export GLOBAL_LDOPTS=\(.*\)|export GLOBAL_LDOPTS=\1 ${LDFLAGS}|' Makefile
}

do_compile() {
    oe_runmake -C ${B}
}

# The folder the zip holds, next to its .desktop: rockbox/{bin,lib/rockbox,share/rockbox,icon.png}.
# ${B}/install keeps it unstripped (gdb's sysroot).
do_install() {
    rm -rf ${B}/install ${B}/zip
    oe_runmake -C ${B} install PREFIX=${B}/install/rockbox
    # Defaults for a user without a config.cfg of their own: the card's music, no idle quit
    install -m 0644 ${UNPACKDIR}/config.cfg ${B}/install/rockbox/share/rockbox/config.cfg
    install -d ${B}/zip
    cp -r ${B}/install/rockbox ${B}/zip/
    install -m 0644 ${UNPACKDIR}/rockbox.desktop ${B}/zip/
    install -m 0644 ${UNPACKDIR}/rockbox.png ${B}/zip/rockbox/icon.png
    find ${B}/zip/rockbox \( -name '*.codec' -o -name '*.rock' -o -name '*.ovl' \) -exec ${STRIP} --strip-unneeded {} +
    ${STRIP} ${B}/zip/rockbox/bin/rockbox
}

do_deploy() {
    cd ${B}/zip
    rm -f ${DEPLOYDIR}/rockbox-${PV}.zip
    zip -qr ${DEPLOYDIR}/rockbox-${PV}.zip rockbox.desktop rockbox
    ln -sf rockbox-${PV}.zip ${DEPLOYDIR}/rockbox.zip
}
addtask deploy after do_install before do_build

# Nothing for the image
PACKAGES = ""
