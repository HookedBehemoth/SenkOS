/* Update status and actions. */
#include <stdlib.h>

#include "launcher_internal.h"
#include "sysd.h"
#include "updates.h"

static void choose_updates(nav *ui, void *choice);

static str megabytes(unsigned long long bytes) {
  static STRBUF(32) out;
  out.n = 0;
  sb_fmt(out, "%llu MB", (bytes + (1 << 19)) >> 20);
  return sb_str(out);
}

typedef STRBUF(64) update_label;
typedef STRBUF(700) update_text;
static size_t update_rows(entry *rows, str *said, update_label labels[2],
                          update_text text[1]) {
  const update_state u = updates;
  size_t n = 0;
  sb_set(text[0], S("This system: "), updates_build(), S(". "));
  if (!u.connected)
    sb_add(text[0], S("The download service isn't running."));
  else if (u.job == UPDATE_IDLE && str_eq(sb_str(u.job_state), S("failed")))
    sb_add(text[0], S("The update failed: "), sb_str(u.job_message));
  else if (u.check == UPDATE_CHECKING)
    sb_add(text[0], S("Checking\u2026"));
  else if (u.check == UPDATE_CURRENT)
    sb_add(text[0], S("It's up to date."));
  else if (u.check == UPDATE_AVAILABLE)
    sb_add(text[0], S("Build "), sb_str(u.build), S(" is out. "),
           sb_str(u.notes));
  else if (u.check == UPDATE_FAILED)
    sb_add(text[0], S("Couldn't check: "), updates_reason(sb_str(u.notes)));
  *said = sb_str(text[0]);
  if (u.job == UPDATE_READY) {
    rows[n++] = (entry){.label = S("Restart to finish the update"),
                        .sysd = sysd_reboot,
                        .action = POWER,
                        .icon = ICON_ROTATE_CW};
    return n;
  }
  if (u.job == UPDATE_RUNNING) {
    labels[0].n = 0;
    if (str_eq(sb_str(u.job_state), S("downloading")))
      sb_fmt(labels[0], "Downloading %d%%",
             u.total ? (int)(u.done * 100 / u.total) : 0);
    else if (str_eq(sb_str(u.job_state), S("installing")))
      sb_set(labels[0], S("Installing\u2026"));
    else
      sb_set(labels[0], S("Waiting\u2026"));
    rows[n++] = (entry){
        .label = sb_str(labels[0]), .action = NOTHING, .icon = ICON_HOURGLASS};
    return n;
  }
  if (u.check == UPDATE_AVAILABLE) {
    sb_set(labels[1], S("Download and install ("), megabytes(u.size), S(")"));
    rows[n++] = (entry){.label = sb_str(labels[1]),
                        .action = UPDATE_INSTALL,
                        .icon = ICON_DOWNLOAD};
  }
  rows[n++] = (entry){.label = S("Check for updates"),
                      .action = UPDATE_CHECK,
                      .icon = ICON_REFRESH_CW};
  return n;
}

static void updates_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  entry *rows = NAV_FRAME(ui, entry, 3);
  update_label *labels = NAV_FRAME(ui, update_label, 2);
  update_text *message = NAV_FRAME(ui, update_text, 1);

  str said;
  const size_t n = update_rows(rows, &said, labels, message);
  ui_lines text = {0};
  ui_wrap_text(&text, said, ROW_TEXT, state->draw.w - MARGIN * 4);
  const int line = ui_text_height(ROW_TEXT) + 4;
  const size_t shown_lines = MIN(text.count, 4);
  for (size_t i = 0; i < shown_lines; i++)
    ui_text(ROW_TEXT, MARGIN * 2, (int)content.y + MARGIN + (int)i * line,
            text.lines[i], DIM);
  free(text.lines);
  const float top = MARGIN + shown_lines * line;
  const ui_rect below = {content.x, content.y + top, content.w,
                         content.h - top};
  const entry *selected = menu(ui, rows, n, below);
  if (selected != NULL)
    nav_defer(ui, choose_updates, selected, sizeof(*selected));
}

static void choose_updates(nav *ui, void *choice) {
  (void)ui;
  const entry *chosen = choice;
  switch (chosen->action) {
  case POWER:
    chosen->sysd();
    break;
  case UPDATE_CHECK:
    updates_check();
    break;
  case UPDATE_INSTALL:
    updates_install();
    break;
  default:
    break;
  }
}

const ui_screen_def updates_screen = {
    .name = "updates",
    .title_text = S("Updates"),
    .frame = updates_frame,
};
