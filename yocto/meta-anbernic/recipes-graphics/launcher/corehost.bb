SUMMARY = "Runs one libretro core with one game and no UI of its own: the launcher drives it (launcher/appctl.h)"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"

# Built from the repo's corehost/ like the launcher, with the launcher's appctl.h beside it
FILESEXTRAPATHS:prepend := "${@'/repo' if os.path.isdir('/repo/corehost') else os.path.abspath(d.getVar('THISDIR') + '/../../../..')}:"
SRC_URI = "file://corehost/ file://launcher/appctl.h"
S = "${UNPACKDIR}/corehost"

inherit meson pkgconfig deploy

# virtual/libgles2: headers only, the functions come from SDL_GL_GetProcAddress
DEPENDS = "libsdl2 zlib virtual/libgles2 vulkan-loader vulkan-headers"

do_deploy() {
    install -m 0755 ${B}/corehost ${DEPLOYDIR}/corehost
}
addtask deploy after do_compile before do_build
