/*
 * Wi-Fi settings: on or off (sysd runs `wifi on|off`, which says which in
 * /run/wifi), and the networks, through wpa_supplicant's control socket
 * (/run/wpa_supplicant/wlan0, group player): what it found, what it knows,
 * what it's on. It saves the networks itself, where only root reads them.
 * Requests and their answers here (watch.c has the events): wifi_poll asks
 * again while the screen is up.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "str.h"

enum { WIFI_MAX = 32 };
typedef struct {
  STRBUF(160) ssid; /* as wpa_supplicant prints it: \xNN for odd bytes */
  int bars;         /* 1-4 */
  bool secured, saved, current;
} wifi_network;

extern wifi_network wifi_networks[WIFI_MAX];
extern size_t wifi_count;

/* /run/wifi says on */
bool wifi_on(void);
/* For the status bar: -1 off, 0 on no network, else its signal in bars, 1-4 */
int wifi_signal(void);
/* "Connected to NAME · 192.168.1.20", "Connecting to NAME…", "Not connected" */
str wifi_status(void);
/* The state and the networks again; a scan now and then. Cheap enough for once a second. */
void wifi_poll(void);
/* A saved network again (psk ""), or a new one, saved */
bool wifi_connect(str ssid, bool secured, str psk);
bool wifi_forget(str ssid);
