# Shutdown: stop-all (senkos-base) instead of rcK. Every process gets SIGTERM and time to exit
# while /data is still writable, then init unmounts. No swap to turn off.
# Daemons init starts again should they die, once rcS is done: sysd (the lid, the power key, the
# launcher's requests), the USB network's DHCP server (its interface is senkos.init's) and
# downloadd (ports and system updates for the store and the launcher).
do_install:append() {
	sed -i -e 's#^::shutdown:/etc/init.d/rcK$#::shutdown:${sbindir}/stop-all#' \
		-e '/^::shutdown:.*swapoff/d' ${D}${sysconfdir}/inittab
	grep -q '^::shutdown:${sbindir}/stop-all$' ${D}${sysconfdir}/inittab
	printf '::respawn:%s\n' '${sbindir}/sysd' '${sbindir}/udhcpd -f -S /etc/udhcpd.conf' \
		'${bindir}/downloadd </dev/null >/var/log/downloadd.log 2>&1' >> ${D}${sysconfdir}/inittab
}
