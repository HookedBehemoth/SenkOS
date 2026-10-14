# Mesa main: Vulkan 1.3 on Bifrost isn't in a release yet. oe-core's patches don't apply and don't concern us.
# 0001: 4-wide subgroups on G31. 0002: skip indexed indirect draws with no indices (device lost).
# 0003: -Dpanfrost-archs. 0004: index min/max search over the whole grid (PPSSPP 35 -> 60 fps).
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
MESA_SNAPSHOT = "7c648edb36af2c692d454a01c4dbf338756805b1"
PV = "26.3.0+git${@d.getVar('MESA_SNAPSHOT')[:8]}"
SRC_URI = "https://gitlab.freedesktop.org/mesa/mesa/-/archive/${MESA_SNAPSHOT}/mesa-${MESA_SNAPSHOT}.tar.gz \
           file://0001-pan-bi-panvk-panfrost-fix-subgroups-on-4-wide-Bifros.patch \
           file://0002-libpan-skip-indexed-indirect-draws-with-no-indices.patch \
           file://0003-panfrost-panvk-build-only-the-archs-in-Dpanfrost-arc.patch \
           file://0004-libpan-panvk-spread-the-index-min-max-search-over-th.patch \
"
SRC_URI[sha256sum] = "e22020b9f7fd7738b3acf6d572320ee1c5c2c07f7dbf5e0227337c10774d6aa2"
S = "${UNPACKDIR}/mesa-${MESA_SNAPSHOT}"
