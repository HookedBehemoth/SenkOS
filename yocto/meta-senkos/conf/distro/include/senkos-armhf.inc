# armhf games: a 32-bit userland next to ours, in /lib32 and /usr/lib32 (lib32-*
# packages); ours stays in /lib and /usr/lib. glibc's bbappend puts the armhf loader where Debian's
# binaries look for it.
require conf/multilib.conf
MULTILIBS = "multilib:lib32"
# A meta-oe recipe that doesn't parse with multilib, and that we don't build
BBMASK += "meta-oe/recipes-connectivity/conserver/"
# multilib derives lib32-u-boot from virtual/bootloader's provider, which then rules out u-boot itself
PREFERRED_PROVIDER_virtual/lib32-bootloader = "u-boot"
DEFAULTTUNE:virtclass-multilib-lib32 = "armv7athf-neon"
BASE_LIB:tune-aarch64 = "lib"
BASE_LIB:tune-armv7athf-neon = "lib32"
