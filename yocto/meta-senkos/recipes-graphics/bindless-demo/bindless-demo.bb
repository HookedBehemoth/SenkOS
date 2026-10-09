SUMMARY = "Vulkan 1.3 demo: 1000 rects, one instanced strip draw, textures from a descriptor-indexed sampler array"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"

# Built from the repo's demos/bindless/, like the launcher. Not in the image: deployed to copy to /data/bindless
FILESEXTRAPATHS:prepend := "${@'/repo' if os.path.isdir('/repo/demos') else os.path.abspath(d.getVar('THISDIR') + '/../../../..')}:"
SRC_URI = "file://demos/bindless/"
S = "${UNPACKDIR}/demos/bindless"

inherit meson pkgconfig deploy

DEPENDS = "libsdl2 vulkan-loader vulkan-headers glslang-native"

do_deploy() {
	install -d ${DEPLOYDIR}/bindless
	${STRIP} -o ${DEPLOYDIR}/bindless/bindless-vk ${B}/bindless-vk
}
addtask deploy after do_compile before do_build
