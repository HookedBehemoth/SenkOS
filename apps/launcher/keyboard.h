/* Shared on-screen text entry for Wi-Fi passwords and title searches. */
#pragma once

#include <stdbool.h>

#include "nav.h"
#include "str.h"

void keyboard_open(str text);
str keyboard_text(void);
void keyboard_delete(void);
void keyboard_shift(void);
bool keyboard(nav *ui, ui_rect view, str label);
