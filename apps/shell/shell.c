/*
 * Copyright 2010-2012 Intel Corporation
 * Copyright 2013 Raspberry Pi Foundation
 * Copyright 2011-2012,2020 Collabora, Ltd.
 * Copyright 2026 SenkOS
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Weston's shell on the handheld (`weston --shell=senkos`), from Weston 15's
 * kiosk-shell. Layers, bottom up:
 * - the apps' toplevels, fullscreen, one tree of them shown at a time (the
 *   one that mapped last), the others hidden;
 * - the launcher's (its client runs as the user "shell"), fullscreen, shown
 *   and hidden by it through senkos_shell_v1 (shell/senkos-shell-v1.xml);
 * - its overlays (the OSD, the performance numbers): their own size, at an
 *   edge or corner, never focused.
 * Also: the frames each output shows, for the launcher's power calibration (a
 * uint32_t rewritten in place in $XDG_RUNTIME_DIR/frames, counted at each
 * repaint's latch, so client buffers on a plane count too),
 * the refresh rate (kept in $XDG_CONFIG_HOME/refresh-rate, set again as each
 * output comes up, before its first frame), who may capture the screen
 * (the launcher and root), the screen on and off for the launcher, and
 * zwp_idle_inhibit_manager_v1: whether a surface shown keeps the screen on
 * is the launcher's to know (idle_inhibited), the dimming and sleeping too.
 */
#include <assert.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libweston/desktop.h>
#include <libweston/libweston.h>
#include <libweston/shell-utils.h>
#include <libweston/xwayland-api.h>
#include <libweston/zalloc.h>

#include "idle-inhibit-unstable-v1-server-protocol.h"
#include "senkos-shell-v1-server-protocol.h"

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

enum kind { APP, LAUNCHER, OVERLAY };

struct shell_seat {
  struct shell *shell;
  struct weston_seat *seat;
  struct wl_listener destroy_listener;
  struct weston_surface *focused_surface;
};

struct shell {
  struct weston_compositor *compositor;
  struct weston_desktop *desktop;
  struct wl_global *global;
  uid_t launcher_uid;

  struct wl_listener destroy_listener;
  struct wl_listener output_created_listener;
  struct wl_listener output_resized_listener;
  struct wl_listener output_moved_listener;
  struct wl_listener seat_created_listener;
  struct wl_listener transform_listener;
  struct wl_listener session_listener;
  struct wl_listener screenshot_auth;

  struct weston_layer background_layer;
  struct weston_layer app_layer;
  /* The apps' trees not shown, and the launcher's toplevels while hidden */
  struct weston_layer hidden_layer;
  struct weston_layer launcher_layer;
  struct weston_layer overlay_layer;

  struct wl_list output_list;
  struct wl_list overlay_list;
  /* The launcher's toplevels, the last focused first */
  struct wl_list launcher_list;
  bool launcher_shown;
  /* Shown at its next commit: not its last frame from before it hid */
  bool launcher_show_pending;
  struct shell_seat *seat;

  const struct weston_xwayland_surface_api *xwayland_surface_api;

  /* senkos_shell_v1's resources, for its events */
  struct wl_list shell_resources;
  struct wl_global *inhibit_global;
  struct wl_list inhibitors;
  bool inhibited;

  int frames_fd;
  uint32_t frames;
  /* The display's, in Hz; 0: the mode each output starts in */
  int refresh_hz;
};

struct shell_output {
  struct weston_output *output;
  struct shell *shell;
  struct wl_listener destroy_listener;
  struct wl_listener latch_listener;
  struct weston_curtain *curtain;
  /* set_refresh's mode, till it's set: Weston drops a switch asked for
   * while the last one's modeset is in flight, then can't commit (the kernel
   * wants a modeset for the mode it sends). The first latch after a switch is
   * its modeset, the second is after it completed. */
  struct weston_mode *want_mode;
  int latches;
  struct wl_event_source *switch_idle;
  /* The apps' tree shown */
  struct wl_list *active_surface_tree;
  struct wl_list link;
};

/* From set_overlay, kept with its wl_surface */
struct overlay {
  struct weston_surface *surface;
  uint32_t anchor;
  int32_t margin;
  struct wl_listener destroy_listener;
  struct wl_list link;
};

struct shell_surface {
  struct weston_desktop_surface *desktop_surface;
  struct weston_view *view;
  struct shell *shell;
  enum kind kind;

  struct shell_output *output;
  struct wl_listener output_destroy_listener;

  struct wl_signal parent_destroy_signal;
  struct wl_listener parent_destroy_listener;
  struct shell_surface *parent;

  /* An app's: its tree, the root's list holding the root and its descendants,
   * the top first */
  struct wl_list surface_tree_list;
  struct wl_list surface_tree_link;
  /* The launcher's: shell::launcher_list */
  struct wl_list launcher_link;

  int focus_count;
  int32_t last_width, last_height;

  struct {
    bool is_set;
    struct weston_coord_global pos;
  } xwayland;
};

static struct shell_surface *get_shell_surface(struct weston_surface *surface) {
  struct weston_desktop_surface *desktop_surface = weston_surface_get_desktop_surface(surface);
  return desktop_surface ? weston_desktop_surface_get_user_data(desktop_surface) : NULL;
}

static uid_t client_uid(struct wl_client *client) {
  uid_t uid = (uid_t)-1;
  if (client != NULL)
    wl_client_get_credentials(client, NULL, &uid, NULL);
  return uid;
}

/* May bind senkos_shell_v1 and capture the screen */
static bool trusted(const struct shell *shell, struct wl_client *client) {
  const uid_t uid = client_uid(client);
  return uid == 0 || uid == shell->launcher_uid;
}

static const struct weston_xwayland_surface_api *xwayland_api(struct shell *shell) {
  if (shell->xwayland_surface_api == NULL)
    shell->xwayland_surface_api = weston_xwayland_surface_get_api(shell->compositor);
  return shell->xwayland_surface_api;
}

static void transform_handler(struct wl_listener *listener, void *data) {
  struct weston_surface *surface = data;
  struct shell_surface *shsurf = get_shell_surface(surface);
  (void)listener;
  if (shsurf == NULL)
    return;
  const struct weston_xwayland_surface_api *api = xwayland_api(shsurf->shell);
  if (api == NULL || !api->is_xwayland_surface(surface) || !weston_view_is_mapped(shsurf->view))
    return;
  api->send_position(surface, shsurf->view->geometry.pos_offset.x, shsurf->view->geometry.pos_offset.y);
}

/* --- surfaces ------------------------------------------------------------ */

static void surface_set_output(struct shell_surface *shsurf, struct shell_output *shoutput);
static void surface_set_parent(struct shell_surface *shsurf, struct shell_surface *parent);
static void output_set_active_surface_tree(struct shell_output *shoutput, struct shell_surface *shroot);
static void output_raise_surface_subtree(struct shell_output *shoutput, struct shell_surface *shroot);

