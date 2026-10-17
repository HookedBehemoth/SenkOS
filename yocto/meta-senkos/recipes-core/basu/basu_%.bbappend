# The 0.2.1 release archive rather than meta-oe's git checkout
PV = "0.2.1"
SRC_URI = "https://git.sr.ht/~emersion/basu/refs/download/v${PV}/basu-${PV}.tar.gz"
SRC_URI[sha256sum] = "d9b373a9fcb5d5eb5f6c1c56355f76edb7f2f52bc744570e80604e83455a19bd"
S = "${UNPACKDIR}/basu-${PV}"
