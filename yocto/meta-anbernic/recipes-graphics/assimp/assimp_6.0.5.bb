SUMMARY = "Open Asset Import Library: 3D model loader (for vkmark)"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=f2d38c9e0d63058b051065fb7b1102a8"
SRC_URI = "https://github.com/assimp/assimp/archive/refs/tags/v${PV}.tar.gz;downloadfilename=${BP}.tar.gz"
SRC_URI[sha256sum] = "edf3749559c2b7d1f758ffb66fc5bec62186221e623b7f2e8969f17ee46ecb6f"
# Tag tarball over a git clone; the sha256 pins it
ERROR_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/${BP}"

inherit cmake

DEPENDS = "zlib"
# Only the formats vkmark's models use (.3ds, .ply): a fraction of the full library
EXTRA_OECMAKE = "-DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF -DASSIMP_BUILD_3DS_IMPORTER=ON -DASSIMP_BUILD_PLY_IMPORTER=ON \
    -DASSIMP_NO_EXPORT=ON -DASSIMP_BUILD_ASSIMP_TOOLS=OFF -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_ZLIB=OFF \
    -DASSIMP_WARNINGS_AS_ERRORS=OFF -DASSIMP_BUILD_USE_CCACHE=OFF -DASSIMP_IGNORE_GIT_HASH=ON"

# find_package(ZLIB) bakes the build sysroot's libz.so path into the exported link flags
do_install:append() {
	sed -i 's|^Libs.private:.*|Libs.private: -lstdc++ -lz|' ${D}${libdir}/pkgconfig/assimp.pc
	sed -i 's|${STAGING_LIBDIR}/libz.so|z|g' ${D}${libdir}/cmake/assimp-*/assimpTargets.cmake
}
