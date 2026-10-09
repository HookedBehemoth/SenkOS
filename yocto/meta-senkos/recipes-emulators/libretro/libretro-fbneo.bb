LICENSE = "FBNeo"
LIC_FILES_CHKSUM = "file://src/license.txt;md5=f61de60ada8817f1957a71b82dd3091f"
LIBRETRO_REPO = "FBNeo"
LIBRETRO_REV = "a4012b161e48b33b94940f07987323574c237448"
SRC_URI[sha256sum] = "07beabde5b7697b0f138874f320a595ef30b2bc9ab152aeaa9e8d7a4e7b8680d"
require libretro-core.inc
SUMMARY = "Arcade / Neo Geo (FinalBurn Neo) libretro core"
# Non-commercial license
NO_GENERIC_LICENSE[FBNeo] = "src/license.txt"
LIBRETRO_DIR = "${S}/src/burner/libretro"
