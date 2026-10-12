#include "nav.h"

#include <math.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

void *nav_frame_alloc(nav *ui, size_t bytes) {
  const size_t align = alignof(max_align_t);
  const size_t at = (ui->frame_used + align - 1) & ~(align - 1);
  if (at > NAV_FRAME_BYTES || bytes > NAV_FRAME_BYTES - at)
    abort();
  void *memory = ui->frame_memory.data + at;
  memset(memory, 0, bytes);
  ui->frame_used = at + bytes;
  return memory;
}

void nav_defer(nav *ui, nav_action run, const void *data, size_t bytes) {
  if (ui->deferred_count == NAV_DEFERRED)
    abort();
  void *copy = nav_frame_alloc(ui, bytes);
  memcpy(copy, data, bytes);
  ui->deferred[ui->deferred_count++] = (typeof(ui->deferred[0])){run, copy};
}

void nav_run_deferred(nav *ui) {
  for (size_t i = 0; i < ui->deferred_count; i++)
    ui->deferred[i].run(ui, ui->deferred[i].data);
  ui->deferred_count = 0;
}

void nav_set_owner(nav *ui, uint32_t owner) { ui->owner = owner; }

void nav_visit_owner(nav *ui, uint32_t owner) {
  ui->owner = owner;
  if (ui->visible_count < NAV_OWNERS)
    ui->visible_owners[ui->visible_count++] = owner;
}

void nav_forget_owner(nav *ui, uint32_t owner) {
  for (size_t i = 0; i < NAV_STATES; i++)
    if (ui->states[i].id != 0 && ui->states[i].owner == owner)
      memset(&ui->states[i], 0, sizeof(ui->states[i]));
}

static nav_id combine(nav_id seed, nav_id local) {
  const nav_id id =
      seed == 0 ? local
                : seed ^ (local + 0x9e3779b9u + (seed << 6) + (seed >> 2));
  return id != 0 ? id : 1;
}

nav_id nav_child(nav_id scope, nav_id local) { return combine(scope, local); }

nav_id nav_str(const char *text) {
  nav_id hash = 2166136261u;
  for (; *text != '\0'; text++)
    hash = (hash ^ (uint8_t)*text) * 16777619u;
  return hash;
}

nav_id nav_int(uint64_t value) {
  /* splitmix64, with its increment: without it 0 hashes to 0, which is no
   * id at all. */
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return (nav_id)(value ^ (value >> 31));
}

static float ease(uint64_t started_at, uint64_t now, uint64_t duration) {
  if (started_at == 0 || duration == 0 || now - started_at >= duration)
    return 1;
  const float progress = (float)(now - started_at) / (float)duration;
  return 1.0f - powf(2.0f, -10.0f * progress);
}

/* Linear scan of 64 slots, evicting the longest unseen. A hash table
 * if a screen ever holds more scopes than that. */
static nav_state *state_for(nav *ui, nav_id id) {
  nav_state *oldest = &ui->states[0];
  for (size_t i = 0; i < NAV_STATES; i++) {
    if (ui->states[i].id == id && ui->states[i].owner == ui->owner)
      return &ui->states[i];
    if (ui->states[i].id == 0 || ui->states[i].seen < oldest->seen)
      oldest = &ui->states[i];
  }
  memset(oldest, 0, sizeof(*oldest));
  oldest->id = id;
  oldest->owner = ui->owner;
  return oldest;
}

static const nav_item_record *shown(const nav *ui, nav_id id) {
  for (size_t i = 0; i < ui->count; i++)
    if (ui->items[i].id == id)
      return &ui->items[i];
  return NULL;
}

/* Android's FocusFinder weighting: distance along the move dominates, and
 * sideways offset only counts where the two do not overlap; between equals,
 * the one more nearly in line with the focus wins, so Down from a seek bar
 * lands under its middle rather than at its left end. Anything
 * overlapping sideways beats anything that does not, and Left and Right never
 * leave the band at all: at the end of a row they stop rather than climb into
 * the next one, while Down from a long row still reaches a shorter one. */
