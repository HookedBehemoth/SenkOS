/*
 * Requests to sysd, root's side of the UI (/run/sysd.sock): suspend, reboot, the clocks, the
 * backlight, Wi-Fi, USB and the rest it lists. The launcher runs unprivileged.
 */
#pragma once

#include <stdbool.h>

#include "str.h"

/* Sent; false when sysd isn't there (yet). Its reply comes on a thread: one that isn't "ok"
 * wakes the UI with UI_REFUSED. */
bool sysd(str request);
/* What the last refusal said: "REQUEST: WHY" */
str sysd_refused(char *buf, size_t size);
