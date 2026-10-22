# Mesa's Vulkan HUD (VK_INSTANCE_LAYERS=VK_LAYER_MESA_overlay)
EXTRA_OEMESON += "-Dvulkan-layers=overlay"
FILES:mesa-vulkan-drivers += "${libdir}/libVkLayer_MESA_overlay.so"
# Asserts are in the GPU's precompiled helper kernels too, which every indexed JM draw runs (PPSSPP 45 -> 60 fps)
EXTRA_OEMESON += "-Db_ndebug=true"
# -Os + LTO: smaller, so Weston and the launcher start sooner; rendering is GPU-bound either way.
# Mesa throws nothing, so no exceptions or unwind tables.
MESA_LTO = "-flto=auto -ffat-lto-objects -fuse-linker-plugin"
FULL_OPTIMIZATION = "-Os ${DEBUG_LEVELFLAG} ${MESA_LTO} -fno-asynchronous-unwind-tables -fno-unwind-tables"
TARGET_CXXFLAGS:append = " -fno-exceptions"
# The link compiles too; the prefix map keeps build paths out of the debug info
TARGET_LDFLAGS:append = " ${FULL_OPTIMIZATION} ${DEBUG_PREFIX_MAP}"
# qemu (virtio in qemu.yml's PACKAGECONFIG): venus' protocol is a meson wrap from git; downloads are
# off, so the release where the wrap looks for it
SRC_URI:append:qemuall = " https://gitlab.freedesktop.org/virgl/venus-protocol/-/archive/v1.1.3/venus-protocol-v1.1.3.tar.gz;name=venus;subdir=mesa-${MESA_SNAPSHOT}/subprojects/venus-protocol-1.1.3;striplevel=1"
SRC_URI[venus.sha256sum] = "c5a362ca28e0a35042db1baeadaf9183500b216841e49d72714b80bae50b4445"
