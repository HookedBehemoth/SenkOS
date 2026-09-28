FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# cairo/pango/gdk-pixbuf/glib are only for Weston's clients, desktop shell and nested backends,
# all disabled (PACKAGECONFIG in anbernic.conf). pixman stays: libweston's core needs it.
SRC_URI += "file://0001-shared-make-cairo-optional-for-cairo-shared.patch"
DEPENDS:remove = "cairo pango gdk-pixbuf glib-2.0"
# No cairo users: wcap decoder off; of the tools only weston-debug (protocol/scene dumps)
EXTRA_OEMESON += "-Dwcap-decode=false -Dtools=debug -Dbackend-default=drm"
