# Joystick discovery through udev: SDL's own evdev guess wants ABS_X/ABS_Y, and the built-in
# gpio-keys gamepad has buttons only (udev tags it ID_INPUT_JOYSTICK). libudev is dlopen'd.
DEPENDS:append:class-target = " udev"
RDEPENDS:${PN}:append:class-target = " libudev"
