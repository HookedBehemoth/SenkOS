# oe-core's recipe is 2026.01; this is the same build of the latest release
require recipes-bsp/u-boot/u-boot-common.inc
require recipes-bsp/u-boot/u-boot.inc

DEPENDS += "bc-native dtc-native gnutls-native python3-pyelftools-native"

SRC_URI[sha256sum] = "e0253a2460f30943748e01967820ba0ebbaf1fa1c918a1bfce6a35554332bd0e"
