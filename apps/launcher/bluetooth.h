/*
 * Bluetooth through BlueZ (org.bluez on the system bus): a mirror of its adapter and devices, kept
 * by its signals, and what the settings do with them. While pairing (the pairing screen is up) the
 * adapter searches and is pairable, and the agent (bt_agent.c) says yes; else it refuses.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "device1.h"
#include "str.h"

enum { BT_MAX = 32 };
enum { BT_OTHER, BT_AUDIO, BT_PAD };
typedef struct {
  STRBUF(64) path;
  device1_props p;
  /* Gone while pairing: kept until it ends, so the rows stay where they are */
  bool gone;
} bt_device;

/* At start, before the bus is used */
void bt_init(void);
/* Once true after the mirror changed (watch.c: WATCH_BLUETOOTH) */
bool bt_changed(void);
/* The devices in the order BlueZ told of them */
const bt_device *bt_devices(size_t *count);
int bt_kind(const bt_device *d);
/* -1 without an adapter, else whether it's on */
int bt_powered(void);
bool bt_searching(void);
/* For the status bar: -1 off (or none), 0 on, 1 something connected */
int bt_bar(void);
/* The device an action is on: one at a time */
bool bt_busy(const char *path);

void bt_power(bool on);
/* The pairing screen's, as it comes and goes */
void bt_pairing(bool on);
bool bt_pairing_on(void);
/* Connect, or disconnect when connected */
void bt_connect(const char *path);
/* Pair, trust, connect */
void bt_pair(const char *path);
/* Once true after a pairing went through */
bool bt_paired_new(void);
/* Unpair */
void bt_forget(const char *path);

/* bt_agent.c: on each connection, and whenever bluetoothd comes */
void bt_agent_add(sd_bus *bus);
void bt_agent_register(sd_bus *bus);
