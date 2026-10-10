/*
 * The keys on the side, read whatever has the focus, each by a thread of its own.
 *
 * Volume: the default sink's volume through PipeWire's Pulse server
 * (WirePlumber keeps it and restores it at boot), in 16 steps of PA's cubic
 * scale. One connection, subscribed, so the OSD (UI_VOLUME) also shows what
 * others change. With the pad's Menu held they step the backlight instead
 * (UI_BRIGHTNESS).
 *
 * Power: held POWER_HOLD_MS (sysd's HOLD_S: let go sooner, sysd suspends),
 * UI_POWER_HOLD.
 */
#pragma once

/* keys/key: the evdev fd (gpio-keys-volume, axp20x-pek), as (intptr_t) */
void *volume_thread(void *keys);
void *power_key_thread(void *key);
