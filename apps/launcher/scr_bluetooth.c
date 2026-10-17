/* Bluetooth settings: on or off and the paired devices; pairing new ones on a screen of its own,
 * which searches while it's up. */
#include "bluetooth.h"
#include "launcher_internal.h"

typedef STRBUF(120) row_label;
/* A choice, with its device: the list may change before it's run */
typedef struct {
  int action;
  bool checked;
  STRBUF(64) path;
} choice;

static void choose(nav *ui, void *data);

static const int icons[] = {[BT_OTHER] = ICON_BLUETOOTH, [BT_AUDIO] = ICON_HEADPHONES, [BT_PAD] = ICON_GAMEPAD_2};

static void status_line(ui_rect *content, str text) {
  const int line = ui_text_height(ROW_TEXT) + MARGIN;
  ui_text(ROW_TEXT, MARGIN * 2, (int)content->y + MARGIN,
          ui_text_fit(ROW_TEXT, text, launcher.draw.w - MARGIN * 4, NULL), DIM);
  content->y += line;
  content->h -= line;
}

static void pick(nav *ui, const entry *rows, size_t n, ui_rect content, const bt_device *const *of) {
  const entry *selected = menu(ui, rows, n, content);
  if (selected == NULL)
    return;
  choice c = {.action = selected->action, .checked = selected->checked};
  if (of[selected - rows] != NULL)
    sb_set(c.path, sb_str(of[selected - rows]->path));
  nav_defer(ui, choose, &c, sizeof(c));
}

/* The paired ones, rows from 1 (0 is on or off); `of` each row's device */
static size_t known(const bt_device **of, size_t max) {
  size_t count, n = 0;
  const bt_device *all = bt_devices(&count);
  for (size_t i = 0; i < count && n < max; i++)
    if (all[i].p.paired)
      of[n++] = &all[i];
  return n;
}

static void bluetooth_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  entry *rows = NAV_FRAME(ui, entry, BT_MAX + 2);
  row_label *labels = NAV_FRAME(ui, row_label, BT_MAX);
  const bt_device **of = NAV_FRAME(ui, const bt_device *, BT_MAX + 2);

  const int on = bt_powered();
  const bt_device *paired[BT_MAX];
  const size_t count = on > 0 ? known(paired, BT_MAX) : 0;
  const bt_device *connected = NULL;
  size_t n = 0;
  if (on >= 0) {
    of[n] = NULL;
    rows[n++] = (entry){.label = S("Bluetooth"), .action = BT_TOGGLE,
                        .icon = on ? ICON_BLUETOOTH : ICON_BLUETOOTH_OFF, .checked = on};
  }
  for (size_t i = 0; i < count; i++) {
    const bt_device *d = paired[i];
    if (d->p.connected && connected == NULL)
      connected = d;
    sb_set(labels[i], str_c(d->p.alias),
           bt_busy(d->path.p) ? S("  ·  …") : d->p.connected ? S("  ·  connected") : S(""));
    of[n] = d;
    rows[n++] = (entry){.label = sb_str(labels[i]), .action = BT_PICK, .icon = icons[bt_kind(d)],
                        .checked = d->p.connected};
  }
  if (on > 0) {
    of[n] = NULL;
    rows[n++] = (entry){.label = S("Pair new device…"), .action = BT_PAIRING, .icon = ICON_SEARCH};
  }
  STRBUF(96) status;
  if (connected != NULL)
    sb_set(status, S("Connected to "), str_c(connected->p.alias));
  else
    sb_set(status, on < 0 ? S("No Bluetooth") : on ? S("On") : S("Off"));
  status_line(&content, sb_str(status));
  pick(ui, rows, n, content, of);
}

/* The focused paired device, NULL on another row */
static const bt_device *focused(const nav *ui) {
  const bt_device *paired[BT_MAX];
  const size_t count = bt_powered() > 0 ? known(paired, BT_MAX) : 0;
  const int row = focused_row(ui, nav_str("bluetooth"), count + 2) - 1;
  return row >= 0 && (size_t)row < count ? paired[row] : NULL;
}

static str bluetooth_hint(const ui_screen *screen, const nav *ui) {
  (void)screen;
  return focused(ui) != NULL ? S("X: forget") : S("");
}

static bool bluetooth_button(ui_screen *screen, nav *ui, int button) {
  (void)screen;
  if (button != APPCTL_X)
    return false;
  const bt_device *d = focused(ui);
  if (d != NULL)
    bt_forget(d->path.p);
  return true;
}

static void pairing_frame(ui_screen *screen, nav *ui, ui_rect content) {
  (void)screen;
  launcher_state *state = &launcher;
  if (bt_paired_new()) {
    screen_pop(state->visible, ui);
    return;
  }
  entry *rows = NAV_FRAME(ui, entry, BT_MAX);
  row_label *labels = NAV_FRAME(ui, row_label, BT_MAX);
  const bt_device **of = NAV_FRAME(ui, const bt_device *, BT_MAX);

  size_t count, n = 0;
  const bt_device *all = bt_devices(&count);
  const bt_device *pairing = NULL;
  /* As they were heard, named ones */
  for (size_t i = 0; i < count; i++) {
    const bt_device *d = &all[i];
    if (d->p.paired || d->p.name[0] == '\0')
      continue;
    if (bt_busy(d->path.p))
      pairing = d;
    sb_set(labels[n], str_c(d->p.name),
           bt_busy(d->path.p) ? S("  ·  pairing…") : d->gone ? S("  ·  gone") : S(""));
    of[n] = d;
    rows[n] = (entry){.label = sb_str(labels[n]), .action = BT_PAIR, .icon = icons[bt_kind(d)]};
    n++;
  }
  STRBUF(96) status;
  if (pairing != NULL)
    sb_set(status, S("Pairing with "), str_c(pairing->p.name), S("…"));
  else
    sb_set(status, bt_powered() <= 0 ? S("Bluetooth is off")
                   : bt_searching()  ? S("Searching… Put the device in pairing mode.")
                                     : S("Not searching"));
  status_line(&content, sb_str(status));
  pick(ui, rows, n, content, of);
}

static void choose(nav *ui, void *data) {
  launcher_state *state = &launcher;
  const choice *c = data;
  switch (c->action) {
  case BT_TOGGLE:
    bt_power(!c->checked);
    break;
  case BT_PICK:
    bt_connect(c->path.p);
    break;
  case BT_PAIRING:
    screen_push(state->visible, ui, &bt_pairing_screen, 0);
    break;
  case BT_PAIR:
    bt_pair(c->path.p);
    break;
  default:
    break;
  }
}

const ui_screen_def bluetooth_screen = {.name = "bluetooth",
                                        .frame = bluetooth_frame,
                                        .button = bluetooth_button,
                                        .title_text = S("Bluetooth"),
                                        .hint = bluetooth_hint};
const ui_screen_def bt_pairing_screen = {.name = "bt-pairing", .frame = pairing_frame, .title_text = S("Pair new device")};
