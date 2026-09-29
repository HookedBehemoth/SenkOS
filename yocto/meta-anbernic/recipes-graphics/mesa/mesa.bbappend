# Mali-G31 subgroup fix on a Mesa main snapshot, see mesa-main.inc
require mesa-main.inc
# Mesa's Vulkan HUD (VK_INSTANCE_LAYERS=VK_LAYER_MESA_overlay): FPS/frame times over any Vulkan app
EXTRA_OEMESON += "-Dvulkan-layers=overlay"
FILES:mesa-vulkan-drivers += "${libdir}/libVkLayer_MESA_overlay.so"
