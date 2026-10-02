# Mali-G31 subgroup fix on a Mesa main snapshot, see mesa-main.inc
require mesa-main.inc
# Mesa's Vulkan HUD (VK_INSTANCE_LAYERS=VK_LAYER_MESA_overlay): FPS/frame times over any Vulkan app
EXTRA_OEMESON += "-Dvulkan-layers=overlay"
FILES:mesa-vulkan-drivers += "${libdir}/libVkLayer_MESA_overlay.so"
# Mali-G31 is v7 (Bifrost): only its copy of the per-arch code and shaders. All ten made libgallium
# 21.6 MB, and loading it from squashfs was most of Weston's GL startup.
EXTRA_OEMESON += "-Dpanfrost-archs=7"
# Release asserts off (buildtype is plain, so meson keeps them): they are in the GPU's precompiled
# helper kernels too, which every indexed draw on JM runs (PPSSPP 45 fps with them, 60 without)
EXTRA_OEMESON += "-Db_ndebug=true"
# -Os + LTO: smaller on disk (lz4), so Weston + the launcher start sooner. Rendering is GPU-bound either
# way (glmark2 unchanged); shader compiles without the cache take ~3% longer.
# No exceptions or unwind tables: Mesa throws none. Only cancelling the VK_KHR_display threads would
# unwind through Mesa, and that needs DRM master, which Weston holds.
MESA_LTO = "-flto=auto -ffat-lto-objects -fuse-linker-plugin"
FULL_OPTIMIZATION = "-Os ${DEBUG_LEVELFLAG} ${MESA_LTO} -fno-asynchronous-unwind-tables -fno-unwind-tables"
TARGET_CXXFLAGS:append = " -fno-exceptions"
# The link compiles too: same flags, and the prefix map keeps build paths out of the debug info
TARGET_LDFLAGS:append = " ${FULL_OPTIMIZATION} ${DEBUG_PREFIX_MAP}"
