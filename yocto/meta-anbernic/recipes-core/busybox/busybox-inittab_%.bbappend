# Shutdown: stop-all (anbernic-base) instead of rcK. Every process gets SIGTERM and time to exit
# while /data is still writable, then init unmounts. No swap to turn off.
do_install:append() {
	sed -i -e 's#^::shutdown:/etc/init.d/rcK$#::shutdown:${sbindir}/stop-all#' \
		-e '/^::shutdown:.*swapoff/d' ${D}${sysconfdir}/inittab
	grep -q '^::shutdown:${sbindir}/stop-all$' ${D}${sysconfdir}/inittab
}
