/*
 * What SDL makes of the inputs, as a game sees them: every joystick SDL finds
 * (name, GUID, whether it has a game controller mapping and rumble), its raw
 * buttons, axes and hats, the controller's named buttons and axes, and the last
 * events, keys (volume) included.
 *
 * Select + Start rumbles (both motors) while held, Select + Up / Down sets the
 * strength. Menu is shown like any button: Menu + Start opens the launcher's
 * menu (APPCTL_MENU_START), where Quit ends padtest.
 *
 * L2 and R2 are buttons on every board (GPIOs): the mapping makes them SDL's
 * triggers, so they read 0 or full.
 *
 * SDL on purpose, unlike our other apps: this is a test of SDL's view.
 */
#include <SDL.h>
#include <SDL_ttf.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include "appctl.h"

#define FONT "/usr/share/fonts/ttf/LiberationMono-Regular.ttf"
#define FONT_PX 14
#define LINE 17
#define LOG_LINES 6
#define MAX_PADS 4

static SDL_Renderer *ren;
static TTF_Font *font;
static char log_lines[LOG_LINES][96];
static int log_next;
static int ctl = -1;
static int strength = 100; /* percent */
static bool paused;

static const SDL_Color FG = {230, 230, 230, 255}, DIM = {110, 110, 110, 255}, ON = {90, 200, 120, 255};

static void text(int x, int y, SDL_Color c, const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (buf[0] == '\0')
    return;
  SDL_Surface *s = TTF_RenderUTF8_Blended(font, buf, c);
  if (s == NULL)
    return;
  SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
  SDL_Rect r = {x, y, s->w, s->h};
  SDL_RenderCopy(ren, t, NULL, &r);
  SDL_DestroyTexture(t);
  SDL_FreeSurface(s);
}

static void logf_(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(log_lines[log_next], sizeof(log_lines[0]), fmt, ap);
  va_end(ap);
  log_next = (log_next + 1) % LOG_LINES;
}

/* A bar for an axis, -32768..32767, centre marked */
static void bar(int x, int y, int w, int v) {
  SDL_Rect r = {x, y + 3, w, LINE - 6};
  SDL_SetRenderDrawColor(ren, 60, 60, 60, 255);
  SDL_RenderFillRect(ren, &r);
  int c = x + w / 2, p = x + (int)((v + 32768L) * w / 65536);
  SDL_Rect f = {p < c ? p : c, y + 3, p < c ? c - p : p - c, LINE - 6};
  SDL_SetRenderDrawColor(ren, 90, 200, 120, 255);
  SDL_RenderFillRect(ren, &f);
}

/* The launcher's channel: Menu is ours, nothing to save or set */
static void open_ctl(void) {
  int sock, mem;
  const char *env = getenv(APPCTL_ENV);
  if (env == NULL || sscanf(env, "%d,%d", &sock, &mem) != 2)
    return;
  appctl_shm *shm = mmap(NULL, sizeof(appctl_shm), PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0);
  if (shm == MAP_FAILED)
    return;
  shm->magic = APPCTL_MAGIC;
  shm->version = APPCTL_VERSION;
  shm->flags = APPCTL_MENU_START;
  atomic_store(&shm->option_count, 0);
  ctl = sock;
}

static void control(void) {
  appctl_msg m;
  while (ctl >= 0 && recv(ctl, &m, sizeof(m), MSG_DONTWAIT) == sizeof(m)) {
    m.result = 0;
    if (m.op == APPCTL_PAUSE)
      paused = true;
    else if (m.op == APPCTL_RESUME)
      paused = false;
    else
      m.result = -ENOTSUP;
    send(ctl, &m, sizeof(m), MSG_NOSIGNAL);
  }
}