static void surface_notify_parent_destroy(struct wl_listener *listener, void *data) {
  struct shell_surface *shsurf = container_of(listener, struct shell_surface, parent_destroy_listener);
  (void)data;
  surface_set_parent(shsurf, shsurf->parent->parent);
}

static void surface_notify_output_destroy(struct wl_listener *listener, void *data) {
  struct shell_surface *shsurf = container_of(listener, struct shell_surface, output_destroy_listener);
  (void)data;
  surface_set_output(shsurf, NULL);
}

static struct shell_surface *surface_get_parent_root(struct shell_surface *shsurf) {
  while (shsurf->parent != NULL)
    shsurf = shsurf->parent;
  return shsurf;
}

static struct shell_output *surface_find_best_output(struct shell_surface *shsurf) {
  if (shsurf->output != NULL)
    return shsurf->output;
  /* All related windows on the same output */
  struct shell_surface *root = surface_get_parent_root(shsurf);
  if (root->output != NULL)
    return root->output;
  struct weston_output *output = weston_shell_utils_get_focused_output(shsurf->shell->compositor);
  if (output == NULL)
    output = weston_shell_utils_get_default_output(shsurf->shell->compositor);
  return output != NULL ? weston_output_get_shell_private(output) : NULL;
}

static void surface_set_output(struct shell_surface *shsurf, struct shell_output *shoutput) {
  shsurf->output = shoutput;
  if (shsurf->output_destroy_listener.notify != NULL) {
    wl_list_remove(&shsurf->output_destroy_listener.link);
    shsurf->output_destroy_listener.notify = NULL;
  }
  if (shsurf->output == NULL)
    return;
  shsurf->output_destroy_listener.notify = surface_notify_output_destroy;
  wl_signal_add(&shsurf->output->output->destroy_signal, &shsurf->output_destroy_listener);
}

static void surface_set_fullscreen(struct shell_surface *shsurf, struct shell_output *shoutput) {
  if (shoutput == NULL)
    shoutput = surface_find_best_output(shsurf);
  surface_set_output(shsurf, shoutput);
  weston_desktop_surface_set_fullscreen(shsurf->desktop_surface, true);
  if (shsurf->output != NULL)
    weston_desktop_surface_set_size(shsurf->desktop_surface, shsurf->output->output->width,
                                    shsurf->output->output->height);
}

static void surface_set_maximized(struct shell_surface *shsurf) {
  surface_set_output(shsurf, surface_find_best_output(shsurf));
  weston_desktop_surface_set_maximized(shsurf->desktop_surface, true);
  if (shsurf->output != NULL)
    weston_desktop_surface_set_size(shsurf->desktop_surface, shsurf->output->output->width,
                                    shsurf->output->output->height);
}

static void surface_set_normal(struct shell_surface *shsurf) {
  if (shsurf->output == NULL)
    surface_set_output(shsurf, surface_find_best_output(shsurf));
  weston_desktop_surface_set_fullscreen(shsurf->desktop_surface, false);
  weston_desktop_surface_set_maximized(shsurf->desktop_surface, false);
  weston_desktop_surface_set_size(shsurf->desktop_surface, 0, 0);
}

static bool surface_is_in_tree(struct shell_surface *shsurf, struct shell_surface *shroot) {
  struct shell_surface *s;
  wl_list_for_each(s, &shroot->surface_tree_list, surface_tree_link) {
    if (s == shsurf)
      return true;
  }
  return false;
}

static bool surface_is_descendant_of(struct shell_surface *shsurf, struct shell_surface *ancestor) {
  for (; shsurf != NULL; shsurf = shsurf->parent)
    if (shsurf == ancestor)
      return true;
  return false;
}

static void move_element_to_top(struct wl_list *list, struct wl_list *element) {
  wl_list_remove(element);
  wl_list_insert(list, element);
}

static void surface_set_parent(struct shell_surface *shsurf, struct shell_surface *parent) {
  struct shell_output *shoutput = shsurf->output;
  struct shell_surface *shroot = surface_get_parent_root(parent != NULL ? parent : shsurf);

  /* xdg clients may set a null parent on a surface without one: a no-op */
  if (parent == NULL && shsurf == shroot)
    return;

  if (shsurf->parent_destroy_listener.notify != NULL) {
    wl_list_remove(&shsurf->parent_destroy_listener.link);
    shsurf->parent_destroy_listener.notify = NULL;
  }
  shsurf->parent = parent;

  if (parent != NULL) {
    shsurf->parent_destroy_listener.notify = surface_notify_parent_destroy;
    wl_signal_add(&parent->parent_destroy_signal, &shsurf->parent_destroy_listener);
    if (!surface_is_in_tree(shsurf, shroot))
      move_element_to_top(&shroot->surface_tree_list, &shsurf->surface_tree_link);
    surface_set_output(shsurf, NULL);
    surface_set_normal(shsurf);
  } else {
    /* The child and its descendants to a tree of their own, the child its root */
    struct shell_surface *s, *tmp;
    wl_list_init(&shsurf->surface_tree_list);
    wl_list_for_each_reverse_safe(s, tmp, &shroot->surface_tree_list, surface_tree_link) {
      if (surface_is_descendant_of(s, shsurf))
        move_element_to_top(&shsurf->surface_tree_list, &s->surface_tree_link);
    }
    if (shoutput != NULL)
      output_set_active_surface_tree(shoutput, shsurf);
    surface_set_fullscreen(shsurf, shsurf->output);
  }
}

static void surface_reconfigure_for_output(struct shell_surface *shsurf) {
  if (shsurf->output == NULL || shsurf->kind == OVERLAY)
    return;
  struct weston_output *output = shsurf->output->output;
  if (weston_desktop_surface_get_maximized(shsurf->desktop_surface) ||
      weston_desktop_surface_get_fullscreen(shsurf->desktop_surface))
    weston_desktop_surface_set_size(shsurf->desktop_surface, output->width, output->height);
  weston_shell_utils_center_on_output(shsurf->view, output);
  weston_view_update_transform(shsurf->view);
}

static void surface_destroy(struct shell_surface *shsurf) {
  wl_list_remove(&shsurf->surface_tree_link);
  wl_list_remove(&shsurf->launcher_link);
  weston_desktop_surface_set_user_data(shsurf->desktop_surface, NULL);
  weston_desktop_surface_unlink_view(shsurf->view);
  weston_view_destroy(shsurf->view);
  if (shsurf->output_destroy_listener.notify != NULL)
    wl_list_remove(&shsurf->output_destroy_listener.link);
  if (shsurf->parent_destroy_listener.notify != NULL)
    wl_list_remove(&shsurf->parent_destroy_listener.link);
  free(shsurf);
}

