/*
 * One file out of a zip, found through its central directory, so the order of
 * the entries doesn't matter. Stored or deflated, no zip64 or encryption.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "str.h"

/* The file `name` (case aside: jars get it wrong) on the heap (free it),
 * NUL-terminated past its `size` bytes; NULL without one */
uint8_t *zip_read(const char *path, str name, size_t *size);