static bool score(ui_rect from, ui_rect to, nav_dir where, float *out) {
  float major, minor, centre, ahead;
  if (where == NAV_LEFT || where == NAV_RIGHT) {
    major =
        where == NAV_RIGHT ? to.x - (from.x + from.w) : from.x - (to.x + to.w);
    minor = fmaxf(0, fmaxf(to.y - (from.y + from.h), from.y - (to.y + to.h)));
    centre = (to.y + to.h / 2) - (from.y + from.h / 2);
    ahead = (to.x + to.w / 2) - (from.x + from.w / 2);
  } else {
    major =
        where == NAV_DOWN ? to.y - (from.y + from.h) : from.y - (to.y + to.h);
    minor = fmaxf(0, fmaxf(to.x - (from.x + from.w), from.x - (to.x + to.w)));
    centre = (to.x + to.w / 2) - (from.x + from.w / 2);
    ahead = (to.y + to.h / 2) - (from.y + from.h / 2);
  }
  /* Beyond by its centre, not its edge: hit areas may overlap, as the seek
   * bar's does the tops of the buttons under it. */
  if (where == NAV_LEFT || where == NAV_UP)
    ahead = -ahead;
  if (ahead < 1 || (minor > 0 && (where == NAV_LEFT || where == NAV_RIGHT)))
    return false;
  major = fmaxf(0, major);
  *out = 13 * major * major + minor * minor + 0.01f * centre * centre +
         (minor > 0 ? 1e12f : 0);
  return true;
}

/* The item nearest `from` that way, `skip` aside */
static const nav_item_record *nearest(const nav *ui, ui_rect from, const nav_item_record *skip, nav_dir where) {
  const nav_item_record *items = ui->items;
  const nav_item_record *best = NULL;
  float best_score = INFINITY;
  for (size_t i = 0; i < ui->count; i++) {
    float s;
    if (&items[i] != skip && score(from, items[i].rect, where, &s) && s < best_score) {
      best = &items[i];
      best_score = s;
    }
  }
  return best;
}

bool nav_move(nav *ui, nav_dir where) {
  const nav_item_record *items = ui->items;
  const nav_item_record *from = shown(ui, ui->focus);
  if (from == NULL) {
    if (ui->count == 0)
      return false;
    ui->focus = items[0].id;
    ui->reveal = true;
    return true;
  }
  const nav_item_record *best = nearest(ui, from->rect, from, where);
  if (best == NULL)
    return false;
  ui->focus = best->id;
  ui->reveal = true;
  return true;
}

void nav_wrap_around(nav *ui, nav_dir where) {
  const nav_item_record *items = ui->items;
  const nav_item_record *from = shown(ui, ui->focus);
  if (from == NULL)
    return;
  /* The focus's rect moved past the far side of everything, then the nearest that way */
  ui_rect r = from->rect;
  for (size_t i = 0; i < ui->count; i++) {
    const ui_rect o = items[i].rect;
    if (where == NAV_RIGHT)
      r.x = fminf(r.x, o.x - r.w - 1);
    else if (where == NAV_LEFT)
      r.x = fmaxf(r.x, o.x + o.w + 1);
    else if (where == NAV_DOWN)
      r.y = fminf(r.y, o.y - r.h - 1);
    else
      r.y = fmaxf(r.y, o.y + o.h + 1);
  }
  const nav_item_record *best = nearest(ui, r, NULL, where);
  if (best != NULL) {
    ui->focus = best->id;
    ui->reveal = true;
  }
}

void nav_wrap(nav *ui, nav_dir where) {
  const size_t n = ui->count;
  if (n == 0)
    return;
  ui->focus = ui->items[where == NAV_UP || where == NAV_LEFT ? n - 1 : 0].id;
  ui->reveal = true;
}

void nav_activate(nav *ui) {
  ui->activated = ui->focus;
}
void nav_focus(nav *ui, nav_id id) {
  ui->focus = id;
  ui->reveal = true;
  /* Another screen, or one opened over this: the ring starts there rather than flying over */
  ui->ring_frame = 0;
}
void nav_step(nav *ui, int steps) { ui->steps += steps; }
void nav_begin(nav *ui, ui_context *ctx, uint64_t now) {
  if (ui->deferred_count != 0)
    abort();
  ui->frame_used = 0;
  ui->owner = 0;
  ui->visible_count = 0;
  ui->ctx = ctx;
  ui->fill = (nav_fill_record){0};
  ui->now = now;
  ui->frame++;
  ui->count = 0;
  ui->depth = 0;
  ui->scroll_depth = 0;
  ui->inert = false;
  ui->want_default = false;
  ui->default_id = 0;
}

/* Scopes deeper than NAV_DEPTH share the deepest one's ids. */
static nav_id scoped(const nav *ui, nav_id id) {
  const uint8_t depth = ui->depth < NAV_DEPTH ? ui->depth : NAV_DEPTH;
  return combine(depth ? ui->scopes[depth - 1] : 0, id);
}

