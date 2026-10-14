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
