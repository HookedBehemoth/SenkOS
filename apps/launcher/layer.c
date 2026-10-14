#include "layer.h"

#include <string.h>

#include <wayland-client.h>

#include "senkos-shell-v1-client-protocol.h"

static struct senkos_shell_v1 *shell;

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
  (void)data, (void)version;
  if (strcmp(interface, senkos_shell_v1_interface.name) == 0)
    shell = wl_registry_bind(registry, name, &senkos_shell_v1_interface, 1);
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
  (void)data, (void)registry, (void)name;
}
static const struct wl_registry_listener registry_listener = {global, global_remove};

void layer_connect(void) {
  struct wl_registry *registry = wl_display_get_registry(win_display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(win_display);
  wl_registry_destroy(registry);
}

void layer_show(win *w, bool shown) {
  if (shown && !w->configured)
    win_map(w);
  else if (shell == NULL && !shown)
    win_unmap(w);
  if (shell != NULL)
    shown ? senkos_shell_v1_show(shell) : senkos_shell_v1_hide(shell);
}

void layer_overlay(win *w, unsigned anchor, int margin) {
  if (shell == NULL)
    return;
  if (w->surface == NULL)
    w->surface = wl_compositor_create_surface(win_compositor);
  senkos_shell_v1_set_overlay(shell, w->surface, anchor, margin);
}

void layer_show_app(int pgid) {
  if (shell != NULL)
    senkos_shell_v1_show_app(shell, pgid);
}

void layer_refresh(int mhz) {
  if (shell != NULL)
    senkos_shell_v1_set_refresh(shell, mhz);
}
