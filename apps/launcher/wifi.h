/*
 * Wi-Fi: on or off is `wifi`'s (sysd runs it; it says which in /run/wifi), the rest wpa_supplicant's
 * (fi.w1.wpa_supplicant1 on the system bus): a mirror of its interface and the networks it saved,
 * kept by its signals, and what the settings do with them. It saves the networks itself, where only
 * root reads them. Opening the add screen searches; what it finds shows in the order it was found.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "str.h"

enum { WIFI_MAX = 32 };

/* A saved network */
typedef struct {
  STRBUF(64) path;
  STRBUF(128) name;
  bool current;   /* the one it's on, or getting on */
  bool connected; /* and it's on it */
  bool busy;      /* being selected */
} wifi_saved;

/* A network found (its access points as one) */
typedef struct {
  STRBUF(128) name;
  uint8_t ssid[32];
  uint8_t ssid_len;
  int bars; /* 1-4, the best of them */
  bool secured;
  bool gone; /* not heard any more: kept until adding ends, so the rows stay */
  bool busy; /* being added */
} wifi_found;

/* At start, before the bus is used */
void wifi_init(void);
/* Once true after the mirror changed (watch.c: WATCH_WIFI) */
bool wifi_changed(void);
/* /run/wifi says on */
bool wifi_on(void);
/* For the status bar: -1 off, 0 on no network, else its signal in bars, 1-4 */
int wifi_signal(void);
/* The signal asked for again (it's no event): each minute */
void wifi_poll_signal(void);
/* "Connected to NAME · 192.168.1.20", "Connecting to NAME…", "Not connected" */
str wifi_status(char *buf, size_t size);

/* In the order it saved them */
size_t wifi_saved_list(wifi_saved *out, size_t max);
void wifi_select(str path);
/* Unsave; back on whichever saved one is there */
void wifi_forget(str path);

/* The add screen's: a search when it comes up */
void wifi_adding_start(void);
void wifi_adding_stop(void);
/* What's found and not saved yet */
size_t wifi_found_list(wifi_found *out, size_t max);
/* Saved and on it; psk "" for an open one */
void wifi_add(const wifi_found *net, str psk);
/* Once true after a network was added */
bool wifi_added_new(void);
