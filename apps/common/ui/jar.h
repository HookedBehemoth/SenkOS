/*
 * A J2ME jar's name and icon, as its manifest gives them (MIDlet-Name,
 * MIDlet-Icon, else the first MIDlet's: "MIDlet-1: Name, /icon.png, Class").
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* NUL-terminated into `out`; false without one, or when it isn't UTF-8 */
bool jar_name(const char *path, char *out, size_t size);
/* The icon file's bytes on the heap (free them); NULL without one */
uint8_t *jar_icon(const char *path, size_t *size);