void nav_push(nav *ui, nav_id id) {
  const nav_id full = scoped(ui, id);
  if (ui->depth < NAV_DEPTH)
    ui->scopes[ui->depth] = full;
  ui->depth++;
}

void nav_pop(nav *ui) { ui->depth--; }

static void scroll_to(nav *ui, nav_state *state, float target) {
  if (fabsf(state->target - target) < 0.01f)
    return;
  /* A list that has only just appeared goes straight to its focus: the
   * fallback that picks it lands a frame after the list does. */
  const bool animate = ui->animate && ui->frame - state->appeared > 1;
  state->from = state->value;
  state->target = target;
  state->started_at = animate ? ui->now : 0;
  /* This frame was laid out at the old offset: without an animation to keep
   * frames coming, ask for the one that shows the new offset. */
  if (!animate) {
    state->value = target;
    ui->acted = true;
  }
}

static float scroll_limit(const nav_scroll_frame *scroll) {
  const float length =
      scroll->axis == UI_HORIZONTAL ? scroll->view.w : scroll->view.h;
  return fmaxf(0, scroll->extent - length);
}

float nav_scroll_begin(nav *ui, nav_id id, ui_rect view, ui_axis axis,
                       float extent, float margin, float step) {
  nav_state *state = state_for(ui, scoped(ui, id));
  /* A region that was not on screen last frame appears where it belongs
   * rather than scrolling there. */
  if (state->seen + 1 != ui->frame) {
    state->started_at = 0;
    state->appeared = ui->frame;
  }
  state->seen = ui->frame;
  nav_push(ui, id);
  nav_scroll_frame *scroll = &ui->scrolls[ui->scroll_depth++];
  *scroll =
      (nav_scroll_frame){state, view, axis, extent, margin, step, ui->inert};
  state->target = fminf(fmaxf(state->target, 0), scroll_limit(scroll));
  const float t = ease(state->started_at, ui->now, ui->duration);
  if (t >= 1)
    state->started_at = 0;
  state->value = state->from + (state->target - state->from) * t;
  return state->value;
}

void nav_scroll_end(nav *ui) {
  ui->fill = (nav_fill_record){0};
  nav_scroll_frame *scroll = &ui->scrolls[--ui->scroll_depth];
  const float limit = scroll_limit(scroll);
  if (!scroll->inert && scroll->axis == UI_VERTICAL &&
      ui->steps != 0 && scroll->step > 0) {
    scroll_to(ui, scroll->state,
              fminf(fmaxf(scroll->state->target + ui->steps * scroll->step,
                          0), limit));
    ui->steps = 0;
  }
  nav_pop(ui);
}

void nav_forget(nav *ui, nav_id id) {
  for (size_t i = 0; i < NAV_STATES; i++)
    if (ui->states[i].id == id && ui->states[i].owner == ui->owner)
      memset(&ui->states[i], 0, sizeof(ui->states[i]));
}

static void reveal(nav *ui, const nav_scroll_frame *scroll, ui_rect rect) {
  const bool across = scroll->axis == UI_HORIZONTAL;
  const float length = across ? scroll->view.w : scroll->view.h;
  /* In content coordinates, which do not move while the region animates. */
  const float low =
      (across ? rect.x - scroll->view.x : rect.y - scroll->view.y) +
      scroll->state->value;
  const float high = low + (across ? rect.w : rect.h);
  const float current = scroll->state->target;
  float target = current;
  if (high - target > length - scroll->margin)
    target = high - (length - scroll->margin);
  if (low - target < scroll->margin)
    target = low - scroll->margin;
  if (scroll->step > 0 && target != current)
    target = (target > current ? ceilf(target / scroll->step - 0.001f)
                               : floorf(target / scroll->step + 0.001f)) *
             scroll->step;
  scroll_to(ui, scroll->state, fminf(fmaxf(target, 0), scroll_limit(scroll)));
}

unsigned nav_item(nav *ui, nav_id id, ui_rect rect, ui_rect clip) {
  if (ui->inert || ui->count == NAV_ITEMS)
    return 0;
  nav_item_record *item = &ui->items[ui->count++];
  *item = (nav_item_record){
      .id = scoped(ui, id),
      .rect = rect,
      .clip = clip,
      .mask = ui->ctx->mask,
      .fill = ui->fill,
  };
  if (ui->want_default) {
    ui->default_id = item->id;
    ui->want_default = false;
  }

  unsigned flags = 0;
  if (item->id == ui->activated) {
    flags |= NAV_ACTIVATED;
    ui->activated = 0;
    ui->acted = true;
  }
  if (item->id == ui->focus) {
    flags |= NAV_FOCUSED;
    if (ui->reveal)
      for (uint8_t i = 0; i < ui->scroll_depth; i++)
        reveal(ui, &ui->scrolls[i], rect);
  }
  return flags;
}

