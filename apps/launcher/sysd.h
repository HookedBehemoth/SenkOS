/*
 * Calls to sysd, root's side of the UI: org.senkos.System1 on the system bus
 * (apps/bus/org.senkos.System1.xml, system1.h generated from it): suspend, reboot, the clocks,
 * Wi-Fi, USB and the rest it lists. The launcher runs unprivileged.
 *
 * A call: org.senkos.System1's method (its system1_call_ without the prefix), its arguments:
 *
 *     SYSD("SSH off", set_ssh, false)
 *
 * false when sysd isn't on the bus (yet). Its reply wakes the UI: UI_DONE, or UI_REFUSED with
 * bus_refused saying "WHAT: WHY".
 */
#pragma once

#include <stdbool.h>

#include "bus.h"
#include "system1.h"

/* At start, before the bus is used */
void sysd_init(void);
/* The bus, NULL while sysd isn't on it */
sd_bus *sysd_bus(void);

#define SYSD(what, method, ...)                                                           \
  ({                                                                                      \
    static const system1_##method##_then sysd_then_ = {bus_done, (void *)(what)};         \
    sd_bus *const sysd_bus_ = sysd_bus();                                                 \
    sysd_bus_ != NULL && system1_call_##method(sysd_bus_, ##__VA_ARGS__, &sysd_then_) >= 0; \
  })

/* For menus' entries (entry.sysd) */
bool sysd_suspend(void);
bool sysd_reboot(void);
bool sysd_power_off(void);
bool sysd_usb_on(void);
bool sysd_usb_off(void);
bool sysd_usb_mtp(void);
bool sysd_usb_smb(void);
bool sysd_wifi_on(void);
bool sysd_wifi_off(void);
