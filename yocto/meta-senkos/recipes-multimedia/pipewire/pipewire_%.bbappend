# The card, Bluetooth audio (SBC), WirePlumber, ALSA's default (pipewire-alsa) and the Pulse
# socket (senkos.conf): none of the recipe's other desktop extras (camera, JACK, GStreamer, Avahi, ...)
PACKAGECONFIG:class-target = "alsa bluez udev wireplumber pipewire-alsa"
# 32-bit: only ALSA's plugin, for the armhf games' ALSA default (the daemon is the 64-bit one).
# alsa too: without it spa/meson.build turns alsa_dep off, and the plugin with it
PACKAGECONFIG:virtclass-multilib-lib32 = "alsa pipewire-alsa"
