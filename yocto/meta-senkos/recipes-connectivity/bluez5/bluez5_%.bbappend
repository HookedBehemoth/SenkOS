FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://0001-policy-Add-ReconnectOnPower.patch"

do_install:append() {
	sed -i 's/^#ReconnectOnPower = false$/ReconnectOnPower = true/' ${D}${sysconfdir}/bluetooth/main.conf
	grep -q '^ReconnectOnPower = true$' ${D}${sysconfdir}/bluetooth/main.conf
}
