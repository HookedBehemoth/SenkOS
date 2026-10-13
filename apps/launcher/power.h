/*
 * The pause menu's Power page: the CPU's and the GPU's clocks for each app,
 * and the backlight. No power draw: the AXP717's battery current is a raw ADC
 * value of unknown offset (mainline's driver says so), ~405 mA idle or busy.
 *
 * An app's clocks come from the kernel's tables, set whenever it comes on
 * screen: the CPU's a cap (Auto: the highest), the GPU's pinned (Auto: its
 * governor's choice). At home everything goes back to Auto. sysd writes them, the launcher runs unprivileged.
 * The display's refresh rate too, one of its modes' (Auto: the one Weston
 * started with), switched by Weston's frames.so. Kept a line an app in
 * $XDG_CONFIG_HOME/launcher-power: "CPU_KHZ GPU_HZ FPS REFRESH_MHZ KEY", 0 for Auto.
 *
 * Calibration finds the lowest clocks that still hold the app's frame rate
 * (FPS, 30 to the refresh rate): from the highest, the CPU's a step at a time while every
 * second of five holds it, then the GPU's the same way, and keeps the last
 * that held. Frames are what the display shows (Weston's frames.so
 * module), whoever draws them. The result is set once, like a manual choice.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "str.h"

/* The page's rows */
enum { POWER_CALIBRATE, POWER_CPU, POWER_GPU, POWER_REFRESH, POWER_BRIGHTNESS, POWER_ROWS };

/* The tables, and the saved brightness set */
void power_init(void);
/* The saved brightness and the clocks in use, again: at start sysd may not be there yet */
void power_apply(void);
/* `key`'s clocks (an app's: its ROM, folder or command) set; "": home, Auto */
void power_use(str key);
/* A row's value as shown: "60 fps", "Auto", "1008 MHz", "120 Hz", "50%" */
str power_value(int row);
/* Left or right on a row: set and saved */
void power_step(int row, int by);
/* The backlight's percent, -1 without one */
int power_brightness(void);

/* The frames shown so far (Weston's frames.so), false without the counter */
bool power_frames(uint32_t *out);
void power_calibrate(void);
bool power_calibrating(void);
/* "Testing CPU 1104 · GPU 648 MHz" */
str power_progress(void);
/* Calibration given up (the app was paused or is gone): its saved clocks again */
void power_cancel(void);
/* Calibration's next step if due: ms until it wants this again, -1 when not
 * calibrating. `*say` is set to what to show, if anything (else left as is):
 * the clocks tried next, or at the end what was set. */
int power_tick(uint64_t now, str *say);
