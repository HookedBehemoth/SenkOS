FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://0001-policy-Add-ReconnectOnPower.patch"

# The launcher powers the adapter on as the player left it
do_install:append() {
	sed -i 's/^#ReconnectOnPower = false$/ReconnectOnPower = true/; s/^#AutoEnable=true$/AutoEnable=false/' \
		${D}${sysconfdir}/bluetooth/main.conf
	grep -q '^ReconnectOnPower = true$' ${D}${sysconfdir}/bluetooth/main.conf
	grep -q '^AutoEnable=false$' ${D}${sysconfdir}/bluetooth/main.conf
}