void nav_ring(nav *ui, ui_rect ring, float radius) {
  /* An inert nav_item recorded nothing: the last record is another's */
  if (ui->inert || ui->count == 0)
    return;
  nav_item_record *item = &ui->items[ui->count - 1];
  item->ring = ring;
  item->radius = radius;
}

void nav_set_inert(nav *ui, bool inert) { ui->inert = inert; }

void nav_fill(nav *ui, gfx_color color, float radius) {
  ui->fill = (nav_fill_record){gfx_reserve(), color, radius};
}
void nav_default(nav *ui) { ui->want_default = true; }

/* Where the focus goes when it is on nothing shown: the screen's default, or
 * the first item wholly in view - not merely the first declared, which in a
 * virtual list is the half-hidden row above the viewport. */
static const nav_item_record *fallback(nav *ui) {
  const nav_item_record *items = ui->items;
  const nav_item_record *chosen = shown(ui, ui->default_id);
  if (chosen != NULL)
    return chosen;
  for (size_t i = 0; i < ui->count; i++) {
    const ui_rect r = items[i].rect, c = items[i].clip;
    if (r.x >= c.x && r.y >= c.y && r.x + r.w <= c.x + c.w &&
        r.y + r.h <= c.y + c.h)
      return &items[i];
  }
  return &items[0];
}

static float mix(float a, float b, float t) { return a + (b - a) * t; }

bool nav_end(nav *ui, gfx_color color, float width) {
  bool running = ui->acted;
  ui->acted = false;
  const nav_item_record *focused = shown(ui, ui->focus);
  /* A reveal waits for its item: a screen still loading declares nothing. */
  if (focused != NULL)
    ui->reveal = false;
  /* Choose a visible item when the focused item disappears. */
  if (focused == NULL && ui->count > 0) {
    ui->focus = fallback(ui)->id;
    ui->reveal = ui->focus == ui->default_id;
    running = true;
  }
  if (focused != NULL && focused->ring.w > 0) {
    if (ui->ring_owner != focused->id) {
      const bool was_drawn = ui->ring_frame + 1 == ui->frame;
      ui->ring_from = was_drawn ? ui->ring_drawn : focused->ring;
      ui->ring_started_at = was_drawn && ui->animate ? ui->now : 0;
      ui->ring_owner = focused->id;
    }
    const float t = ease(ui->ring_started_at, ui->now, ui->duration);
    const ui_rect to = focused->ring;
    ui->ring_drawn = (ui_rect){
        mix(ui->ring_from.x, to.x, t), mix(ui->ring_from.y, to.y, t),
        mix(ui->ring_from.w, to.w, t), mix(ui->ring_from.h, to.h, t)};
    ui->ring_frame = ui->frame;
    running |= t < 1;
    const ui_rect r = ui->ring_drawn, c = focused->clip;
    if (focused->fill.color.a > 0)
      gfx_set(focused->fill.quad, (gfx_quad){.x = r.x, .y = r.y, .w = r.w, .h = r.h, .u1 = 1, .v1 = 1,
                                             .clip = {c.x, c.y, c.x + c.w, c.y + c.h},
                                             .color = gfx_rgba(focused->fill.color), .tex = GFX_WHITE,
                                             .radius = focused->fill.radius});
    const ui_mask outer = ui->ctx->mask;
    ui->ctx->mask = focused->mask;
    ui_stroke(ui->ctx, ui_inset(ui->ring_drawn, -width), &focused->clip,
                color, width, focused->radius);
    ui->ctx->mask = outer;
  }
  for (size_t i = 0; i < NAV_STATES; i++)
    running |= ui->states[i].seen == ui->frame && ui->states[i].started_at != 0;
  for (size_t i = 0; i < NAV_STATES; i++) {
    nav_state *state = &ui->states[i];
    if (state->id == 0 || ui->frame - state->seen <= 2)
      continue;
    bool visible = state->owner == 0;
    for (size_t j = 0; j < ui->visible_count; j++)
      visible |= ui->visible_owners[j] == state->owner;
    if (visible)
      memset(state, 0, sizeof(*state));
  }
  ui->activated = 0;
  ui->steps = 0;
  return running;
}
