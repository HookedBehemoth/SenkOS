/*
 * The screen and sleep while nobody touches the handheld: dimmed after a while, off after
 * another, asleep after a third (each a setting, Never too). Any key brings the screen back;
 * the press also reaches the app in front. A surface shown with a zwp_idle_inhibitor_v1 (a video
 * playing: layer_inhibited) keeps all of it away.
 *
 * Music (an MPRIS player playing, media.h) keeps the handheld awake when "Music keeps it awake"
 * is on: no sleep while it plays, and the lid closed turns the screen off instead (sysd's
 * SetSleepHold). Once it stops, the lid closed sleeps, and idle sleep counts from then.
 *
 * Settings: config_file("-idle"), "DIM OFF SLEEP MUSIC" (seconds, 0: never; 1/0).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "str.h"

/* At start; the lid's thread */
void idle_init(void);
/* A key, from any thread: the timers start again, the screen comes back */
void idle_poke(void);
/* The UI thread's, for UI_ACTIVE and UI_LID (closed: 1) */
void idle_event(int why, int value);
/* Each pass of the UI loop: what's due done; ms till the next, -1: nothing */
int idle_tick(uint64_t now);
/* Nothing may draw: frames wouldn't come back */
bool idle_screen_off(void);

/* The settings page's rows */
enum { IDLE_DIM, IDLE_OFF, IDLE_SLEEP, IDLE_MUSIC, IDLE_ROWS };
str idle_value(int row);
bool idle_music(void);
/* Left/Right (by -1, 1), A: 1 */
void idle_step(int row, int by);
