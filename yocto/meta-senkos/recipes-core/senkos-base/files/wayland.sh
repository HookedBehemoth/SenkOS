# Wayland clients (ssh sessions included) talk to the Weston started by /etc/init.d/weston (as player)
export XDG_RUNTIME_DIR=/run/user/$(id -u player)
export WAYLAND_DISPLAY=wayland-0
# X11 clients: Weston's Xwayland (started on the first connection). The session type keeps Qt on
# Wayland when it has both.
export DISPLAY=:0 XDG_SESSION_TYPE=wayland
# AppImages (pkgforge's hooks) don't update themselves: their prompt has no dialog to show here,
# and the prompting shell's exit is reaped as the game's by games that wait() for any child (SRB2)
export DISABLE_AUTO_UPDATES=1
# The player's session bus (weston.init): MPRIS players for the launcher
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
# SDL2 passes over Wayland unless asked by name when the compositor has no decorations (Weston)
export SDL_VIDEODRIVER=wayland
# SDL2 keeps the screen on (an idle inhibitor) unless allowed: games and Rockbox let it dim and go
# off; video players inhibit themselves
export SDL_VIDEO_ALLOW_SCREENSAVER=1
# SDL2 talks to PipeWire only when asked by name; ALSA (pipewire-alsa) for the 32-bit one
export SDL_AUDIODRIVER=pipewire,alsa
# Built-in pad (anbernic-gamepad, the same buttons on every board) by label, not position: SDL's
# guess from the evdev codes makes the A button (BTN_EAST) SDL's B, and games that confirm with A
# cancel instead (STK quits). Joystick buttons are keycode order: BTN_SOUTH EAST NORTH WEST TL TR
# TL2 TR2 SELECT START MODE THUMBL THUMBR, DPAD_UP DOWN LEFT RIGHT; axes X Y RX RY where there are sticks
export SDL_GAMECONTROLLERCONFIG="19000000010000000100000000010000,Anbernic Gamepad,a:b1,b:b0,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,back:b8,start:b9,guide:b10,leftstick:b11,rightstick:b12,dpup:b13,dpdown:b14,dpleft:b15,dpright:b16,leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,"
# No ICU in the image: .NET programs (ports' runtimes and tools) run without locale data rather
# than abort
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
