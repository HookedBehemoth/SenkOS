/*
 * Launcher: a status bar over pages of menus, immediate mode. Every frame is
 * drawn from the pages and the status; nav keeps focus and scrolling.
 *
 * D-pad moves (left/right past the menu, or L1/R1, change page), A launches.
 * What it launched owns the screen (Weston kiosk shell) until it exits; the
 * gamepad is read straight from evdev, so input is ignored meanwhile.
 *
 * Apps page: built-ins, then every executable in /data/OS/apps.
 */
#include <dirent.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "nav.h"
#include "text.h"

#define APPS_DIR "/data/OS/apps"

enum { BAR = 32, BAR_TEXT = 18, ROW = 44, ROW_TEXT = 22, MARGIN = 12 };

static const SDL_Color BG = {0x14, 0x14, 0x18, 0xff};
static const SDL_Color FG = {0xee, 0xee, 0xee, 0xff};
static const SDL_Color DIM = {0x55, 0x55, 0x5c, 0xff};
static const SDL_Color FOCUS = {0x2a, 0x2e, 0x3a, 0xff};
static const SDL_Color LOW = {0xe0, 0x40, 0x40, 0xff};
static const SDL_Color CHARGING = {0x50, 0xc8, 0x60, 0xff};
static const ui_color RING = {0x6a, 0x9c, 0xff, 0xff};

typedef struct {
  char label[64];
  /* Absolute path: exec'd as is. Anything else: sh -c. */
  char command[256];
} entry;

static const entry builtin_apps[] = {
    {"Vulkan cube", "vkcube --wsi wayland"},
};
static const entry system_entries[] = {
    {"Sleep", "/usr/sbin/suspend"},
    {"Reboot", "reboot"},
    {"Power off", "poweroff"},
};
static entry apps[64];
static size_t app_count;
static const size_t system_count = SDL_arraysize(system_entries);

static const struct {
  const char *name;
  const entry *entries;
  const size_t *count;
} pages[] = {
    {"Apps", apps, &app_count},
    {"System", system_entries, &system_count},
};
#define PAGES SDL_arraysize(pages)

typedef struct {
  int battery; /* percent, -1 without one */
  bool charging;
  int wifi; /* bars 0-4, -1 without a wireless interface */
} status;

static SDL_Renderer *renderer;
static ui_context ctx;
static pid_t child;

static int by_label(const void *a, const void *b) {
  return strcmp(((const entry *)a)->label, ((const entry *)b)->label);
}

static void scan_apps(void) {
  app_count = 0;
  for (size_t i = 0; i < SDL_arraysize(builtin_apps); i++)
    apps[app_count++] = builtin_apps[i];
  DIR *dir = opendir(APPS_DIR);
  if (dir == NULL)
    return;
  const size_t first = app_count;
  struct dirent *d;
  while ((d = readdir(dir)) != NULL && app_count < SDL_arraysize(apps)) {
    entry *e = &apps[app_count];
    if (d->d_name[0] == '.' ||
        snprintf(e->command, sizeof(e->command), APPS_DIR "/%s", d->d_name) >=
            (int)sizeof(e->command) ||
        access(e->command, X_OK) != 0)
      continue;
    snprintf(e->label, sizeof(e->label), "%.63s", d->d_name);
    app_count++;
  }
  closedir(dir);
  qsort(apps + first, app_count - first, sizeof(*apps), by_label);
}

static void launch(const char *command) {
  const pid_t pid = fork();
  if (pid == 0) {
    setsid();
    if (command[0] == '/')
      execl(command, command, (char *)NULL);
    else
      execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }
  if (pid > 0)
    child = pid;
}

static int read_line(const char *path, char *out, size_t size) {
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return -1;
  const bool ok = fgets(out, (int)size, f) != NULL;
  fclose(f);
  return ok ? 0 : -1;
}

