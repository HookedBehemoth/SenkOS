SUMMARY = "OpenAL Soft: 3D audio API (games), ALSA backend only"
LICENSE = "LGPL-2.0-or-later & BSD-3-Clause"
LIC_FILES_CHKSUM = "file://COPYING;md5=0f159f19f9377e1895fbb477d5a7953e \
                    file://LICENSE-pffft;md5=c104e5027ef1e3ebb8f4c0a464b05c4e"
SRC_URI = "https://github.com/kcat/openal-soft/archive/refs/tags/${PV}.tar.gz;downloadfilename=openal-soft-${PV}.tar.gz"
SRC_URI[sha256sum] = "fb27e5839aa11f0e5b9d33756965291fad5d6909ab928ea1f796f4a1a6877894"
# Only a Windows binary among the release assets; the tag archive is pinned by the checksum above
ERROR_QA:remove = "src-uri-bad"
WARN_QA:remove = "src-uri-bad"
S = "${UNPACKDIR}/openal-soft-${PV}"

inherit cmake pkgconfig
DEPENDS = "alsa-lib"

# ALSA is dlopen'd (libasound is in the image). HRTF data is embedded.
EXTRA_OECMAKE = "-DALSOFT_REQUIRE_ALSA=ON -DALSOFT_BACKEND_OSS=OFF -DALSOFT_BACKEND_PIPEWIRE=OFF \
    -DALSOFT_BACKEND_PULSEAUDIO=OFF -DALSOFT_BACKEND_JACK=OFF -DALSOFT_BACKEND_PORTAUDIO=OFF \
    -DALSOFT_BACKEND_SNDIO=OFF -DALSOFT_BACKEND_SDL2=OFF -DALSOFT_BACKEND_SDL3=OFF -DALSOFT_BACKEND_WAVE=OFF \
    -DALSOFT_RTKIT=OFF -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_INSTALL_CONFIG=OFF \
    -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF"
RDEPENDS:${PN} = "libasound"
