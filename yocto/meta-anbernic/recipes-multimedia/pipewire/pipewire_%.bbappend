# The card, WirePlumber, ALSA's default (pipewire-alsa) and the Pulse socket (anbernic.conf): none
# of the recipe's desktop extras (camera, Bluetooth, JACK, GStreamer, Avahi, ...)
PACKAGECONFIG:class-target = "alsa udev wireplumber pipewire-alsa"
# 32-bit: only ALSA's plugin, for the armhf ports' ALSA default (the daemon is the 64-bit one).
# alsa too: without it spa/meson.build turns alsa_dep off, and the plugin with it
PACKAGECONFIG:virtclass-multilib-lib32 = "alsa pipewire-alsa"