static void read_status(status *s) {
  char line[256];
  s->battery = -1;
  s->charging = false;
  glob_t g;
  if (glob("/sys/class/power_supply/*/capacity", 0, NULL, &g) == 0) {
    if (read_line(g.gl_pathv[0], line, sizeof(line)) == 0)
      s->battery = atoi(line);
    char path[512];
    snprintf(path, sizeof(path), "%.*s/status",
             (int)(strrchr(g.gl_pathv[0], '/') - g.gl_pathv[0]),
             g.gl_pathv[0]);
    s->charging = read_line(path, line, sizeof(line)) == 0 &&
                  strncmp(line, "Charging", 8) == 0;
    globfree(&g);
  }
  /* Two header lines, then "wlan0: 0000   54.  -56. ...": link quality of 70. */
  s->wifi = -1;
  FILE *f = fopen("/proc/net/wireless", "r");
  if (f != NULL) {
    float quality;
    if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f) &&
        fgets(line, sizeof(line), f) &&
        sscanf(line, "%*[^:]: %*x %f", &quality) == 1)
      s->wifi = SDL_clamp((int)(quality * 4 / 70 + 0.5f), 0, 4);
    fclose(f);
  }
}

static void fill(float x, float y, float w, float h, SDL_Color c) {
  SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
  SDL_RenderFillRectF(renderer, &(SDL_FRect){x, y, w, h});
}

/* "[time]      [page]         [wifi bars][battery%][battery symbol]" */
static void top_bar(int width, const char *page, const status *s) {
  char clock[8];
  const time_t now = time(NULL);
  strftime(clock, sizeof(clock), "%H:%M", localtime(&now));
  const int y = (BAR - text_height(BAR_TEXT)) / 2;
  text_draw(BAR_TEXT, MARGIN, y, clock, FG);
  text_draw(BAR_TEXT, (width - text_width(BAR_TEXT, page)) / 2, y, page, FG);

  float x = width - MARGIN;
  if (s->battery >= 0) {
    const SDL_Color level = s->charging         ? CHARGING
                            : s->battery <= 15 ? LOW
                                               : FG;
    fill(x -= 2, BAR / 2 - 3, 2, 6, FG);
    x -= 24;
    ui_stroke(&ctx, (ui_rect){x, BAR / 2 - 6, 24, 12}, NULL,
              (ui_color){FG.r, FG.g, FG.b, FG.a}, 1, 0);
    fill(x + 2, BAR / 2 - 4, 20 * SDL_clamp(s->battery, 0, 100) / 100.0f, 8,
         level);
    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%", s->battery);
    x -= 6 + text_width(BAR_TEXT, percent);
    text_draw(BAR_TEXT, (int)x, y, percent, FG);
  }
  x -= 10 + 4 * 5;
  for (int i = 0; i < 4; i++) {
    const float h = 4 + 3 * i;
    fill(x + i * 5, BAR / 2 + 7 - h, 3, h, i < s->wifi ? FG : DIM);
  }
  fill(0, BAR, width, 1, DIM);
}

/* Returns the entry activated this frame, if any. */
static const entry *menu(nav *ui, const entry *entries, size_t count,
                         ui_rect view) {
  if (count == 0) {
    text_draw(ROW_TEXT, view.x + MARGIN * 2, view.y + MARGIN, "Nothing here",
              DIM);
    return NULL;
  }
  const entry *chosen = NULL;
  const float offset =
      nav_scroll_begin(ui, nav_str("menu"), view, UI_VERTICAL,
                       count * ROW + MARGIN * 2, ROW, 0);
  SDL_RenderSetClipRect(renderer, &(SDL_Rect){(int)view.x, (int)view.y,
                                              (int)view.w, (int)view.h});
  for (size_t i = 0; i < count; i++) {
    const ui_rect r = {view.x + MARGIN, view.y + MARGIN + i * ROW - offset,
                       view.w - MARGIN * 2, ROW - 4};
    const unsigned flags = nav_item(ui, nav_int(i), r, view);
    nav_ring(ui, r, 0);
    if (flags & NAV_FOCUSED)
      fill(r.x, r.y, r.w, r.h, FOCUS);
    text_draw(ROW_TEXT, (int)r.x + MARGIN,
              (int)r.y + ((int)r.h - text_height(ROW_TEXT)) / 2,
              entries[i].label, FG);
    if (flags & NAV_ACTIVATED)
      chosen = &entries[i];
  }
  SDL_RenderSetClipRect(renderer, NULL);
  nav_scroll_end(ui);
  return chosen;
}

static void turn_page(size_t *page, int by) {
  *page = (*page + PAGES + by) % PAGES;
  if (*page == 0)
    scan_apps();
}