static struct shell_surface *surface_create(struct shell *shell, struct weston_desktop_surface *desktop_surface) {
  struct wl_client *client = weston_desktop_client_get_client(weston_desktop_surface_get_client(desktop_surface));
  struct weston_view *view = weston_desktop_surface_create_view(desktop_surface);
  if (view == NULL)
    return NULL;
  struct shell_surface *shsurf = zalloc(sizeof(*shsurf));
  if (shsurf == NULL) {
    if (client != NULL)
      wl_client_post_no_memory(client);
    else
      weston_log("senkos-shell: no memory for a shell surface\n");
    weston_view_destroy(view);
    return NULL;
  }
  shsurf->desktop_surface = desktop_surface;
  shsurf->view = view;
  shsurf->shell = shell;
  weston_desktop_surface_set_user_data(desktop_surface, shsurf);
  wl_signal_init(&shsurf->parent_destroy_signal);
  /* The root of its own tree */
  wl_list_init(&shsurf->surface_tree_list);
  wl_list_init(&shsurf->surface_tree_link);
  wl_list_insert(&shsurf->surface_tree_list, &shsurf->surface_tree_link);
  wl_list_init(&shsurf->launcher_link);
  return shsurf;
}

/* Focus, xdg activation, and on top of its layer */
static void surface_activate(struct shell_surface *shsurf, uint32_t flags) {
  struct shell *shell = shsurf->shell;
  struct shell_seat *shseat = shell->seat;
  if (shseat == NULL)
    return;

  weston_view_activate_input(shsurf->view, shseat->seat, flags);
  if (shseat->focused_surface != NULL) {
    struct shell_surface *current = get_shell_surface(shseat->focused_surface);
    assert(current != NULL);
    if (--current->focus_count == 0)
      weston_desktop_surface_set_activated(current->desktop_surface, false);
  }
  shseat->focused_surface = weston_desktop_surface_get_surface(shsurf->desktop_surface);
  if (shsurf->focus_count++ == 0)
    weston_desktop_surface_set_activated(shsurf->desktop_surface, true);

  if (shsurf->kind == APP && shsurf->output != NULL) {
    output_raise_surface_subtree(shsurf->output, shsurf);
  } else if (shsurf->kind == LAUNCHER) {
    move_element_to_top(&shell->launcher_list, &shsurf->launcher_link);
    if (shell->launcher_shown)
      weston_view_move_to_layer(shsurf->view, &shell->launcher_layer.view_list);
  }
}

/* --- the launcher's layer ------------------------------------------------ */

static struct shell_surface *top_launcher(struct shell *shell, struct shell_surface *except) {
  struct shell_surface *s;
  wl_list_for_each(s, &shell->launcher_list, launcher_link) {
    if (s != except && weston_surface_is_mapped(weston_desktop_surface_get_surface(s->desktop_surface)))
      return s;
  }
  return NULL;
}

/* The top of the apps' tree shown */
static struct shell_surface *top_app(struct shell *shell, struct shell_surface *except) {
  struct weston_view *view;
  wl_list_for_each(view, &shell->app_layer.view_list.link, layer_link.link) {
    struct shell_surface *s = get_shell_surface(view->surface);
    if (s != NULL && s != except && s->kind == APP)
      return s;
  }
  return NULL;
}

static void inhibit_update(struct shell *shell);

static void launcher_set_shown(struct shell *shell, bool shown) {
  struct shell_surface *s;
  shell->launcher_shown = shown;
  shell->launcher_show_pending = false;
  /* Bottom up: each goes on top */
  wl_list_for_each_reverse(s, &shell->launcher_list, launcher_link) {
    if (weston_surface_is_mapped(weston_desktop_surface_get_surface(s->desktop_surface)))
      weston_view_move_to_layer(s->view, shown ? &shell->launcher_layer.view_list : &shell->hidden_layer.view_list);
  }
  inhibit_update(shell);
}

/* Shown: the launcher's top toplevel focused; hidden: the app's */
static void focus_top(struct shell *shell, struct shell_surface *except) {
  struct shell_surface *top = shell->launcher_shown ? top_launcher(shell, except) : NULL;
  if (top == NULL)
    top = top_app(shell, except);
  if (top != NULL)
    surface_activate(top, WESTON_ACTIVATE_FLAG_NONE);
}

/* --- seats --------------------------------------------------------------- */

static void seat_handle_destroy(struct wl_listener *listener, void *data) {
  struct shell_seat *shseat = container_of(listener, struct shell_seat, destroy_listener);
  (void)data;
  shseat->shell->seat = NULL;
  wl_list_remove(&shseat->destroy_listener.link);
  free(shseat);
}

static void seat_create(struct shell *shell, struct weston_seat *seat) {
  if (shell->seat != NULL) {
    weston_log("senkos-shell: a seat more, ignored: one is all it handles\n");
    return;
  }
  struct shell_seat *shseat = zalloc(sizeof(*shseat));
  if (shseat == NULL) {
    weston_log("senkos-shell: no memory for a seat\n");
    return;
  }
  shseat->shell = shell;
  shseat->seat = seat;
  shseat->destroy_listener.notify = seat_handle_destroy;
  wl_signal_add(&seat->destroy_signal, &shseat->destroy_listener);
  shell->seat = shseat;
}

/* --- outputs ------------------------------------------------------------- */

static void output_set_active_surface_tree(struct shell_output *shoutput, struct shell_surface *shroot) {
  struct shell *shell = shoutput->shell;
  struct shell_surface *s;
  if (shoutput->active_surface_tree != NULL) {
    wl_list_for_each_reverse(s, shoutput->active_surface_tree, surface_tree_link)
        weston_view_move_to_layer(s->view, &shell->hidden_layer.view_list);
  }
  if (shroot != NULL) {
    wl_list_for_each_reverse(s, &shroot->surface_tree_list, surface_tree_link)
        weston_view_move_to_layer(s->view, &shell->app_layer.view_list);
  }
  shoutput->active_surface_tree = shroot != NULL ? &shroot->surface_tree_list : NULL;
  inhibit_update(shell);
}

/* The subtree from shroot of the tree shown to the top of the apps' layer */
static void output_raise_surface_subtree(struct shell_output *shoutput, struct shell_surface *shroot) {
  struct shell *shell = shroot->shell;
  struct wl_list tmp_list;
  struct shell_surface *s, *tmp_s;
  if (shoutput->active_surface_tree == NULL)
    return;
  /* Out to tmp_list, in their order; then each on top, of the layer and the tree */
  wl_list_init(&tmp_list);
  wl_list_for_each_reverse_safe(s, tmp_s, shoutput->active_surface_tree, surface_tree_link) {
    if (surface_is_descendant_of(s, shroot))
      move_element_to_top(&tmp_list, &s->surface_tree_link);
  }
  wl_list_for_each_reverse_safe(s, tmp_s, &tmp_list, surface_tree_link) {
    weston_view_move_to_layer(s->view, &shell->app_layer.view_list);
    move_element_to_top(shoutput->active_surface_tree, &s->surface_tree_link);
  }
}

static int background_get_label(struct weston_surface *surface, char *buf, size_t len) {
  (void)surface;
  return snprintf(buf, len, "senkos shell background surface");
}

