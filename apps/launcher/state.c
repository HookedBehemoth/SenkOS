/* Shared launcher state and display labels. */

#include "launcher_internal.h"

launcher_state launcher = {.capture = {.option = -1}};
str button_names[APPCTL_BUTTONS];
str category_names[DEBUG_PAGE + 1];
