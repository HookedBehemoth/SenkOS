# Environment overrides from /data/OS/profile.d/*.sh (e.g. VK_DRIVER_FILES, PANVK_DEBUG, MESA_*).
# Last in /etc/profile.d, so they win; weston.init sources it too (Weston, launcher and apps).
for f in /data/OS/profile.d/*.sh; do
	[ -r "$f" ] && . "$f"
done
unset f
