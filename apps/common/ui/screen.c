#include "screen.h"

#include <time.h>

#include "ui.h"

static uint32_t next_instance;

static ui_screen new_screen(const ui_screen_def *def, intptr_t arg) {
  if (++next_instance == 0)
    ++next_instance;
  return (ui_screen){.def = def, .instance = next_instance, .arg = arg};
}

ui_screen *screen_top(screen_stack *stack) {
  return stack->count ? &stack->items[stack->count - 1] : NULL;
}

const ui_screen *screen_base(const screen_stack *stack) {
  if (!stack->count)
    return NULL;
  size_t at = stack->count - 1;
  while (at > 0 && stack->items[at].def->overlay)
    at--;
  return &stack->items[at];
}

const ui_screen *screen_find(const screen_stack *stack, const ui_screen_def *def) {
  if (stack == NULL)
    return NULL;
  for (size_t i = stack->count; i > 0; i--)
    if (stack->items[i - 1].def == def)
      return &stack->items[i - 1];
  return NULL;
}

const ui_screen_def *screen_current(const screen_stack *stack) {
  return stack != NULL && stack->count > 0 ? stack->items[stack->count - 1].def : NULL;
}

const ui_screen_def *screen_base_def(const screen_stack *stack) {
  const ui_screen *screen = stack != NULL ? screen_base(stack) : NULL;
  return screen != NULL ? screen->def : NULL;
}

bool screen_contains(const screen_stack *stack, const ui_screen_def *def) {
  return screen_find(stack, def) != NULL;
}

intptr_t screen_arg(const screen_stack *stack, const ui_screen_def *def) {
  const ui_screen *screen = screen_find(stack, def);
  return screen != NULL ? screen->arg : 0;
}

void screen_push(screen_stack *stack, nav *ui, const ui_screen_def *def,
                 intptr_t arg) {
  if (stack->count == SCREEN_DEPTH)
    return;
  screen_focus_save(stack, ui);
  stack->items[stack->count++] = new_screen(def, arg);
  stack->generation++;
  nav_set_owner(ui, stack->items[stack->count - 1].instance);
  nav_focus(ui, 0);
  if (def->enter != NULL)
    def->enter();
}

void screen_pop(screen_stack *stack, nav *ui) {
  if (stack->count <= 1)
    return;
  const ui_screen_def *gone = stack->items[stack->count - 1].def;
  nav_forget_owner(ui, stack->items[stack->count - 1].instance);
  stack->count--;
  if (gone->leave != NULL)
    gone->leave();
  stack->generation++;
  screen_focus_restore(stack, ui);
}

void screen_replace(screen_stack *stack, nav *ui, const ui_screen_def *def,
                    intptr_t arg) {
  if (!stack->count)
    screen_push(stack, ui, def, arg);
  else {
    const ui_screen_def *gone = stack->items[stack->count - 1].def;
    nav_forget_owner(ui, stack->items[stack->count - 1].instance);
    if (gone->leave != NULL)
      gone->leave();
    stack->items[stack->count - 1] = new_screen(def, arg);
    stack->generation++;
    nav_set_owner(ui, stack->items[stack->count - 1].instance);
    nav_focus(ui, 0);
    if (def->enter != NULL)
      def->enter();
  }
}

void screen_focus_save(screen_stack *stack, const nav *ui) {
  ui_screen *top = screen_top(stack);
  if (top != NULL)
    top->focus = ui->focus;
}

void screen_focus_restore(const screen_stack *stack, nav *ui) {
  nav_set_owner(ui, stack->count ? stack->items[stack->count - 1].instance : 0);
  nav_focus(ui, stack->count ? stack->items[stack->count - 1].focus : 0);
}

bool screen_button(screen_stack *stack, nav *ui, int button) {
  ui_screen *top = screen_top(stack);
  return top != NULL && top->def->button != NULL &&
         top->def->button(top, ui, button);
}

void screen_draw(screen_stack *stack, nav *ui, ui_rect content) {
  if (!stack->count)
    return;
  const size_t count = stack->count;
  const unsigned generation = stack->generation;
  size_t base = count - 1;
  while (base > 0 && stack->items[base].def->overlay)
    base--;
  for (size_t i = base; i < count && stack->generation == generation; i++) {
    nav_set_inert(ui, i + 1 < count);
    nav_visit_owner(ui, stack->items[i].instance);
    if (stack->items[i].def->frame != NULL)
      NAV_SCOPE(ui, nav_str(stack->items[i].def->name)) {
        stack->items[i].def->frame(&stack->items[i], ui, content);
      }
  }
  nav_set_owner(ui, stack->count ? stack->items[stack->count - 1].instance : 0);
  nav_set_inert(ui, false);
}

