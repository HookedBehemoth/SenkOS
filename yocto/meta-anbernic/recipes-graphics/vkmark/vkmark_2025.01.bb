SUMMARY = "Vulkan benchmark (glmark2's scenes), Wayland and KMS window systems"
LICENSE = "LGPL-2.1-or-later"
LIC_FILES_CHKSUM = "file://COPYING-LGPL2.1;md5=4fbd65380cdd255951079008b364516c"
# glm is header-only: unpacked into the source (inside the debug prefix map: its asserts keep __FILE__) instead of a recipe
SRC_URI = "https://github.com/vkmark/vkmark/archive/refs/tags/${PV}.tar.gz;downloadfilename=${BP}.tar.gz \
           https://github.com/g-truc/glm/archive/refs/tags/1.0.3.tar.gz;downloadfilename=glm-1.0.3.tar.gz;name=glm;subdir=${BP}"
SRC_URI[sha256sum] = "1ae362844344d0f9878b7a3f13005f77eae705108892a4e8abf237d452d37edc"
SRC_URI[glm.sha256sum] = "6775e47231a446fd086d660ecc18bcd076531cfedd912fbd66e576b118607001"
# Tag tarball over a git clone; the sha256 pins it
ERROR_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/${BP}"

inherit meson pkgconfig python3native

DEPENDS = "vulkan-loader vulkan-headers assimp wayland wayland-native wayland-protocols libdrm virtual/libgbm"
CXXFLAGS += "-I${S}/glm-1.0.3"
EXTRA_OEMESON = "-Dxcb=false -Dwayland=true -Dkms=true"

# vk.xml is looked up under vulkan.pc's prefix (/usr): the build host's, not the sysroot's
do_configure:prepend() {
	sed -i "s|vulkan_dep.get_pkgconfig_variable('prefix')|'${STAGING_DIR_HOST}${prefix}'|" ${S}/src/meson.build
}