static void output_recreate_background(struct shell_output *shoutput) {
  struct shell *shell = shoutput->shell;
  struct weston_output *output = shoutput->output;
  if (shoutput->curtain != NULL)
    weston_shell_utils_curtain_destroy(shoutput->curtain);
  shoutput->curtain = NULL;
  if (output == NULL)
    return;
  struct weston_curtain_params params = {
      .r = 0,
      .g = 0,
      .b = 0,
      .a = 1,
      .pos = output->pos,
      .width = output->width,
      .height = output->height,
      .capture_input = true,
      .get_label = background_get_label,
  };
  shoutput->curtain = weston_shell_utils_curtain_create(shell->compositor, &params);
  weston_surface_set_role(shoutput->curtain->view->surface, "senkos-shell-background", NULL, 0);
  shoutput->curtain->view->surface->output = output;
  weston_view_move_to_layer(shoutput->curtain->view, &shell->background_layer.view_list);
  weston_view_set_output(shoutput->curtain->view, output);
}

static void output_destroy(struct shell_output *shoutput) {
  if (shoutput->switch_idle != NULL)
    wl_event_source_remove(shoutput->switch_idle);
  if (shoutput->curtain != NULL)
    weston_shell_utils_curtain_destroy(shoutput->curtain);
  wl_list_remove(&shoutput->destroy_listener.link);
  wl_list_remove(&shoutput->latch_listener.link);
  wl_list_remove(&shoutput->link);
  free(shoutput);
}

static void output_notify_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  output_destroy(container_of(listener, struct shell_output, destroy_listener));
}

static void output_switch_mode(struct shell_output *shoutput);

static void switch_idle(void *data) {
  struct shell_output *shoutput = data;
  shoutput->switch_idle = NULL;
  output_switch_mode(shoutput);
}

static void output_latched(struct wl_listener *listener, void *data) {
  struct shell_output *shoutput = container_of(listener, struct shell_output, latch_listener);
  struct shell *shell = shoutput->shell;
  (void)data;
  if (shoutput->latches < 2)
    shoutput->latches++;
  /* Not in the middle of a repaint */
  if (shoutput->want_mode != NULL && shoutput->switch_idle == NULL)
    shoutput->switch_idle =
        wl_event_loop_add_idle(wl_display_get_event_loop(shell->compositor->wl_display), switch_idle, shoutput);
  shell->frames++;
  /* Failed, the launcher reads the count before: no worse */
  if (shell->frames_fd >= 0 && pwrite(shell->frames_fd, &shell->frames, sizeof(shell->frames), 0) < 0)
    return;
}

static bool output_set_refresh(struct shell_output *shoutput, int hz);

/* $XDG_CONFIG_HOME/refresh-rate: "120", in Hz */
static FILE *refresh_file(const char *mode) {
  const char *dir = getenv("XDG_CONFIG_HOME");
  char path[256];
  if (dir == NULL)
    return NULL;
  snprintf(path, sizeof(path), "%s/refresh-rate", dir);
  return fopen(path, mode);
}

static void output_create(struct shell *shell, struct weston_output *output) {
  struct shell_output *shoutput = zalloc(sizeof(*shoutput));
  if (shoutput == NULL)
    return;
  shoutput->output = output;
  shoutput->shell = shell;
  shoutput->latches = 2;
  shoutput->destroy_listener.notify = output_notify_destroy;
  wl_signal_add(&output->destroy_signal, &shoutput->destroy_listener);
  /* frame_signal is only for frames Weston renders itself, and a listener on
   * it makes the DRM backend render every frame, client buffers on planes too */
  shoutput->latch_listener.notify = output_latched;
  wl_signal_add(&output->post_latch_signal, &shoutput->latch_listener);
  wl_list_insert(shell->output_list.prev, &shoutput->link);
  weston_output_set_shell_private(output, shoutput);
  output_recreate_background(shoutput);
  weston_output_set_ready(output);
  output_set_refresh(shoutput, shell->refresh_hz);
}

/* To want_mode once the last switch is done; else a repaint, for the latches */
static void output_switch_mode(struct shell_output *shoutput) {
  struct weston_output *output = shoutput->output;
  struct weston_mode *want = shoutput->want_mode;
  if (want == NULL || want == output->current_mode) {
    shoutput->want_mode = NULL;
    return;
  }
  if (shoutput->latches < 2) {
    weston_output_schedule_repaint(output);
    return;
  }
  shoutput->want_mode = NULL;
  shoutput->latches = 0;
  weston_log("senkos-shell: %s at %d mHz\n", output->name, want->refresh);
  weston_output_mode_set_native(output, want, output->current_scale);
}

/* To its mode of that rate (in Hz, rounded) at its size, if it has one */
static bool output_set_refresh(struct shell_output *shoutput, int hz) {
  struct weston_output *output = shoutput->output;
  struct weston_mode *mode;
  wl_list_for_each(mode, &output->mode_list, link) {
    if (mode->width == output->current_mode->width && mode->height == output->current_mode->height &&
        ((int)mode->refresh + 500) / 1000 == hz) {
      shoutput->want_mode = mode;
      output_switch_mode(shoutput);
      return true;
    }
  }
  return false;
}

/* Set, and kept for the next start; a rate no output has, ignored */
static void set_refresh(struct shell *shell, int mhz) {
  struct shell_output *shoutput;
  const int hz = (mhz + 500) / 1000;
  bool any = false;
  wl_list_for_each(shoutput, &shell->output_list, link) any |= output_set_refresh(shoutput, hz);
  if (!any)
    return;
  shell->refresh_hz = hz;
  FILE *f = refresh_file("w");
  if (f != NULL) {
    fprintf(f, "%d\n", shell->refresh_hz);
    fclose(f);
  }
}

/* --- overlays ------------------------------------------------------------ */

static struct overlay *find_overlay(struct shell *shell, struct weston_surface *surface) {
  struct overlay *o;
  wl_list_for_each(o, &shell->overlay_list, link) {
    if (o->surface == surface)
      return o;
  }
  return NULL;
}

static void overlay_surface_destroyed(struct wl_listener *listener, void *data) {
  struct overlay *o = container_of(listener, struct overlay, destroy_listener);
  (void)data;
  wl_list_remove(&o->link);
  wl_list_remove(&o->destroy_listener.link);
  free(o);
}

/* At its edges, margin in; centred on the others */
static void overlay_place(struct shell_surface *shsurf) {
  struct weston_surface *surface = weston_desktop_surface_get_surface(shsurf->desktop_surface);
  const struct overlay *o = find_overlay(shsurf->shell, surface);
  if (shsurf->output == NULL)
    surface_set_output(shsurf, surface_find_best_output(shsurf));
  if (o == NULL || shsurf->output == NULL)
    return;
  const struct weston_output *output = shsurf->output->output;
  struct weston_coord_global pos = output->pos;
  const int free_x = output->width - surface->width, free_y = output->height - surface->height;
  pos.c.x += o->anchor & SENKOS_SHELL_V1_ANCHOR_LEFT    ? o->margin
             : o->anchor & SENKOS_SHELL_V1_ANCHOR_RIGHT ? free_x - o->margin
                                                        : free_x / 2;
  pos.c.y += o->anchor & SENKOS_SHELL_V1_ANCHOR_TOP      ? o->margin
             : o->anchor & SENKOS_SHELL_V1_ANCHOR_BOTTOM ? free_y - o->margin
                                                         : free_y / 2;
  weston_view_set_position(shsurf->view, pos);
}

