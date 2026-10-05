#include "keyboard.h"

enum { KEY_H = 44, GAP = 4, TEXT = 20, FIELD = 40, COLS = 10 };

static const gfx_color FG = {0xee, 0xee, 0xee, 0xff};
static const gfx_color DIM = {0x8a, 0x8a, 0x94, 0xff};
static const gfx_color KEY = {0x26, 0x27, 0x2e, 0xff};
static const gfx_color FOCUS = {0x3a, 0x40, 0x52, 0xff};
static const gfx_color FIELD_BG = {0x1c, 0x1d, 0x23, 0xff};

/* Four rows of ten, each layer; a key is one UTF-8 character */
static const char *const layers[3][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm,._"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM,._"},
    {"!@#$%^&*()", "~`=+[]{}\\|", ";:'\"<>/?-_", "€£§°,.´^!?"},
};
static int layer;
static STRBUF(128) text;

void keyboard_open(void) {
  text.n = 0;
  text.p[0] = '\0';
  layer = 0;
}

str keyboard_text(void) { return sb_str(text); }

void keyboard_delete(void) {
  /* A whole UTF-8 character */
  while (text.n > 0 && (text.p[--text.n] & 0xc0) == 0x80)
    ;
  text.p[text.n] = '\0';
}

void keyboard_shift(void) { layer = layer == 0 ? 1 : 0; }

/* The `i`th UTF-8 character of `row` */
static str key_at(const char *row, int i) {
  str s = str_c(row);
  for (size_t at = 0; at < s.n; i--) {
    size_t n = 1;
    while (at + n < s.n && (s.p[at + n] & 0xc0) == 0x80)
      n++;
    if (i == 0)
      return (str){s.p + at, n};
    at += n;
  }
  return S("");
}

static bool button(nav *ui, nav_id id, ui_rect r, str label, ui_rect clip) {
  const unsigned flags = nav_item(ui, id, r, clip);
  nav_ring(ui, r, 6);
  ui_rounded(r.x, r.y, r.w, r.h, flags & NAV_FOCUSED ? FOCUS : KEY, 6, 0);
  ui_text(TEXT, (int)(r.x + (r.w - ui_text_width(TEXT, label)) / 2), (int)(r.y + (r.h - ui_text_height(TEXT)) / 2),
          label, FG);
  return flags & NAV_ACTIVATED;
}

bool keyboard(nav *ui, ui_rect view, str label) {
  /* The field: what's typed so far, its end showing */
  const float x0 = view.x + GAP * 3, w = view.w - GAP * 6;
  ui_text(TEXT - 4, (int)x0, (int)view.y + GAP * 2, label, DIM);
  const float fy = view.y + GAP * 2 + ui_text_height(TEXT - 4) + GAP;
  ui_rounded(x0, fy, w, FIELD, FIELD_BG, 6, 0);
  str shown = sb_str(text);
  while (shown.n > 0 && ui_text_width(TEXT, shown) > w - 24)
    shown = str_at(shown, 1);
  const int tx = (int)x0 + 10 + ui_text_width(TEXT, shown);
  ui_text(TEXT, (int)x0 + 10, (int)(fy + (FIELD - ui_text_height(TEXT)) / 2), shown, FG);
  ui_fill(tx + 1, fy + 8, 2, FIELD - 16, DIM);

  const float kw = (w - GAP * (COLS - 1)) / COLS;
  float y = fy + FIELD + GAP * 3;
  bool done = false;
  for (int row = 0; row < 4; row++, y += KEY_H + GAP)
    for (int col = 0; col < COLS; col++) {
      const str k = key_at(layers[layer][row], col);
      const ui_rect r = {x0 + col * (kw + GAP), y, kw, KEY_H};
      if (button(ui, nav_int((uint64_t)(row * COLS + col)), r, k, view))
        sb_add(text, k);
    }
  /* Shift, symbols, space, Done */
  const float wide = (w - GAP * 3) / 4;
  if (button(ui, nav_int(100), (ui_rect){x0, y, wide, KEY_H}, layer == 1 ? S("abc") : S("Shift"), view))
    layer = layer == 1 ? 0 : 1;
  if (button(ui, nav_int(101), (ui_rect){x0 + (wide + GAP), y, wide, KEY_H}, layer == 2 ? S("abc") : S("#+="), view))
    layer = layer == 2 ? 0 : 2;
  if (button(ui, nav_int(102), (ui_rect){x0 + (wide + GAP) * 2, y, wide, KEY_H}, S("Space"), view))
    sb_add(text, S(" "));
  if (button(ui, nav_int(103), (ui_rect){x0 + (wide + GAP) * 3, y, wide, KEY_H}, S("Done"), view))
    done = true;
  return done;
}