str screen_title(const screen_stack *stack) {
  const ui_screen *base = screen_base(stack);
  if (base == NULL)
    return S("");
  return base->def->title != NULL ? base->def->title(base)
                                  : base->def->title_text;
}

str screen_hint(const screen_stack *stack, const nav *ui) {
  if (!stack->count)
    return S("");
  const ui_screen *base = screen_base(stack);
  for (size_t i = stack->count; i > (size_t)(base - stack->items); i--) {
    const ui_screen *screen = &stack->items[i - 1];
    if (screen->def->hint != NULL) {
      const str hint = screen->def->hint(screen, ui);
      if (hint.n > 0)
        return hint;
    }
  }
  return S("");
}

enum {
  BAR = 32,
  BAR_TEXT = 18,
  BAR_ICON = 18,
  ROW = 44,
  ROW_TEXT = 22,
  MARGIN = 12
};
#define FG ui_pal.fg
#define DIM ui_pal.faint
#define MUTED ui_pal.dim
#define LOW ui_pal.bad
#define CHARGING ui_pal.ok

static time_t wall_time(void) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return now.tv_sec;
}

static void top_bar(int width, str page, const ui_status *s) {
  char clock[8];
  const time_t now = wall_time();
  const str hhmm = {clock,
                    strftime(clock, sizeof(clock), "%H:%M", localtime(&now))};
  const int y = (BAR - ui_text_height(BAR_TEXT)) / 2;
  ui_text(BAR_TEXT, MARGIN, y, hhmm, FG);
  ui_text(BAR_TEXT, (width - ui_text_width(BAR_TEXT, page)) / 2, y, page, FG);

  float x = width - MARGIN;
  if (s->battery >= 0) {
    const gfx_color level = s->charging        ? CHARGING
                            : s->battery <= 15 ? LOW
                                               : FG;
    ui_rounded(x -= 2, BAR / 2 - 3, 2, 6, FG, 1, 0);
    x -= 24;
    ui_rounded(x, BAR / 2 - 6, 24, 12, FG, 3, 1);
    ui_rounded(x + 2, BAR / 2 - 4, 20 * CLAMP(s->battery, 0, 100) / 100.0f, 8,
               level, 1.5f, 0);
    STRBUF(8) percent = {0};
    sb_fmt(percent, "%d%%", s->battery);
    x -= 6 + ui_text_width(BAR_TEXT, sb_str(percent));
    ui_text(BAR_TEXT, (int)x, y, sb_str(percent), FG);
  }
  if (s->wifi >= 0) {
    static const int arcs[] = {ICON_WIFI_OFF, ICON_WIFI_LOW, ICON_WIFI_HIGH,
                               ICON_WIFI, ICON_WIFI};
    x -= 10 + BAR_ICON;
    ui_icon(arcs[CLAMP(s->wifi, 0, 4)], BAR_ICON, (int)x, (BAR - BAR_ICON) / 2,
            s->wifi > 0 ? FG : DIM);
  }
  if (s->bluetooth >= 0) {
    x -= 10 + BAR_ICON;
    ui_icon(s->bluetooth > 0 ? ICON_BLUETOOTH_CONNECTED : ICON_BLUETOOTH, BAR_ICON, (int)x,
            (BAR - BAR_ICON) / 2, s->bluetooth > 0 ? FG : DIM);
  }
  ui_fill(0, BAR, width, 1, DIM);
}

static ui_rect chrome(int w, int h, str title, str hint, const ui_status *s) {
  top_bar(w, title, s);
  if (hint.n > 0)
    ui_text(ROW_TEXT, MARGIN * 2, h - MARGIN - ui_text_height(ROW_TEXT), hint,
            MUTED);
  return (ui_rect){0, BAR + 1, w, h - BAR - 1 - (hint.n > 0 ? ROW : 0)};
}

ui_rect screen_chrome(const screen_stack *stack, const nav *ui, int w, int h,
                      const ui_status *status) {
  return chrome(w, h, screen_title(stack), screen_hint(stack, ui), status);
}