/* --- senkos_shell_v1 ----------------------------------------------------- */

static void shell_destroy_request(struct wl_client *client, struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static void shell_set_overlay(struct wl_client *client, struct wl_resource *resource,
                              struct wl_resource *surface_resource, uint32_t anchor, int32_t margin) {
  struct shell *shell = wl_resource_get_user_data(resource);
  struct weston_surface *surface = wl_resource_get_user_data(surface_resource);
  struct overlay *o = find_overlay(shell, surface);
  if (o == NULL) {
    if ((o = zalloc(sizeof(*o))) == NULL) {
      wl_client_post_no_memory(client);
      return;
    }
    o->surface = surface;
    o->destroy_listener.notify = overlay_surface_destroyed;
    wl_signal_add(&surface->destroy_signal, &o->destroy_listener);
    wl_list_insert(&shell->overlay_list, &o->link);
  }
  o->anchor = anchor;
  o->margin = margin;
  /* Moved: placed now, not at its next commit */
  struct shell_surface *shsurf = get_shell_surface(surface);
  if (shsurf != NULL && shsurf->kind == OVERLAY && weston_surface_is_mapped(surface))
    overlay_place(shsurf);
}

static void shell_show(struct wl_client *client, struct wl_resource *resource) {
  struct shell *shell = wl_resource_get_user_data(resource);
  (void)client;
  if (!shell->launcher_shown)
    shell->launcher_show_pending = true;
}

static void shell_hide(struct wl_client *client, struct wl_resource *resource) {
  struct shell *shell = wl_resource_get_user_data(resource);
  (void)client;
  launcher_set_shown(shell, false);
  focus_top(shell, NULL);
}

static pid_t client_pgid(struct wl_client *client) {
  pid_t pid = 0;
  if (client != NULL)
    wl_client_get_credentials(client, &pid, NULL, NULL);
  return pid > 0 ? getpgid(pid) : -1;
}

static void shell_show_app(struct wl_client *client, struct wl_resource *resource, int32_t pgid) {
  struct shell *shell = wl_resource_get_user_data(resource);
  struct weston_layer *layers[] = {&shell->app_layer, &shell->hidden_layer};
  struct weston_view *view;
  (void)client;
  for (size_t i = 0; i < COUNTOF(layers); i++) {
    wl_list_for_each(view, &layers[i]->view_list.link, layer_link.link) {
      struct shell_surface *s = get_shell_surface(view->surface);
      if (s == NULL || s->kind != APP || s->output == NULL ||
          client_pgid(weston_desktop_client_get_client(weston_desktop_surface_get_client(s->desktop_surface))) != pgid)
        continue;
      struct shell_surface *root = surface_get_parent_root(s);
      if (s->output->active_surface_tree != &root->surface_tree_list)
        output_set_active_surface_tree(s->output, root);
      if (!shell->launcher_shown)
        focus_top(shell, NULL);
      return;
    }
  }
}

static void shell_set_refresh(struct wl_client *client, struct wl_resource *resource, int32_t mhz) {
  (void)client;
  set_refresh(wl_resource_get_user_data(resource), mhz);
}

static void shell_set_screen(struct wl_client *client, struct wl_resource *resource, uint32_t on) {
  struct shell *shell = wl_resource_get_user_data(resource);
  (void)client;
  if (on)
    weston_compositor_wake(shell->compositor);
  else
    weston_compositor_sleep(shell->compositor);
}

static const struct senkos_shell_v1_interface shell_implementation = {
    .destroy = shell_destroy_request,
    .set_overlay = shell_set_overlay,
    .show = shell_show,
    .hide = shell_hide,
    .show_app = shell_show_app,
    .set_refresh = shell_set_refresh,
    .set_screen = shell_set_screen,
};

static void shell_resource_destroyed(struct wl_resource *resource) { wl_list_remove(wl_resource_get_link(resource)); }

static void bind_shell(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
  struct shell *shell = data;
  struct wl_resource *resource = wl_resource_create(client, &senkos_shell_v1_interface, (int)version, id);
  if (resource == NULL) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(resource, &shell_implementation, data, shell_resource_destroyed);
  wl_list_insert(&shell->shell_resources, wl_resource_get_link(resource));
  senkos_shell_v1_send_idle_inhibited(resource, shell->inhibited);
}

/* --- zwp_idle_inhibit_manager_v1 ----------------------------------------- */

struct inhibitor {
  struct shell *shell;
  struct weston_surface *surface; /* NULL once gone: then it does nothing */
  struct wl_listener surface_destroy;
  struct wl_list link;
};

/* Shown: its toplevel's view in the launcher's layer, or in the apps' with the launcher's hidden */
static bool inhibitor_shown(const struct inhibitor *inh) {
  if (inh->surface == NULL)
    return false;
  const struct shell *shell = inh->shell;
  const struct shell_surface *s = get_shell_surface(weston_surface_get_main_surface(inh->surface));
  if (s == NULL || s->view == NULL)
    return false;
  const struct weston_layer *layer = s->view->layer_link.layer;
  return layer == &shell->launcher_layer || (layer == &shell->app_layer && !shell->launcher_shown);
}

static void inhibit_update(struct shell *shell) {
  bool inhibited = false;
  struct inhibitor *inh;
  wl_list_for_each(inh, &shell->inhibitors, link) inhibited |= inhibitor_shown(inh);
  if (inhibited == shell->inhibited)
    return;
  shell->inhibited = inhibited;
  struct wl_resource *resource;
  wl_resource_for_each(resource, &shell->shell_resources) senkos_shell_v1_send_idle_inhibited(resource, inhibited);
}

static void inhibitor_surface_destroyed(struct wl_listener *listener, void *data) {
  struct inhibitor *inh = container_of(listener, struct inhibitor, surface_destroy);
  (void)data;
  wl_list_remove(&inh->surface_destroy.link);
  inh->surface = NULL;
  inhibit_update(inh->shell);
}

static void inhibitor_destroyed(struct wl_resource *resource) {
  struct inhibitor *inh = wl_resource_get_user_data(resource);
  if (inh->surface != NULL)
    wl_list_remove(&inh->surface_destroy.link);
  wl_list_remove(&inh->link);
  struct shell *shell = inh->shell;
  free(inh);
  inhibit_update(shell);
}

static const struct zwp_idle_inhibitor_v1_interface inhibitor_implementation = {
    .destroy = shell_destroy_request,
};

static void inhibit_create(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                           struct wl_resource *surface_resource) {
  struct shell *shell = wl_resource_get_user_data(resource);
  struct inhibitor *inh = zalloc(sizeof(*inh));
  struct wl_resource *r = inh != NULL ? wl_resource_create(client, &zwp_idle_inhibitor_v1_interface,
                                                           wl_resource_get_version(resource), id)
                                      : NULL;
  if (r == NULL) {
    free(inh);
    wl_client_post_no_memory(client);
    return;
  }
  inh->shell = shell;
  inh->surface = wl_resource_get_user_data(surface_resource);
  inh->surface_destroy.notify = inhibitor_surface_destroyed;
  wl_signal_add(&inh->surface->destroy_signal, &inh->surface_destroy);
  wl_list_insert(&shell->inhibitors, &inh->link);
  wl_resource_set_implementation(r, &inhibitor_implementation, inh, inhibitor_destroyed);
  inhibit_update(shell);
}

static const struct zwp_idle_inhibit_manager_v1_interface inhibit_manager_implementation = {
    .destroy = shell_destroy_request,
    .create_inhibitor = inhibit_create,
};

static void bind_inhibit_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
  struct wl_resource *resource = wl_resource_create(client, &zwp_idle_inhibit_manager_v1_interface, (int)version, id);
  if (resource == NULL) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(resource, &inhibit_manager_implementation, data, NULL);
}

