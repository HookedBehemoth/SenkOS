/* Home page persistence and navigation. */
#include <stdio.h>
#include <stdlib.h>

#include "cfg.h"
#include "launcher_internal.h"
#include "romdb.h"

#define PAGES (category_count + 1)

static str page_name(size_t page) {
  return page == 0 ? S("Running") : categories[page - 1].name;
}

void home_rescan(size_t page) {
  if (page >= 1 && page <= category_count)
    apps_scan(&categories[page - 1]);
}

size_t home_load_page(void) {
  str text = str_read(state_file("", "r")), rest = text;
  const str name = str_cut(&rest, '\n');
  size_t page = 1;
  for (size_t i = 1; page == 1 && i < PAGES; i++)
    if (str_eq(name, page_name(i)) && apps_present(&categories[i - 1]))
      page = i;
  free((char *)text.p);
  return page;
}

void home_save_page(size_t page) {
  FILE *f = state_file("", "w");
  if (f == NULL)
    return;
  str_fput(f, page_name(page), S("\n"));
  fclose(f);
}

void home_turn_page(nav *ui, size_t *page, int by) {
  do
    *page = (*page + PAGES + by) % PAGES;
  while ((*page == 0 && run_count() == 0) ||
         (*page >= 1 && !apps_present(&categories[*page - 1])));
  if (screen_current(launcher.visible) == &quick_menu_screen ||
      screen_current(launcher.visible) == &cores_screen)
    screen_pop(launcher.visible, ui);
  home_rescan(*page);
}
