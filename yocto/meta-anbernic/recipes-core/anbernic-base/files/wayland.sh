# Wayland clients (ssh sessions included) talk to the Weston started by /etc/init.d/weston
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
# SDL2 passes over Wayland unless asked by name when the compositor has no decorations (Weston)
export SDL_VIDEODRIVER=wayland