/* Only the launcher (and root) see it */
static bool global_filter(const struct wl_client *client, const struct wl_global *global, void *data) {
  const struct shell *shell = data;
  return global != shell->global || trusted(shell, (struct wl_client *)client);
}

/* --- libweston-desktop --------------------------------------------------- */

static void desktop_surface_added(struct weston_desktop_surface *desktop_surface, void *data) {
  struct shell *shell = data;
  struct weston_surface *surface = weston_desktop_surface_get_surface(desktop_surface);
  struct shell_surface *shsurf = surface_create(shell, desktop_surface);
  if (shsurf == NULL)
    return;
  weston_surface_set_label_func(surface, weston_shell_utils_surface_get_label);

  /* xdg-shell adds a toplevel at its first commit: set_overlay came before */
  struct wl_client *client = weston_desktop_client_get_client(weston_desktop_surface_get_client(desktop_surface));
  if (find_overlay(shell, surface) != NULL) {
    shsurf->kind = OVERLAY;
    surface_set_output(shsurf, surface_find_best_output(shsurf));
    return;
  }
  if (client_uid(client) == shell->launcher_uid) {
    shsurf->kind = LAUNCHER;
    wl_list_insert(&shell->launcher_list, &shsurf->launcher_link);
  }
  surface_set_fullscreen(shsurf, NULL);
}

/* The top app view on the same output other than shsurf's, of its tree if
 * there's one left, from the tree shown, else the hidden ones */
static struct shell_surface *app_successor(struct shell_surface *shsurf) {
  struct shell *shell = shsurf->shell;
  struct shell_surface *parent_root = surface_get_parent_root(shsurf);
  struct weston_layer *layers[] = {&shell->app_layer, &shell->hidden_layer};
  struct weston_view *top_view = NULL, *view;
  if (shsurf->output == NULL)
    return NULL;
  for (size_t i = 0; i < COUNTOF(layers); i++) {
    wl_list_for_each(view, &layers[i]->view_list.link, layer_link.link) {
      struct shell_surface *s = get_shell_surface(view->surface);
      if (view == shsurf->view || view->output != shsurf->output->output || s == NULL || s->kind != APP)
        continue;
      if (top_view == NULL)
        top_view = view;
      if (surface_get_parent_root(s) == parent_root) {
        top_view = view;
        break;
      }
    }
  }
  return top_view != NULL ? get_shell_surface(top_view->surface) : NULL;
}

static void desktop_surface_removed(struct weston_desktop_surface *desktop_surface, void *data) {
  struct shell *shell = data;
  struct shell_surface *shsurf = weston_desktop_surface_get_user_data(desktop_surface);
  struct weston_surface *surface = weston_desktop_surface_get_surface(desktop_surface);
  if (shsurf == NULL)
    return;

  /* The children reparented (and maybe relinked to trees of their own) first */
  wl_signal_emit(&shsurf->parent_destroy_signal, shsurf);

  /* The root of the tree shown gone: the next tree shown */
  struct shell_output *shoutput = shsurf->output;
  if (shsurf->kind == APP && shoutput != NULL && shoutput->active_surface_tree == &shsurf->surface_tree_list) {
    struct shell_surface *next = app_successor(shsurf);
    output_set_active_surface_tree(shoutput, next != NULL ? surface_get_parent_root(next) : NULL);
  }

  struct shell_seat *shseat = shell->seat;
  if (shseat != NULL && shseat->focused_surface == surface) {
    shseat->focused_surface = NULL;
    focus_top(shell, shsurf);
  }
  surface_destroy(shsurf);
  inhibit_update(shell);
}

static void map_app(struct shell_surface *shsurf) {
  struct shell *shell = shsurf->shell;
  if (shsurf->parent == NULL) {
    /* A new tree: shown, over the launcher, replacing the tree shown */
    launcher_set_shown(shell, false);
    output_set_active_surface_tree(shsurf->output, shsurf);
    surface_activate(shsurf, WESTON_ACTIVATE_FLAG_NONE);
  } else if (!shell->launcher_shown) {
    surface_activate(shsurf, WESTON_ACTIVATE_FLAG_NONE);
  } else {
    /* Under the launcher, unfocused till it hides */
    output_raise_surface_subtree(shsurf->output, shsurf);
  }
}

static void desktop_surface_committed(struct weston_desktop_surface *desktop_surface,
                                      struct weston_coord_surface buf_offset, void *data) {
  struct shell_surface *shsurf = weston_desktop_surface_get_user_data(desktop_surface);
  struct weston_surface *surface = weston_desktop_surface_get_surface(desktop_surface);
  (void)data;
  assert(shsurf != NULL);
  if (surface->width == 0)
    return;

  if (shsurf->kind == OVERLAY) {
    overlay_place(shsurf);
    if (!weston_surface_is_mapped(surface)) {
      weston_view_move_to_layer(shsurf->view, &shsurf->shell->overlay_layer.view_list);
      weston_surface_map(surface);
    }
    return;
  }
  if (shsurf->output == NULL)
    surface_set_output(shsurf, surface_find_best_output(shsurf));
  if (shsurf->output == NULL)
    return;

  const bool is_resized = surface->width != shsurf->last_width || surface->height != shsurf->last_height;
  const bool is_fullscreen = weston_desktop_surface_get_maximized(desktop_surface) ||
                             weston_desktop_surface_get_fullscreen(desktop_surface);
  if (!weston_surface_is_mapped(surface) || (is_resized && is_fullscreen)) {
    if (is_fullscreen || !shsurf->xwayland.is_set) {
      weston_shell_utils_center_on_output(shsurf->view, shsurf->output->output);
    } else {
      const struct weston_geometry geometry = weston_desktop_surface_get_geometry(desktop_surface);
      const struct weston_coord_surface offset = weston_coord_surface(-geometry.x, -geometry.y, shsurf->view->surface);
      weston_view_set_position_with_offset(shsurf->view, shsurf->xwayland.pos, offset);
    }
    weston_view_update_transform(shsurf->view);
  }

  if (!weston_surface_is_mapped(surface)) {
    weston_surface_map(surface);
    if (shsurf->kind == LAUNCHER) {
      launcher_set_shown(shsurf->shell, true);
      surface_activate(shsurf, WESTON_ACTIVATE_FLAG_NONE);
    } else {
      map_app(shsurf);
    }
    inhibit_update(shsurf->shell);
  } else if (shsurf->kind == LAUNCHER && shsurf->shell->launcher_show_pending) {
    launcher_set_shown(shsurf->shell, true);
    focus_top(shsurf->shell, NULL);
  }

  if (!is_fullscreen && (buf_offset.c.x != 0 || buf_offset.c.y != 0)) {
    const struct weston_coord_global pos = weston_view_get_pos_offset_global(shsurf->view);
    weston_view_set_position_with_offset(shsurf->view, pos, buf_offset);
    weston_view_update_transform(shsurf->view);
  }
  shsurf->last_width = surface->width;
  shsurf->last_height = surface->height;
}

