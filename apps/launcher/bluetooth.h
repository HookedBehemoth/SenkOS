/*
 * Bluetooth through BlueZ (org.bluez on the system bus): a mirror of its adapter and devices, kept
 * by its signals, and what the settings do with them. While pairing (the pairing screen is up) the
 * adapter searches and is pairable, and the agent (bt_agent.c) says yes; else it refuses.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "conn.h"
#include "device1.h"
#include "str.h"

enum { BT_MAX = 32 };
enum { BT_OTHER, BT_AUDIO, BT_PAD };
/* A slot of its own while it's there: its pending call is in it */
typedef struct {
  bool used;
  unsigned seq; /* the order BlueZ told of them */
  STRBUF(64) path;
  device1_props p;
  /* Gone while pairing: kept until it ends, so the rows stay where they are */
  bool gone;
  /* Pairing, connecting or disconnecting */
  bus_call call;
} bt_device;

/* At start, before the bus is used */
void bt_init(void);
/* Once true after the mirror changed (watch.c: WATCH_BLUETOOTH) */
bool bt_changed(void);
/* The devices in the order BlueZ told of them */
size_t bt_list(const bt_device **out, size_t max);
int bt_kind(const bt_device *d);
/* -1 without an adapter, else whether it's on */
int bt_powered(void);
bool bt_searching(void);
/* For the status bar: -1 off (or none), 0 on, 1 something connected */
int bt_bar(void);

void bt_power(bool on);
/* The pairing screen's: searching, pairable, the agent saying yes while it's up */
void bt_pairing_start(void);
void bt_pairing_stop(void);
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
void bt_agent_add(bus_conn *bus);
void bt_agent_register(bus_conn *bus);
