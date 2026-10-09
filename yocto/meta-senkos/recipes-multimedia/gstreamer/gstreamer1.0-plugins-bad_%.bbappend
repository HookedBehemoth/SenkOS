FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Seeking an open-GOP HEVC stream (a CRA key unit) hung Cedrus
SRC_URI += "file://0001-h265decoder-start-a-new-bitstream-on-flush.patch"