static void desktop_surface_move(struct weston_desktop_surface *desktop_surface, struct weston_seat *seat,
                                 uint32_t serial, void *shell) {
  (void)desktop_surface, (void)seat, (void)serial, (void)shell;
}

static void desktop_surface_resize(struct weston_desktop_surface *desktop_surface, struct weston_seat *seat,
                                   uint32_t serial, enum weston_desktop_surface_edge edges, void *shell) {
  (void)desktop_surface, (void)seat, (void)serial, (void)edges, (void)shell;
}

static void desktop_surface_set_parent(struct weston_desktop_surface *desktop_surface,
                                       struct weston_desktop_surface *parent, void *shell) {
  struct shell_surface *shsurf = weston_desktop_surface_get_user_data(desktop_surface);
  struct shell_surface *shsurf_parent = parent != NULL ? weston_desktop_surface_get_user_data(parent) : NULL;
  (void)shell;
  if (shsurf->kind == APP && (shsurf_parent == NULL || shsurf_parent->kind == APP))
    surface_set_parent(shsurf, shsurf_parent);
}

/* Toplevels are fullscreen from the start; but Xwayland sets its WM state
 * itself, maybe dropping fullscreen before telling: applied again */
static void desktop_surface_fullscreen_requested(struct weston_desktop_surface *desktop_surface, bool fullscreen,
                                                 struct weston_output *output, void *shell) {
  struct shell_surface *shsurf = weston_desktop_surface_get_user_data(desktop_surface);
  (void)shell;
  if (shsurf->kind == OVERLAY)
    return;
  if (shsurf->parent == NULL || fullscreen)
    surface_set_fullscreen(shsurf, output != NULL ? weston_output_get_shell_private(output) : NULL);
  else
    surface_set_normal(shsurf);
}

static void desktop_surface_maximized_requested(struct weston_desktop_surface *desktop_surface, bool maximized,
                                                void *shell) {
  struct shell_surface *shsurf = weston_desktop_surface_get_user_data(desktop_surface);
  (void)shell;
  if (shsurf->kind == OVERLAY)
    return;
  if (shsurf->parent == NULL)
    surface_set_fullscreen(shsurf, NULL);
  else if (maximized)
    surface_set_maximized(shsurf);
  else
    surface_set_normal(shsurf);
}

static void desktop_surface_minimized_requested(struct weston_desktop_surface *desktop_surface, void *shell) {
  (void)desktop_surface, (void)shell;
}

static void desktop_surface_ping_timeout(struct weston_desktop_client *client, void *shell) {
  (void)client, (void)shell;
}

static void desktop_surface_pong(struct weston_desktop_client *client, void *shell) { (void)client, (void)shell; }

static void desktop_surface_set_xwayland_position(struct weston_desktop_surface *desktop_surface,
                                                  struct weston_coord_global pos, void *shell) {
  struct shell_surface *shsurf = weston_desktop_surface_get_user_data(desktop_surface);
  (void)shell;
  shsurf->xwayland.pos = pos;
  shsurf->xwayland.is_set = true;
}

static void desktop_surface_get_position(struct weston_desktop_surface *desktop_surface, int32_t *x, int32_t *y,
                                         void *shell) {
  struct shell_surface *shsurf = weston_desktop_surface_get_user_data(desktop_surface);
  (void)shell;
  *x = shsurf->view->geometry.pos_offset.x;
  *y = shsurf->view->geometry.pos_offset.y;
}

static const struct weston_desktop_api desktop_api = {
    .struct_size = sizeof(struct weston_desktop_api),
    .surface_added = desktop_surface_added,
    .surface_removed = desktop_surface_removed,
    .committed = desktop_surface_committed,
    .move = desktop_surface_move,
    .resize = desktop_surface_resize,
    .set_parent = desktop_surface_set_parent,
    .fullscreen_requested = desktop_surface_fullscreen_requested,
    .maximized_requested = desktop_surface_maximized_requested,
    .minimized_requested = desktop_surface_minimized_requested,
    .ping_timeout = desktop_surface_ping_timeout,
    .pong = desktop_surface_pong,
    .set_xwayland_position = desktop_surface_set_xwayland_position,
    .get_position = desktop_surface_get_position,
};

/* --- the shell ----------------------------------------------------------- */

static void handle_output_created(struct wl_listener *listener, void *data) {
  output_create(container_of(listener, struct shell, output_created_listener), data);
}

static void handle_output_resized(struct wl_listener *listener, void *data) {
  struct shell *shell = container_of(listener, struct shell, output_resized_listener);
  struct weston_output *output = data;
  struct weston_layer *layers[] = {&shell->app_layer, &shell->launcher_layer};
  struct weston_view *view;
  output_recreate_background(weston_output_get_shell_private(output));
  for (size_t i = 0; i < COUNTOF(layers); i++) {
    wl_list_for_each(view, &layers[i]->view_list.link, layer_link.link) {
      struct shell_surface *shsurf = get_shell_surface(view->surface);
      if (view->output == output && shsurf != NULL)
        surface_reconfigure_for_output(shsurf);
    }
  }
}

static void handle_output_moved(struct wl_listener *listener, void *data) {
  struct shell *shell = container_of(listener, struct shell, output_moved_listener);
  struct weston_output *output = data;
  struct weston_layer *layers[] = {&shell->background_layer, &shell->app_layer, &shell->launcher_layer};
  struct weston_view *view;
  for (size_t i = 0; i < COUNTOF(layers); i++) {
    wl_list_for_each(view, &layers[i]->view_list.link, layer_link.link) {
      if (view->output == output)
        weston_view_set_position(view, weston_coord_global_add(weston_view_get_pos_offset_global(view), output->move));
    }
  }
}

static void handle_seat_created(struct wl_listener *listener, void *data) {
  seat_create(container_of(listener, struct shell, seat_created_listener), data);
}

