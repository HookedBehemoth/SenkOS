# Mesa main snapshot instead of oe-core's 26.0.5: Vulkan 1.3 on Bifrost (Mali-G31) landed after 26.2.
# 0001 fixes subgroups on 4-wide Bifrost (G31 faulted with INSTR_INVALID_ENC); 0002 skips indexed indirect draws with
# indexCount 0 (the GPU faulted and the device was lost; vkQuake). oe-core's patches (mips, freedreno, armhf LLVM)
# don't apply to main and none of them concern what we build.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
MESA_SNAPSHOT = "ea3bfc00a4913146697318816fde2dc8bd23820e"
PV = "26.3.0+git${@d.getVar('MESA_SNAPSHOT')[:8]}"
SRC_URI = "https://gitlab.freedesktop.org/mesa/mesa/-/archive/${MESA_SNAPSHOT}/mesa-${MESA_SNAPSHOT}.tar.gz \
           file://0001-pan-bi-panvk-panfrost-fix-subgroups-on-4-wide-Bifros.patch \
           file://0002-libpan-skip-indexed-indirect-draws-with-no-indices.patch \
"
SRC_URI[sha256sum] = "4363794178230dacb43872d3f904d7ae8d6a8a3b538d73128958f6b440cf76ca"
S = "${UNPACKDIR}/mesa-${MESA_SNAPSHOT}"
