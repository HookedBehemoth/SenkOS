/* downloadd's halves (main.c says what it is): the daemon, and one job run by it */
#pragma once

#include <stdbool.h>

#include "str.h"

/* A pack's name is a file's: "Nintendo - Game Boy" is OS/assets/thumbnails/Nintendo - Game Boy.pack */
bool safe_pack(str s);

/* `downloadd pack NAME | check | os | zone`, as the daemon runs it (or by hand) */
int job(int argc, char **argv);
int daemon_main(void);