/* Back on the VT: the focus as it was */
static void handle_session(struct wl_listener *listener, void *data) {
  struct shell *shell = container_of(listener, struct shell, session_listener);
  struct weston_compositor *compositor = data;
  if (!compositor->session_active || shell->seat == NULL || shell->seat->focused_surface == NULL)
    return;
  struct shell_surface *focused = get_shell_surface(shell->seat->focused_surface);
  if (focused != NULL)
    weston_view_activate_input(focused->view, shell->seat->seat, WESTON_ACTIVATE_FLAG_NONE);
}

/* The launcher's pause screen captures the app it covers; `launcher
 * screenshot` runs as root */
static void authorize_capture(struct wl_listener *listener, struct weston_output_capture_attempt *attempt) {
  struct shell *shell = container_of(listener, struct shell, screenshot_auth);
  if (trusted(shell, attempt->who->client))
    attempt->authorized = true;
}

static void destroy_surfaces_on_layer(struct weston_layer *layer) {
  struct weston_view *view, *view_next;
  wl_list_for_each_safe(view, view_next, &layer->view_list.link, layer_link.link) {
    struct shell_surface *shsurf = get_shell_surface(view->surface);
    assert(shsurf != NULL);
    surface_destroy(shsurf);
  }
  weston_layer_fini(layer);
}

static void shell_destroy(struct wl_listener *listener, void *data) {
  struct shell *shell = container_of(listener, struct shell, destroy_listener);
  struct shell_output *shoutput, *tmp;
  struct overlay *o, *otmp;
  (void)data;
  wl_list_remove(&shell->destroy_listener.link);
  wl_list_remove(&shell->output_created_listener.link);
  wl_list_remove(&shell->output_resized_listener.link);
  wl_list_remove(&shell->output_moved_listener.link);
  wl_list_remove(&shell->seat_created_listener.link);
  wl_list_remove(&shell->transform_listener.link);
  wl_list_remove(&shell->session_listener.link);
  wl_list_remove(&shell->screenshot_auth.link);
  wl_list_for_each_safe(shoutput, tmp, &shell->output_list, link) output_destroy(shoutput);
  wl_list_for_each_safe(o, otmp, &shell->overlay_list, link) overlay_surface_destroyed(&o->destroy_listener, NULL);
  /* The background views went with their outputs */
  weston_layer_fini(&shell->background_layer);
  destroy_surfaces_on_layer(&shell->app_layer);
  destroy_surfaces_on_layer(&shell->hidden_layer);
  destroy_surfaces_on_layer(&shell->launcher_layer);
  destroy_surfaces_on_layer(&shell->overlay_layer);
  if (shell->seat != NULL)
    seat_handle_destroy(&shell->seat->destroy_listener, NULL);
  wl_global_destroy(shell->global);
  wl_global_destroy(shell->inhibit_global);
  weston_desktop_destroy(shell->desktop);
  if (shell->frames_fd >= 0)
    close(shell->frames_fd);
  free(shell);
}

WL_EXPORT int wet_shell_init(struct weston_compositor *compositor, int *argc, char *argv[]) {
  struct weston_seat *seat;
  struct weston_output *output;
  (void)argc, (void)argv;

  struct shell *shell = zalloc(sizeof(*shell));
  if (shell == NULL)
    return -1;
  shell->compositor = compositor;
  if (!weston_compositor_add_destroy_listener_once(compositor, &shell->destroy_listener, shell_destroy)) {
    free(shell);
    return 0;
  }
  const struct passwd *pw = getpwnam("shell");
  shell->launcher_uid = pw != NULL ? pw->pw_uid : (uid_t)-1;
  shell->launcher_shown = true;
  wl_list_init(&shell->output_list);
  wl_list_init(&shell->overlay_list);
  wl_list_init(&shell->launcher_list);
  wl_list_init(&shell->shell_resources);
  wl_list_init(&shell->inhibitors);

  const char *dir = getenv("XDG_RUNTIME_DIR");
  char path[256];
  snprintf(path, sizeof(path), "%s/frames", dir != NULL ? dir : "/tmp");
  shell->frames_fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (shell->frames_fd < 0)
    weston_log("senkos-shell: can't open %s\n", path);
  FILE *f = refresh_file("r");
  if (f != NULL) {
    if (fscanf(f, "%d", &shell->refresh_hz) != 1)
      shell->refresh_hz = 0;
    fclose(f);
  }

  shell->transform_listener.notify = transform_handler;
  wl_signal_add(&compositor->transform_signal, &shell->transform_listener);

  weston_layer_init(&shell->background_layer, compositor);
  weston_layer_init(&shell->app_layer, compositor);
  weston_layer_init(&shell->hidden_layer, compositor);
  weston_layer_init(&shell->launcher_layer, compositor);
  weston_layer_init(&shell->overlay_layer, compositor);
  weston_layer_set_position(&shell->background_layer, WESTON_LAYER_POSITION_BACKGROUND);
  weston_layer_set_position(&shell->hidden_layer, WESTON_LAYER_POSITION_HIDDEN);
  /* NORMAL: Xwayland puts its override-redirect windows at NORMAL + 1 */
  weston_layer_set_position(&shell->app_layer, WESTON_LAYER_POSITION_NORMAL);
  weston_layer_set_position(&shell->launcher_layer, WESTON_LAYER_POSITION_FULLSCREEN);
  weston_layer_set_position(&shell->overlay_layer, WESTON_LAYER_POSITION_TOP_UI);

  shell->desktop = weston_desktop_create(compositor, &desktop_api, shell);
  if (shell->desktop == NULL)
    return -1;
  shell->global = wl_global_create(compositor->wl_display, &senkos_shell_v1_interface, 1, shell, bind_shell);
  shell->inhibit_global =
      wl_global_create(compositor->wl_display, &zwp_idle_inhibit_manager_v1_interface, 1, shell, bind_inhibit_manager);
  if (shell->global == NULL || shell->inhibit_global == NULL)
    return -1;
  wl_display_set_global_filter(compositor->wl_display, global_filter, shell);

  wl_list_for_each(seat, &compositor->seat_list, link) seat_create(shell, seat);
  shell->seat_created_listener.notify = handle_seat_created;
  wl_signal_add(&compositor->seat_created_signal, &shell->seat_created_listener);

  wl_list_for_each(output, &compositor->output_list, link) output_create(shell, output);
  shell->output_created_listener.notify = handle_output_created;
  wl_signal_add(&compositor->output_created_signal, &shell->output_created_listener);
  shell->output_resized_listener.notify = handle_output_resized;
  wl_signal_add(&compositor->output_resized_signal, &shell->output_resized_listener);
  shell->output_moved_listener.notify = handle_output_moved;
  wl_signal_add(&compositor->output_moved_signal, &shell->output_moved_listener);

  shell->session_listener.notify = handle_session;
  wl_signal_add(&compositor->session_signal, &shell->session_listener);
  weston_compositor_add_screenshot_authority(compositor, &shell->screenshot_auth, authorize_capture);
  return 0;
}