int main(void) {
  open_ctl();
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0 || TTF_Init() < 0) {
    fprintf(stderr, "padtest: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *win = SDL_CreateWindow("padtest", 0, 0, 640, 480, SDL_WINDOW_FULLSCREEN_DESKTOP);
  ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
  if (ren)
    SDL_RenderSetLogicalSize(ren, 640, 480);
  font = TTF_OpenFont(FONT, FONT_PX);
  if (ren == NULL || font == NULL) {
    fprintf(stderr, "padtest: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Joystick *joy[MAX_PADS] = {0};
  SDL_GameController *gc[MAX_PADS] = {0};
  bool quit = false;
  while (!quit) {
    control();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
      case SDL_QUIT:
        quit = true;
        break;
      case SDL_JOYDEVICEADDED:
        if (e.jdevice.which < MAX_PADS && joy[e.jdevice.which] == NULL) {
          int i = e.jdevice.which;
          if (SDL_IsGameController(i))
            gc[i] = SDL_GameControllerOpen(i);
          joy[i] = gc[i] ? SDL_GameControllerGetJoystick(gc[i]) : SDL_JoystickOpen(i);
          logf_("added %d: %s", i, SDL_JoystickNameForIndex(i));
        }
        break;
      case SDL_JOYDEVICEREMOVED:
        for (int i = 0; i < MAX_PADS; i++)
          if (joy[i] && SDL_JoystickInstanceID(joy[i]) == e.jdevice.which) {
            gc[i] ? SDL_GameControllerClose(gc[i]) : SDL_JoystickClose(joy[i]);
            joy[i] = NULL, gc[i] = NULL;
            logf_("removed %d", i);
          }
        break;
      case SDL_KEYDOWN:
      case SDL_KEYUP:
        if (!e.key.repeat)
          logf_("key %s %s", SDL_GetKeyName(e.key.keysym.sym), e.type == SDL_KEYDOWN ? "down" : "up");
        break;
      case SDL_CONTROLLERBUTTONDOWN:
        for (int i = 0; i < MAX_PADS; i++)
          if (gc[i] && SDL_JoystickInstanceID(joy[i]) == e.cbutton.which &&
              SDL_GameControllerGetButton(gc[i], SDL_CONTROLLER_BUTTON_BACK)) {
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP && strength < 100)
              strength += 10;
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN && strength > 10)
              strength -= 10;
          }
        /* fall through */
      case SDL_CONTROLLERBUTTONUP:
        logf_("pad %d %s %s", e.cbutton.which, SDL_GameControllerGetStringForButton(e.cbutton.button),
              e.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up");
        break;
      case SDL_JOYBUTTONDOWN:
        logf_("joy %d button %d", e.jbutton.which, e.jbutton.button);
        break;
      case SDL_JOYHATMOTION:
        logf_("joy %d hat %d = %d", e.jhat.which, e.jhat.hat, e.jhat.value);
        break;
      }
    }

    SDL_SetRenderDrawColor(ren, 20, 20, 24, 255);
    SDL_RenderClear(ren);
    int y = 8;
    text(8, y, DIM, "Menu+Start: menu");
    y += LINE + 4;
    for (int i = 0; i < MAX_PADS; i++) {
      if (joy[i] == NULL)
        continue;
      char guid[33];
      SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy[i]), guid, sizeof(guid));
      text(8, y, FG, "%d: %s  %s", i, SDL_JoystickName(joy[i]), guid);
      y += LINE;
      text(8, y, FG, "controller: %s  rumble: %s  buttons %d axes %d hats %d", gc[i] ? "mapped" : "no mapping",
           SDL_JoystickHasRumble(joy[i]) ? "yes" : "no", SDL_JoystickNumButtons(joy[i]),
           SDL_JoystickNumAxes(joy[i]), SDL_JoystickNumHats(joy[i]));
      y += LINE;

      /* Raw buttons, by number */
      int x = 8;
      for (int b = 0; b < SDL_JoystickNumButtons(joy[i]); b++, x += 26)
        text(x, y, SDL_JoystickGetButton(joy[i], b) ? ON : DIM, "%d", b);
      y += LINE;
      for (int h = 0; h < SDL_JoystickNumHats(joy[i]); h++, y += LINE)
        text(8, y, FG, "hat %d: %d", h, SDL_JoystickGetHat(joy[i], h));
      for (int a = 0; a < SDL_JoystickNumAxes(joy[i]); a++) {
        int v = SDL_JoystickGetAxis(joy[i], a);
        text(8 + (a % 2) * 312, y, FG, "axis %d %6d", a, v);
        bar(140 + (a % 2) * 312, y, 160, v);
        if (a % 2 || a + 1 == SDL_JoystickNumAxes(joy[i]))
          y += LINE;
      }

      if (gc[i]) {
        /* The mapping's names, as a game reads them */
        x = 8;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
          if (!SDL_GameControllerHasButton(gc[i], b))
            continue;
          const char *n = SDL_GameControllerGetStringForButton(b);
          int w = (int)SDL_strlen(n) * 8 + 10;
          if (x + w > 632)
            x = 8, y += LINE;
          text(x, y, SDL_GameControllerGetButton(gc[i], b) ? ON : DIM, "%s", n);
          x += w;
        }
        y += LINE;
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
          if (!SDL_GameControllerHasAxis(gc[i], a))
            continue;
          int v = SDL_GameControllerGetAxis(gc[i], a);
          text(8, y, FG, "%-12s %6d", SDL_GameControllerGetStringForAxis(a), v);
          bar(160, y, 200, v);
          y += LINE;
        }

        const bool on = !paused && SDL_GameControllerGetButton(gc[i], SDL_CONTROLLER_BUTTON_BACK) &&
                        SDL_GameControllerGetButton(gc[i], SDL_CONTROLLER_BUTTON_START);
        const Uint16 level = (Uint16)(strength * 65535 / 100);
        /* Renewed every frame while held, so a short duration ends it on release */
        if (on)
          SDL_GameControllerRumble(gc[i], level, level, 100);
        text(8, y, on ? ON : DIM, "rumble %s (Select+Start), strength %d%% (Select+Up/Down)", on ? "on" : "off",
             strength);
        y += LINE;
      }
      y += 6;
    }

    y = 480 - LOG_LINES * LINE - 6;
    for (int l = 0; l < LOG_LINES; l++, y += LINE)
      text(8, y, DIM, "%s", log_lines[(log_next + l) % LOG_LINES]);
    SDL_RenderPresent(ren);
  }

  TTF_CloseFont(font);
  TTF_Quit();
  SDL_Quit();
  return 0;
}
