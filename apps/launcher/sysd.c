#include "sysd.h"

/* org.senkos.System1 has an owner */
static bool there;

static void owned(bus_conn *bus, bool owner) {
  (void)bus;
  there = owner;
}

void sysd_init(void) { bus_watch(SYSTEM1_SERVICE, NULL, owned); }

bus_conn *sysd_bus(void) { return there ? bus_get() : NULL; }

bool sysd_suspend(void) { return SYSD("Sleep", suspend); }
bool sysd_reboot(void) { return SYSD("Reboot", reboot); }
bool sysd_power_off(void) { return SYSD("Power off", power_off); }
bool sysd_usb_on(void) { return SYSD("USB on", set_usb, "on"); }
bool sysd_usb_off(void) { return SYSD("USB off", set_usb, "off"); }
bool sysd_usb_mtp(void) { return SYSD("MTP", set_usb, "mtp"); }
bool sysd_usb_smb(void) { return SYSD("SMB", set_usb, "smb"); }
bool sysd_wifi_on(void) { return SYSD("Wi-Fi on", set_wifi, true); }
bool sysd_wifi_off(void) { return SYSD("Wi-Fi off", set_wifi, false); }
