# armhf binaries built elsewhere (Debian's: the ports) ask for the loader at the ABI's path; ours
# is in /lib32 (multilib, senkos.conf)
ARCH_DYNAMIC_LOADER:arm = "ld-linux-armhf.so.3"
# ...which is outside its libdir on purpose
INSANE_SKIP:${PN}:append:arm = " libdir"
