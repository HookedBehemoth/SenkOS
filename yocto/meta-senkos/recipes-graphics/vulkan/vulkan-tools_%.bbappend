# vulkaninfo + vkcube as a Wayland client
EXTRA_OECMAKE:remove = "-DBUILD_CUBE=OFF"
EXTRA_OECMAKE += "-DBUILD_CUBE=ON -DCUBE_WSI_SELECTION=WAYLAND"
DEPENDS += "wayland-native wayland-protocols"
