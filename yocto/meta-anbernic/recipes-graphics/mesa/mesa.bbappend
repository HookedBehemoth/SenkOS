# Mali-G31 subgroup fix on a Mesa main snapshot, see mesa-main.inc
require mesa-main.inc
# Mesa's Vulkan HUD (VK_INSTANCE_LAYERS=VK_LAYER_MESA_overlay): FPS/frame times over any Vulkan app
EXTRA_OEMESON += "-Dvulkan-layers=overlay"
FILES:mesa-vulkan-drivers += "${libdir}/libVkLayer_MESA_overlay.so"
# Mali-G31 is v7 (Bifrost): only its copy of the per-arch code and shaders. All ten made libgallium
# 21.6 MB, and loading it from squashfs was most of Weston's GL startup.
EXTRA_OEMESON += "-Dpanfrost-archs=7"
