/*
 * The on-screen keyboard, for what the pad has to type (a Wi-Fi password):
 * letters, digits and symbols in a grid nav moves over, A types the focused
 * key. X deletes, Y shifts, Start is Done (the caller's buttons: it calls
 * keyboard_delete, keyboard_shift). Done reports the text.
 */
#pragma once

#include <stdbool.h>

#include "nav.h"
#include "str.h"

void keyboard_open(void);
str keyboard_text(void);
void keyboard_delete(void);
void keyboard_shift(void);
/* The field and the keys in `view`; true once Done was pressed */
bool keyboard(nav *ui, ui_rect view, str label);