static bool input(nav *ui, const SDL_Event *ev, size_t *page) {
  int button = -1;
  switch (ev->type) {
  case SDL_QUIT:
    return false;
  case SDL_CONTROLLERDEVICEADDED:
    SDL_GameControllerOpen(ev->cdevice.which);
    break;
  case SDL_CONTROLLERBUTTONDOWN:
    button = ev->cbutton.button;
    break;
  case SDL_KEYDOWN: /* a keyboard, or the desktop */
    switch (ev->key.keysym.sym) {
    case SDLK_UP: button = SDL_CONTROLLER_BUTTON_DPAD_UP; break;
    case SDLK_DOWN: button = SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
    case SDLK_LEFT: button = SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
    case SDLK_RIGHT: button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
    case SDLK_RETURN: button = SDL_CONTROLLER_BUTTON_A; break;
    case SDLK_PAGEUP: button = SDL_CONTROLLER_BUTTON_LEFTSHOULDER; break;
    case SDLK_PAGEDOWN: button = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; break;
    }
    break;
  }
  /* Menu quits whatever runs: SDL apps turn SIGTERM into SDL_QUIT and exit
   * cleanly (mGBA flushes its save). setsid made the child a group leader. */
  if (child != 0) {
    if (button == SDL_CONTROLLER_BUTTON_GUIDE)
      kill(-child, SIGTERM);
    return true;
  }
  switch (button) {
  case SDL_CONTROLLER_BUTTON_DPAD_UP: nav_move(ui, NAV_UP); break;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: nav_move(ui, NAV_DOWN); break;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    if (!nav_move(ui, NAV_LEFT))
      turn_page(page, -1);
    break;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    if (!nav_move(ui, NAV_RIGHT))
      turn_page(page, 1);
    break;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: turn_page(page, -1); break;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: turn_page(page, 1); break;
  /* A by label: SDL_GAMECONTROLLERCONFIG (/etc/profile.d/wayland.sh) maps the pad */
  case SDL_CONTROLLER_BUTTON_A: nav_activate(ui); break;
  }
  return true;
}

int main(void) {
  /* Weston hands keyboard focus to whatever it launched, but the gamepad is
   * evdev: keep listening, and ignore it while a child runs. */
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
    SDL_Log("SDL_Init: %s", SDL_GetError());
    return 1;
  }
  SDL_Window *window =
      SDL_CreateWindow("Launcher", SDL_WINDOWPOS_UNDEFINED,
                       SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_RESIZABLE);
  renderer = window ? SDL_CreateRenderer(window, -1,
                                         SDL_RENDERER_ACCELERATED |
                                             SDL_RENDERER_PRESENTVSYNC)
                    : NULL;
  if (renderer == NULL || !text_init(renderer, "sans-serif")) {
    SDL_Log("launcher: %s", SDL_GetError());
    return 1;
  }
  ctx.renderer = renderer;
  static nav ui = {.animate = true, .duration = 150};
  size_t page = 0;
  scan_apps();
  status st;
  read_status(&st);
  Uint64 status_at = SDL_GetTicks64();
  bool animating = true;

  for (;;) {
    SDL_Event ev;
    /* Idle: wake once a second for the clock and the status. */
    if (SDL_WaitEventTimeout(&ev, animating ? 0 : 1000)) {
      do
        if (!input(&ui, &ev, &page))
          return 0;
      while (SDL_PollEvent(&ev));
      animating = true;
    }
    if (child != 0 && waitpid(child, NULL, WNOHANG) == child) {
      child = 0;
      scan_apps();
    }
    const Uint64 now = SDL_GetTicks64();
    if (now - status_at >= 1000) {
      read_status(&st);
      status_at = now;
    }
    if (child != 0) {
      animating = false;
      continue;
    }

    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    SDL_SetRenderDrawColor(renderer, BG.r, BG.g, BG.b, BG.a);
    SDL_RenderClear(renderer);
    nav_begin(&ui, &ctx, now);
    top_bar(w, pages[page].name, &st);
    const entry *chosen = NULL;
    NAV_SCOPE(&ui, nav_int(page)) {
      chosen = menu(&ui, pages[page].entries, *pages[page].count,
                    (ui_rect){0, BAR + 1, w, h - BAR - 1});
    }
    animating = nav_end(&ui, RING, 2);
    SDL_RenderPresent(renderer);
    if (chosen != NULL)
      launch(chosen->command);
  }
}
