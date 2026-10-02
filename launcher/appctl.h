/*
 * The launcher's control channel into an app that takes it (corehost): no
 * serialization, both ends include this header.
 *
 * The launcher makes a SOCK_SEQPACKET socketpair and a memfd before the fork
 * and passes their numbers in APPCTL_ENV ("<socket>,<memfd>"). Requests go
 * over the socket as appctl_msg, and each gets one appctl_msg back with the
 * same op and seq. State lives in the memfd as one appctl_shm, read and
 * written in place: the app fills in its options and publishes
 * option_count; the launcher sets an option's value and sends APPCTL_OPTIONS.
 *
 * An app without the variable is paused with SIGSTOP instead.
 */
#pragma once

#include <stdatomic.h>
#include <stdint.h>

#define APPCTL_ENV "LAUNCHER_CTL"
#define APPCTL_MAGIC 0x4c544341u /* "ACTL" */
#define APPCTL_VERSION 3

enum {
  /* Stop running and wait; the app has stopped once it answers */
  APPCTL_PAUSE = 1,
  /* arg 1: it was out of sight (Home), map the window again to come on top */
  APPCTL_RESUME,
  /* arg: slot */
  APPCTL_SAVE_STATE,
  APPCTL_LOAD_STATE,
  APPCTL_RESET,
  /* Values in the shared options changed */
  APPCTL_OPTIONS,
};

typedef struct {
  uint32_t op;
  uint32_t seq;
  int32_t arg;
  int32_t result; /* in the answer: 0, or a negative errno */
} appctl_msg;

/* The pad's buttons, a bit each in a binding. A combo is written in this
 * order, names joined by " + " ("R2 + Start + A"). */
enum {
  APPCTL_MENU, APPCTL_L1, APPCTL_L2, APPCTL_R1, APPCTL_R2,
  APPCTL_UP, APPCTL_DOWN, APPCTL_LEFT, APPCTL_RIGHT,
  APPCTL_START, APPCTL_SELECT,
  APPCTL_A, APPCTL_B, APPCTL_X, APPCTL_Y,
  APPCTL_BUTTONS
};
static const char *const appctl_button_names[APPCTL_BUTTONS] = {
    "Menu", "L1", "L2", "R1", "R2", "Up", "Down", "Left", "Right",
    "Start", "Select", "A", "B", "X", "Y"};

/* Where an option is shown: the pause menu has an entry for each that has any */
enum { APPCTL_SYSTEM, APPCTL_GRAPHICS, APPCTL_AUDIO, APPCTL_INPUT, APPCTL_CATEGORIES };
static const char *const appctl_category_names[APPCTL_CATEGORIES] = {
    "System", "Graphics", "Audio", "Input"};

/* An option is a choice among `values`, or a binding: a combo of buttons,
 * which the launcher records when the player presses it */
enum { APPCTL_CHOICE, APPCTL_BINDING };

#define APPCTL_TEXT 48
#define APPCTL_VALUES 64
#define APPCTL_OPTIONS_MAX 64

typedef struct {
  char key[APPCTL_TEXT];
  char label[APPCTL_TEXT];
  char values[APPCTL_VALUES][APPCTL_TEXT];
  uint8_t count;
  _Atomic uint8_t value; /* index into values; the launcher writes it */
  uint8_t kind;
  uint8_t category;
  _Atomic uint16_t buttons; /* a binding's: 1 << APPCTL_A ...; 0: none */
} appctl_option;

typedef struct {
  uint32_t magic, version;
  /* Save state slots the app has, 0 without save states */
  uint32_t slots;
  /* Set after the options are filled in */
  _Atomic uint32_t option_count;
  appctl_option options[APPCTL_OPTIONS_MAX];
} appctl_shm;
