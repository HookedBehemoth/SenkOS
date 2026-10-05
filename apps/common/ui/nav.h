/*
 * Focus, spatial navigation and scrolling for immediate-mode screens.
 *
 * A screen is a function of the app's data, run every frame. It lays things out
 * itself (ui_stack, ui_virtual_list, arithmetic) and tells nav which
 * rectangles are selectable; nav answers with flags. Nothing is registered, so
 * nothing has to be unregistered, and there are no callbacks:
 *
 *     unsigned f = nav_item(ui, nav_str(card->id), rect, clip);
 *     if (f & NAV_ACTIVATED) chosen = card;
 *
 * What nav keeps, keyed by id, is the state a screen should not have to: which
 * item has focus and every scroll region's animated offset. Moving picks the
 * nearest item that way, from where the focus is now; nothing remembers where
 * a row was left. A carousel is a horizontal scroll region with items in it; a
 * grid is a vertical one with rows of items. Neither is a type.
 *
 * Input is resolved against the rectangles of the frame on screen, at the time
 * of the event, so a key press moves the focus before the next frame is built
 * and nav_move can say whether anything was there.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui.h"

typedef uint32_t nav_id;
typedef enum { NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT } nav_dir;

enum {
  NAV_FOCUSED = 1,
  /* Enter on the focused item, or a click on this one. Reported once. */
  NAV_ACTIVATED = 2,
  NAV_HOVERED = 4,
  /* With NAV_ACTIVATED: it was the pointer, at pointer_x/pointer_y. */
  NAV_CLICKED = 8,
};

/* A season's episodes are declared whole, so a default can be off screen. */
#define NAV_ITEMS 1024
#define NAV_STATES 64
#define NAV_DEPTH 6

/* A focus fill (nav_fill): a quad reserved under a list's items */
typedef struct {
  uint32_t quad;
  gfx_color color;
  float radius;
} nav_fill_record;

typedef struct {
  nav_id id;
  ui_rect rect, ring, clip;
  ui_mask mask;
  float radius;
  nav_fill_record fill; /* color.a 0: none */
} nav_item_record;

typedef struct {
  nav_id id;
  uint64_t seen, appeared;
  /* A scroll region's offset, easing from `from` to `target`. */
  float from, target, value;
  uint64_t started_at;
} nav_state;

typedef struct {
  nav_state *state;
  ui_rect view;
  ui_axis axis;
  float extent, margin, step;
  bool inert;
} nav_scroll_frame;

typedef struct {
  nav_id focus;
  /* The item a new screen starts on, when it declares one. */
  bool want_default;
  nav_id default_id;
  /* Set by nav_move, cleared by nav_end: scroll regions bring the focus into
   * view. Pointer focus and the wheel leave it alone. */
  bool reveal;
  nav_id activated;
  bool clicked;
  /* An activation was delivered this frame: whatever it did to the screen
   * wants a frame of its own. */
  bool acted;
  float pointer_x, pointer_y, wheel;
  int steps;
  /* The innermost row under the pointer, which the wheel scrolls when no
   * list under it runs vertically. */
  nav_state *wheel_across;
  float wheel_across_limit;
  bool pointer_present;
  bool animate;
  bool inert;
  uint64_t duration, now, frame;
  ui_context *ctx;

  /* This frame's items, and the previous frame's, which input resolves
   * against. */
  nav_item_record items[2][NAV_ITEMS];
  size_t count[2];
  uint8_t current;

  nav_id scopes[NAV_DEPTH];
  uint8_t depth;
  nav_scroll_frame scrolls[NAV_DEPTH];
  uint8_t scroll_depth;
  nav_state states[NAV_STATES];

  /* The focus ring, eased from where it was drawn to the live target, so a
   * scrolling list carries it along. */
  nav_fill_record fill;
  nav_id ring_owner;
  ui_rect ring_from, ring_drawn;
  uint64_t ring_started_at, ring_frame;
} nav;

nav_id nav_str(const char *text);
nav_id nav_int(uint64_t value);
/* The full id of `local` declared inside the scope whose full id is `scope`.
 * Outside every scope an item's id is its own, so a screen can focus a
 * top-level control by name. */
nav_id nav_child(nav_id scope, nav_id local);

/* Between frames, from the event handlers. */
/* false when nothing lies that way; the caller decides what that means. */
bool nav_move(nav *ui, nav_dir where);
/* To the first item (Down, Right) or the last (Up, Left) declared: a list
 * going round. */
void nav_wrap(nav *ui, nav_dir where);
/* Round to the far side, in the same row or column: what a grid that goes round does */
void nav_wrap_around(nav *ui, nav_dir where);
void nav_activate(nav *ui);
void nav_pointer(nav *ui, float x, float y);
void nav_pointer_leave(nav *ui);
void nav_click(nav *ui);
/* Scrolls the innermost list under the pointer; a row only when no vertical
 * list is under it. */
void nav_wheel(nav *ui, float delta);
/* Scrolls the innermost vertical list that has a step by that many steps:
 * what Up and Down do over text with nothing in it to focus. */
void nav_step(nav *ui, int steps);
/* Put the focus somewhere directly, e.g. back where a screen was left. Zero
 * lets the screen choose: its default, or the first thing on screen. */
void nav_focus(nav *ui, nav_id id);

/* The frame. */
void nav_begin(nav *ui, ui_context *ctx, uint64_t now);
/* Ids inside are relative to the scope, so a card in two rows is two items. */
void nav_push(nav *ui, nav_id id);
void nav_pop(nav *ui);
#define NAV_SCOPE(ui, id)                                                      \
  for (int nav_once_ = (nav_push(ui, id), 0); !nav_once_;                      \
       nav_once_ = (nav_pop(ui), 1))
/* Returns the offset to lay the content out at. `extent` is the content's
 * length on `axis`; `margin` keeps a revealed item that far from the edges;
 * a non-zero `step` moves the content in whole steps. Also a scope. */
float nav_scroll_begin(nav *ui, nav_id id, ui_rect view, ui_axis axis,
                       float extent, float margin, float step);
void nav_scroll_end(nav *ui);
/* Forgets a top-level scroll region's offset, so the next time it is shown it
 * starts at the top: a list opened afresh rather than returned to. */
void nav_forget(nav *ui, nav_id id);
unsigned nav_item(nav *ui, nav_id id, ui_rect rect, ui_rect clip);
/* Before a nav_item: when the focus is on nothing this screen shows, it goes
 * here, brought into view, rather than to the first thing visible. */
void nav_default(nav *ui);
/* After a nav_item: nav draws its animated ring around `ring` while the item
 * has focus. Items without one draw their own focus. */
void nav_ring(nav *ui, ui_rect ring, float radius);
/* After nav_scroll_begin, before its items: their focus is also a fill under them, `radius`
 * round, that moves with the ring rather than ahead of it. Until nav_scroll_end, or the frame's end. */
void nav_fill(nav *ui, gfx_color color, float radius);
/* Items declared while set are drawn by the caller but cannot take focus, and
 * scroll regions begun while set ignore the wheel: what sits behind a modal. */
void nav_set_inert(nav *ui, bool inert);
/* Draws the focus ring. True while something is still animating, or when an
 * activation this frame may have changed what the next one shows. */
bool nav_end(nav *ui, const ui_color ring, float ring_width);
