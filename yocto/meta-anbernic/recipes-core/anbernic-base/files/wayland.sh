# Wayland clients (ssh sessions included) talk to the Weston started by /etc/init.d/weston
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
# SDL2 passes over Wayland unless asked by name when the compositor has no decorations (Weston)
export SDL_VIDEODRIVER=wayland
# Built-in pad (gpio-keys-gamepad) by label, not position: SDL's guess from the evdev codes makes
# the A button (BTN_EAST) SDL's B, and games that confirm with A cancel instead (STK quits).
# Joystick buttons are keycode order: BTN_SOUTH EAST NORTH WEST TL TR TL2 TR2 SELECT START MODE, DPAD_UP DOWN LEFT RIGHT
export SDL_GAMECONTROLLERCONFIG="19000000010000000100000000010000,gpio-keys-gamepad,a:b1,b:b0,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,back:b8,start:b9,guide:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,platform:Linux,"
