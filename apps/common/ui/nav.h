/* Focus, activation, and scrolling for immediate-mode screens. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui.h"

typedef uint32_t nav_id;
typedef enum { NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT } nav_dir;

enum {
  NAV_FOCUSED = 1,
  NAV_ACTIVATED = 2,
};

/* Bounds for one frame and its retained navigation state. */
#define NAV_ITEMS 1024
#define NAV_STATES 64
#define NAV_DEPTH 6
#define NAV_OWNERS 8
#define NAV_FRAME_BYTES (128 * 1024)
#define NAV_DEFERRED 16

typedef struct nav nav;
typedef void (*nav_action)(nav *ui, void *data);

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
  uint32_t owner;
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

struct nav {
  nav_id focus;
  /* The item a new screen starts on, when it declares one. */
  bool want_default;
  nav_id default_id;
  /* Set by nav_move, cleared by nav_end: scroll regions bring the focus into
   * view. */
  bool reveal;
  nav_id activated;
  /* An activation was delivered this frame: whatever it did to the screen
   * wants a frame of its own. */
  bool acted;
  int steps;
  bool animate;
  bool inert;
  uint64_t duration, now, frame;
  ui_context *ctx;

  nav_item_record items[NAV_ITEMS];
  size_t count;

  nav_id scopes[NAV_DEPTH];
  uint8_t depth;
  nav_scroll_frame scrolls[NAV_DEPTH];
  uint8_t scroll_depth;
  nav_state states[NAV_STATES];
  uint32_t owner;
  uint32_t visible_owners[NAV_OWNERS];
  uint8_t visible_count;
  union {
    max_align_t align;
    unsigned char data[NAV_FRAME_BYTES];
  } frame_memory;
  size_t frame_used;
  struct {
    nav_action run;
    void *data;
  } deferred[NAV_DEFERRED];
  size_t deferred_count;

  /* The focus ring, eased from where it was drawn to the live target, so a
   * scrolling list carries it along. */
  nav_fill_record fill;
  nav_id ring_owner;
  ui_rect ring_from, ring_drawn;
  uint64_t ring_started_at, ring_frame;
};

/* Frame data stays alive through nav_run_deferred, then resets at nav_begin. */
void *nav_frame_alloc(nav *ui, size_t bytes);
#define NAV_FRAME(ui, type, count) ((type *)nav_frame_alloc(ui, sizeof(type) * (count)))
void nav_defer(nav *ui, nav_action run, const void *data, size_t bytes);
void nav_run_deferred(nav *ui);
void nav_set_owner(nav *ui, uint32_t owner);
void nav_visit_owner(nav *ui, uint32_t owner);
void nav_forget_owner(nav *ui, uint32_t owner);

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
/* Scrolls the innermost vertical list that has a step by that many steps:
 * what Up and Down do over text with nothing in it to focus. */
void nav_step(nav *ui, int steps);
/* Put the focus somewhere directly, e.g. back where a screen was left; the
 * ring jumps there, as on a new screen. Zero lets the screen choose: its
 * default, or the first thing on screen. */
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
/* Items behind a modal cannot take focus or scroll. */
void nav_set_inert(nav *ui, bool inert);
/* Draws the focus ring. True while something is still animating, or when an
 * activation this frame may have changed what the next one shows. */
bool nav_end(nav *ui, gfx_color ring, float ring_width);
