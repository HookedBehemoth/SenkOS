LICENSE = "Genesis-Plus-GX"
LIC_FILES_CHKSUM = "file://LICENSE.txt;md5=d4817c708af18ab478a033d92db6b7a9"
LIBRETRO_REPO = "Genesis-Plus-GX"
LIBRETRO_REV = "c2838c7dc4236fc2fe94e5dbd08b41486067918e"
SRC_URI[sha256sum] = "7ba2eab9d6dae71bb42e8208573300ad3475a4263e860178c4d19c36d85fc92b"
require libretro-core.inc
SUMMARY = "Mega Drive / Mega-CD / Master System / Game Gear libretro core"
# Non-commercial license
NO_GENERIC_LICENSE[Genesis-Plus-GX] = "LICENSE.txt"
LIBRETRO_MAKEFILE = "Makefile.libretro"
