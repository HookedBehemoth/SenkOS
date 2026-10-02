/*
 * mediaplayer: plays a video, with the pad. The launcher drives it through
 * appctl.h (pause, settings) like corehost; without the launcher it just plays.
 *
 *   mediaplayer FILE
 *
 * GStreamer's playbin3 decodes: on Cedrus through v4l2codecs, whose dmabufs
 * waylandsink hands to Weston as they are. If decoding fails (a codec profile
 * the engine lacks), the file is opened again with software decoders.
 *
 * Our toplevel is black; waylandsink draws into a subsurface of it, and the
 * OSD (title, time, progress) is a CPU-drawn subsurface above that.
 *
 * A, Start: pause    Left/Right: -/+10 s    L1/R1: -/+60 s
 * Up/Down: previous/next video in the folder    X, Y, Select: OSD    B: quit
 *
 * Where each video was left is kept in $HOME/.local/state/mediaplayer/resume
 * and playback starts there. At the end the next video in the folder plays.
 */
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#define GST_USE_UNSTABLE_API
#include <gst/wayland/wayland.h>
#include <wayland-client.h>

#include "appctl.h"
#include "cfg.h"
#include "pad.h"
#include "str.h"
#include "util.h"
#include "win.h"

#define OSD_H 72
#define OSD_SHOWN_MS 3000
#define SEEK_SHORT (10 * GST_SECOND)
#define SEEK_LONG (60 * GST_SECOND)
#define REPEAT_DELAY_MS 400
#define REPEAT_MS 150
/* As the launcher's Videos page has them (launcher/apps.c) */
static const str video_exts[] = {S(".mp4"), S(".mkv"), S(".webm"), S(".mov"), S(".m4v"), S(".avi"), S(".ts")};

static struct wl_surface *video_surface, *osd_surface;
static struct wl_subsurface *video_sub, *osd_sub;
static void configured(win *w);
static win window = {.app_id = "mediaplayer", .title = "Media player", .fullscreen = true,
                     .w = 640, .h = 480, .configure = configured};

static GstElement *pipe_, *vsink;
static STRBUF(1024) path;
static bool software, finished, quit;
static GstClockTime duration = GST_CLOCK_TIME_NONE;
/* Where a seek goes, until it is done: held buttons add up from here */
static GstClockTime seek_target = GST_CLOCK_TIME_NONE;
static GstClockTime start_at;
static bool user_paused, ctl_paused, prerolled;

static uint64_t osd_until;
static STRBUF(128) message;
static uint64_t message_until;

static int ctl = -1;
static appctl_shm *shm;
enum { OPT_SCALING, OPT_AUDIO };
static STRBUF(512) opt_path, state_path;

/* --- shm buffers ------------------------------------------------------- */

typedef struct {
  struct wl_buffer *buffer;
  uint32_t *pixels;
  int w, h;
  bool busy;
} shm_buffer;

static void buffer_release(void *data, struct wl_buffer *b) {
  (void)b;
  ((shm_buffer *)data)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = {buffer_release};

static bool shm_buffer_make(shm_buffer *sb, int w, int h, uint32_t format) {
  if (sb->buffer != NULL) {
    wl_buffer_destroy(sb->buffer);
    munmap(sb->pixels, (size_t)sb->w * sb->h * 4);
    sb->buffer = NULL;
  }
  const size_t size = (size_t)w * h * 4;
  const int fd = memfd_create("mediaplayer", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
    if (fd >= 0)
      close(fd);
    return false;
  }
  sb->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  struct wl_shm_pool *pool = wl_shm_create_pool(win_shm, fd, (int32_t)size);
  close(fd);
  sb->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, format);
  wl_shm_pool_destroy(pool);
  wl_buffer_add_listener(sb->buffer, &buffer_listener, sb);
  sb->w = w;
  sb->h = h;
  sb->busy = false;
  return sb->pixels != MAP_FAILED;
}

/* --- text ---------------------------------------------------------------- */

static FT_Library ft;
static FT_Face face;

static bool font_init(void) {
  if (!FcInit() || FT_Init_FreeType(&ft) != 0)
    return false;
  FcPattern *wanted = FcNameParse((const FcChar8 *)"sans-serif");
  FcConfigSubstitute(NULL, wanted, FcMatchPattern);
  FcDefaultSubstitute(wanted);
  FcResult result;
  FcPattern *match = FcFontMatch(NULL, wanted, &result);
  FcPatternDestroy(wanted);
  FcChar8 *file;
  int index = 0;
  if (match == NULL || FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch)
    return false;
  FcPatternGetInteger(match, FC_INDEX, 0, &index);
  const bool ok = FT_New_Face(ft, (const char *)file, index, &face) == 0;
  FcPatternDestroy(match);
  return ok;
}

/* The code point at the start of `*s`, which moves past it */
static uint32_t utf8_next(str *s) {
  const unsigned char *p = (const unsigned char *)s->p, *end = p + s->n;
  uint32_t c = *p++;
  int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
  c &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : extra == 1 ? 0x1f : 0x7f;
  while (extra-- > 0 && p < end && (*p & 0xc0) == 0x80)
    c = c << 6 | (*p++ & 0x3f);
  *s = str_at(*s, (size_t)((const char *)p - s->p));
  return c;
}

/* ponytail: glyph by glyph, no shaping; titles in complex scripts would want
 * HarfBuzz like the launcher's ui.c */
static int text_width(str s, int px) {
  FT_Set_Pixel_Sizes(face, 0, px);
  int w = 0;
  while (s.n > 0)
    if (FT_Load_Char(face, utf8_next(&s), FT_LOAD_DEFAULT) == 0)
      w += face->glyph->advance.x >> 6;
  return w;
}

/* Premultiplied ARGB; returns the pen's x after the text */
static int text_draw(shm_buffer *b, int x, int baseline, str s, int px, uint32_t rgb,
                     int max_x) {
  FT_Set_Pixel_Sizes(face, 0, px);
  while (s.n > 0) {
    if (FT_Load_Char(face, utf8_next(&s), FT_LOAD_RENDER) != 0)
      continue;
    const FT_GlyphSlot g = face->glyph;
    if (x + (int)(g->advance.x >> 6) > max_x)
      break;
    for (unsigned row = 0; row < g->bitmap.rows; row++) {
      const int y = baseline - g->bitmap_top + (int)row;
      if (y < 0 || y >= b->h)
        continue;
      for (unsigned col = 0; col < g->bitmap.width; col++) {
        const int px_x = x + g->bitmap_left + (int)col;
        const uint32_t a = g->bitmap.buffer[row * g->bitmap.pitch + col];
        if (px_x < 0 || px_x >= b->w || a == 0)
          continue;
        uint32_t *d = &b->pixels[y * b->w + px_x], out = 0;
        for (int shift = 0; shift < 32; shift += 8) {
          const uint32_t src = shift == 24 ? 255 : (rgb >> shift) & 0xff;
          const uint32_t dst = (*d >> shift) & 0xff;
          out |= ((src * a + dst * (255 - a)) / 255) << shift;
        }
        *d = out;
      }
    }
    x += g->advance.x >> 6;
  }
  return x;
}

/* Cut to fit with an ellipsis */
static void text_fit(shm_buffer *b, int x, int baseline, str s, int px, uint32_t rgb,
                     int max_x) {
  if (x + text_width(s, px) <= max_x) {
    text_draw(b, x, baseline, s, px, rgb, max_x);
    return;
  }
  const int dots = text_width(S("\u2026"), px);
  x = text_draw(b, x, baseline, s, px, rgb, max_x - dots);
  text_draw(b, x, baseline, S("\u2026"), px, rgb, max_x);
}

static void fill(shm_buffer *b, int x, int y, int w, int h, uint32_t argb) {
  for (int j = MAX(y, 0); j < MIN(y + h, b->h); j++)
    for (int i = MAX(x, 0); i < MIN(x + w, b->w); i++)
      b->pixels[j * b->w + i] = argb;
}

/* --- OSD ----------------------------------------------------------------- */

static shm_buffer osd_buffers[2];
static bool osd_mapped;

/* In `out`, which lives until the next call */
static str format_time(GstClockTime t) {
  static STRBUF(32) out;
  if (!GST_CLOCK_TIME_IS_VALID(t))
    return S("--:--");
  const unsigned s = (unsigned)(t / GST_SECOND);
  out.n = 0;
  if (s >= 3600)
    sb_fmt(out, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
  else
    sb_fmt(out, "%u:%02u", s / 60, s % 60);
  return sb_str(out);
}

static GstClockTime position(void) {
  gint64 pos;
  if (GST_CLOCK_TIME_IS_VALID(seek_target))
    return seek_target;
  if (pipe_ != NULL && gst_element_query_position(pipe_, GST_FORMAT_TIME, &pos))
    return (GstClockTime)pos;
  return GST_CLOCK_TIME_NONE;
}

static void show(str text) {
  sb_set(message, text);
  message_until = ms_now() + OSD_SHOWN_MS;
  osd_until = message_until;
}

static void osd_draw(void) {
  const uint64_t now = ms_now();
  const bool wanted = now < osd_until || user_paused || finished;
  if (!window.configured)
    return;
  if (!wanted) {
    if (osd_mapped) {
      wl_surface_attach(osd_surface, NULL, 0, 0);
      wl_surface_commit(osd_surface);
      osd_mapped = false;
    }
    return;
  }
  shm_buffer *b = !osd_buffers[0].busy ? &osd_buffers[0] : !osd_buffers[1].busy ? &osd_buffers[1] : NULL;
  if (b == NULL)
    return;
  if ((b->w != window.w || b->buffer == NULL) && !shm_buffer_make(b, window.w, OSD_H, WL_SHM_FORMAT_ARGB8888))
    return;
  const int m = 16, w = b->w;
  fill(b, 0, 0, w, OSD_H, 0xd0101014);
  int x = m;
  if (user_paused) {
    fill(b, x, 10, 4, 16, 0xffffffff);
    fill(b, x + 7, 10, 4, 16, 0xffffffff);
    x += 20;
  }
  const size_t slash = str_rfind(sb_str(path), '/');
  const str name = slash < path.n ? str_at(sb_str(path), slash + 1) : sb_str(path);
  const bool msg = now < message_until || finished;
  text_fit(b, x, 25, msg ? sb_str(message) : name, 18, msg ? 0xffd070 : 0xffffff, w - m);

  const GstClockTime pos = position();
  fill(b, m, 36, w - 2 * m, 6, 0xff3a3b44);
  if (GST_CLOCK_TIME_IS_VALID(pos) && GST_CLOCK_TIME_IS_VALID(duration) && duration > 0)
    fill(b, m, 36, (int)((w - 2 * m) * (double)MIN(pos, duration) / duration), 6, 0xff6a9cff);
  text_draw(b, m, 63, format_time(pos), 15, 0xc8c8d0, w);
  const str total = format_time(duration);
  text_draw(b, w - m - text_width(total, 15), 63, total, 15, 0xc8c8d0, w);

  wl_surface_attach(osd_surface, b->buffer, 0, 0);
  wl_surface_damage(osd_surface, 0, 0, w, OSD_H);
  wl_surface_commit(osd_surface);
  b->busy = true;
  osd_mapped = true;
}

static void osd_poke(void) { osd_until = ms_now() + OSD_SHOWN_MS; }

/* --- window -------------------------------------------------------------- */

static shm_buffer black, clear;

static void attach_background(void) {
  if (black.w != window.w || black.h != window.h)
    shm_buffer_make(&black, window.w, window.h, WL_SHM_FORMAT_XRGB8888);
  wl_subsurface_set_position(osd_sub, 0, window.h - OSD_H);
  wl_surface_attach(window.surface, black.buffer, 0, 0);
  wl_surface_damage(window.surface, 0, 0, window.w, window.h);
  wl_surface_commit(window.surface);
  if (vsink != NULL)
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(vsink), 0, 0, window.w, window.h);
}

static void configured(win *w) {
  (void)w;
  attach_background();
  osd_draw();
}

/* --- keyboard (testing on a desktop) ------------------------------------ */

static void press(int button);
static void release(int button);

/* APPCTL_*, -1 for none */
static int key_button(uint32_t key) {
  switch (key) {
  case KEY_SPACE: case KEY_ENTER: return APPCTL_A;
  case KEY_ESC: case KEY_BACKSPACE: return APPCTL_B;
  case KEY_TAB: return APPCTL_X;
  case KEY_LEFT: return APPCTL_LEFT;
  case KEY_RIGHT: return APPCTL_RIGHT;
  case KEY_UP: return APPCTL_UP;
  case KEY_DOWN: return APPCTL_DOWN;
  case KEY_PAGEUP: return APPCTL_L1;
  case KEY_PAGEDOWN: return APPCTL_R1;
  default: return -1;
  }
}

static void key(unsigned code, bool down) {
  const int b = key_button(code);
  if (b >= 0)
    (down ? press : release)(b);
}

static bool window_init(void) {
  if (!win_connect(key))
    return false;
  struct wl_surface *top = window.surface = wl_compositor_create_surface(win_compositor);
  /* Video first, OSD second: a new subsurface goes on top of its siblings,
   * and waylandsink's own surfaces stay inside the video's */
  video_surface = wl_compositor_create_surface(win_compositor);
  video_sub = wl_subcompositor_get_subsurface(win_subcompositor, video_surface, top);
  osd_surface = wl_compositor_create_surface(win_compositor);
  osd_sub = wl_subcompositor_get_subsurface(win_subcompositor, osd_surface, top);
  wl_subsurface_set_desync(video_sub);
  wl_subsurface_set_desync(osd_sub);
  /* An unmapped parent hides its children: the video's gets a clear pixel */
  shm_buffer_make(&clear, 1, 1, WL_SHM_FORMAT_ARGB8888);
  wl_surface_attach(video_surface, clear.buffer, 0, 0);
  wl_surface_commit(video_surface);
  /* Pass input to... nothing: the OSD and video never take it */
  struct wl_region *none = wl_compositor_create_region(win_compositor);
  wl_surface_set_input_region(osd_surface, none);
  wl_surface_set_input_region(video_surface, none);
  wl_region_destroy(none);
  win_map(&window);
  return true;
}

/* --- state --------------------------------------------------------------- */

static void mkdirs(char *p) {
  for (char *slash = strchr(p + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    mkdir(p, 0755);
    *slash = '/';
  }
}

/* One "SECONDS PATH" line per video, the latest last.
 * ponytail: rewritten whole each time; a few hundred lines is nothing */
#define RESUME_MAX 500
/* A line's seconds, and its file in `*file`; false for a broken line */
static bool resume_line(str line, long *seconds, str *file) {
  const str number = str_cut(&line, ' ');
  char *end;
  /* The text ends with a NUL (str_read): strtol stops in time */
  *seconds = number.n > 0 ? strtol(number.p, &end, 10) : 0;
  *file = line;
  return number.n > 0 && end == number.p + number.n;
}

static long resume_get(str file) {
  const str text = str_read(fopen(state_path.p, "r"));
  long found = 0, s;
  str name;
  for (str rest = text; rest.n > 0;)
    if (resume_line(str_cut(&rest, '\n'), &s, &name) && str_eq(name, file))
      found = s;
  free((char *)text.p);
  return found;
}

/* seconds < 0: forget it */
static void resume_set(str file, long seconds) {
  STRBUF(600) tmp;
  if (!sb_set(tmp, sb_str(state_path), S(".new")))
    return;
  FILE *out = fopen(tmp.p, "w");
  if (out == NULL)
    return;
  const str text = str_read(fopen(state_path.p, "r"));
  int total = 0;
  for (str rest = text; rest.n > 0; str_cut(&rest, '\n'))
    total++;
  int skip = total >= RESUME_MAX ? total - RESUME_MAX + 1 : 0;
  long s;
  str name;
  for (str rest = text; rest.n > 0;) {
    const str line = str_cut(&rest, '\n');
    if (skip > 0 && skip--)
      continue;
    if (resume_line(line, &s, &name) && !str_eq(name, file))
      str_fput(out, line, S("\n"));
  }
  if (seconds >= 0) {
    fprintf(out, "%ld ", seconds);
    str_fput(out, file, S("\n"));
  }
  free((char *)text.p);
  if (fclose(out) == 0)
    rename(tmp.p, state_path.p);
}

static void remember(void) {
  if (pipe_ == NULL)
    return;
  /* At the end there is no position: seen. The credits count as seen too. */
  const GstClockTime pos = position();
  if (finished || (GST_CLOCK_TIME_IS_VALID(pos) && GST_CLOCK_TIME_IS_VALID(duration) &&
                   pos > duration / 100 * 95))
    resume_set(sb_str(path), -1);
  else if (GST_CLOCK_TIME_IS_VALID(pos))
    resume_set(sb_str(path), (long)(pos / GST_SECOND));
}

/* --- options (appctl) ----------------------------------------------------- */

static void set_option(int i, uint8_t category, str key, str label, const str *values, int count) {
  appctl_option *o = &shm->options[i];
  o->category = category;
  str_copy(o->key, sizeof(o->key), key);
  str_copy(o->label, sizeof(o->label), label);
  o->kind = APPCTL_CHOICE;
  o->count = (uint8_t)MIN(count, APPCTL_VALUES);
  for (int v = 0; v < o->count; v++)
    str_copy(o->values[v], APPCTL_TEXT, values[v]);
}

/* A value we wrote: bounded, terminated */
static str value_of(const appctl_option *o, unsigned v) {
  return (str){o->values[v], strnlen(o->values[v], APPCTL_TEXT)};
}

/* Scaling is kept ("scaling=Fit"); the audio track is the file's */
static void load_options(void) {
  const str text = str_read(fopen(opt_path.p, "r"));
  appctl_option *o = &shm->options[OPT_SCALING];
  cfg_reader r = cfg_init(text);
  while (cfg_next(&r) != CFG_END)
    for (unsigned v = 0; str_eq(r.key, S("scaling")) && v < o->count; v++)
      if (str_eq(r.value, value_of(o, v)))
        atomic_store(&o->value, v);
  free((char *)text.p);
}

static void save_options(void) {
  FILE *f = fopen(opt_path.p, "w");
  if (f == NULL)
    return;
  const appctl_option *o = &shm->options[OPT_SCALING];
  str_fput(f, S("scaling="), value_of(o, atomic_load(&o->value) % o->count), S("\n"));
  fclose(f);
}

/* playbin3's streams: the file's tracks, and which audio one plays */
static GstStreamCollection *collection;
static int audio_playing = -1;

/* The n-th audio stream of the collection */
static GstStream *audio_stream(int n) {
  for (guint i = 0; collection != NULL && i < gst_stream_collection_get_size(collection); i++) {
    GstStream *st = gst_stream_collection_get_stream(collection, i);
    if ((gst_stream_get_stream_type(st) & GST_STREAM_TYPE_AUDIO) && n-- == 0)
      return st;
  }
  return NULL;
}

static void audio_options(void) {
  int n = 0;
  while (n < APPCTL_VALUES && audio_stream(n) != NULL)
    n++;
  atomic_store(&shm->option_count, OPT_AUDIO);
  if (n < 2)
    return;
  char names[APPCTL_VALUES][APPCTL_TEXT];
  str values[APPCTL_VALUES];
  for (int i = 0; i < n; i++) {
    GstTagList *tags = gst_stream_get_tags(audio_stream(i));
    gchar *lang = NULL, *title = NULL;
    if (tags != NULL) {
      if (!gst_tag_list_get_string(tags, GST_TAG_LANGUAGE_NAME, &lang))
        gst_tag_list_get_string(tags, GST_TAG_LANGUAGE_CODE, &lang);
      gst_tag_list_get_string(tags, GST_TAG_TITLE, &title);
      gst_tag_list_unref(tags);
    }
    const int n = title != NULL && lang != NULL
        ? snprintf(names[i], APPCTL_TEXT, "%d: %s (%s)", i + 1, title, lang)
        : snprintf(names[i], APPCTL_TEXT, "%d%s%s", i + 1, title || lang ? ": " : "",
                   title ? title : lang ? lang : "");
    values[i] = (str){names[i], (size_t)CLAMP(n, 0, APPCTL_TEXT - 1)};
    g_free(lang);
    g_free(title);
  }
  set_option(OPT_AUDIO, APPCTL_AUDIO, S("audio"), S("Audio track"), values, n);
  atomic_store(&shm->options[OPT_AUDIO].value, (uint8_t)MAX(audio_playing, 0));
  atomic_store(&shm->option_count, OPT_AUDIO + 1);
}

/* What playbin3 picked, so the menu shows it */
static void streams_selected(GstMessage *m) {
  for (guint i = 0; i < gst_message_streams_selected_get_size(m); i++) {
    GstStream *st = gst_message_streams_selected_get_stream(m, i);
    for (int n = 0; (gst_stream_get_stream_type(st) & GST_STREAM_TYPE_AUDIO) && audio_stream(n); n++)
      if (audio_stream(n) == st)
        audio_playing = n;
    gst_object_unref(st);
  }
  if (atomic_load(&shm->option_count) > OPT_AUDIO)
    atomic_store(&shm->options[OPT_AUDIO].value, (uint8_t)MAX(audio_playing, 0));
}

/* The first video stream and the chosen audio one */
static void select_audio(int n) {
  GList *ids = NULL;
  for (guint i = 0; i < gst_stream_collection_get_size(collection); i++) {
    GstStream *st = gst_stream_collection_get_stream(collection, i);
    if (gst_stream_get_stream_type(st) & GST_STREAM_TYPE_VIDEO) {
      ids = g_list_append(ids, (gpointer)gst_stream_get_stream_id(st));
      break;
    }
  }
  ids = g_list_append(ids, (gpointer)gst_stream_get_stream_id(audio_stream(n)));
  gst_element_send_event(pipe_, gst_event_new_select_streams(ids));
  g_list_free(ids);
  audio_playing = n;
}

static void apply_options(void) {
  if (vsink != NULL)
    g_object_set(vsink, "force-aspect-ratio", atomic_load(&shm->options[OPT_SCALING].value) == 0, NULL);
  const int n = atomic_load(&shm->options[OPT_AUDIO].value);
  if (pipe_ != NULL && atomic_load(&shm->option_count) > OPT_AUDIO && n != audio_playing &&
      audio_stream(n) != NULL)
    select_audio(n);
}

static bool open_ctl(void) {
  int sock, mem;
  const char *env = getenv(APPCTL_ENV);
  if (env != NULL && sscanf(env, "%d,%d", &sock, &mem) == 2) {
    void *map = mmap(NULL, sizeof(appctl_shm), PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0);
    close(mem);
    if (map != MAP_FAILED) {
      ctl = sock;
      shm = map;
    }
  }
  unsetenv(APPCTL_ENV);
  if (shm == NULL && (shm = calloc(1, sizeof(*shm))) == NULL)
    return false;
  shm->magic = APPCTL_MAGIC;
  shm->version = APPCTL_VERSION;
  shm->slots = 0;
  static const str scaling[] = {S("Fit"), S("Stretch")};
  set_option(OPT_SCALING, APPCTL_GRAPHICS, S("scaling"), S("Scaling"), scaling, 2);
  load_options();
  atomic_store(&shm->option_count, OPT_SCALING + 1);
  return true;
}

/* --- playback ------------------------------------------------------------ */

static GstBusSyncReply bus_sync(GstBus *bus, GstMessage *m, gpointer data) {
  (void)bus, (void)data;
  if (gst_is_wl_display_handle_need_context_message(m)) {
    GstContext *c = gst_wl_display_handle_context_new(win_display);
    gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(m)), c);
    gst_context_unref(c);
    gst_message_unref(m);
    return GST_BUS_DROP;
  }
  if (gst_is_video_overlay_prepare_window_handle_message(m)) {
    GstVideoOverlay *o = GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(m));
    gst_video_overlay_set_window_handle(o, (guintptr)video_surface);
    gst_video_overlay_set_render_rectangle(o, 0, 0, window.w, window.h);
    gst_message_unref(m);
    return GST_BUS_DROP;
  }
  return GST_BUS_PASS;
}

static void close_file(void) {
  if (pipe_ == NULL)
    return;
  gst_element_set_state(pipe_, GST_STATE_NULL);
  gst_object_unref(pipe_);
  pipe_ = vsink = NULL;
  if (collection != NULL)
    gst_object_unref(collection);
  collection = NULL;
  audio_playing = -1;
  atomic_store(&shm->option_count, OPT_AUDIO);
}

/* Cedrus's decoders out of the running, for the files it can't do */
static void use_software(void) {
  GList *list = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
  for (GList *l = list; l != NULL; l = l->next)
    if (g_str_has_prefix(gst_plugin_feature_get_name(l->data), "v4l2sl"))
      gst_plugin_feature_set_rank(l->data, GST_RANK_NONE);
  gst_plugin_feature_list_free(list);
  software = true;
}

static bool open_file(GstClockTime at) {
  close_file();
  pipe_ = gst_element_factory_make("playbin3", NULL);
  vsink = gst_element_factory_make("waylandsink", NULL);
  GstElement *asink = gst_element_factory_make("alsasink", NULL);
  gchar *uri = gst_filename_to_uri(path.p, NULL);
  if (pipe_ == NULL || vsink == NULL || uri == NULL) {
    show(S("GStreamer is missing playbin3 or waylandsink"));
    g_free(uri);
    return false;
  }
  /* playbin3: playbin's decodebin answers the decoder's allocation query
   * before the sink is linked, without VideoMeta, and v4l2codecs then refuses
   * dmabufs. Video and audio only; playsink's converter passes dmabufs
   * through and turns software's 10-bit into something waylandsink takes. */
  g_object_set(pipe_, "uri", uri, "flags", 0x1 | 0x2, "video-sink", vsink, NULL);
  if (asink != NULL)
    g_object_set(pipe_, "audio-sink", asink, NULL);
  g_free(uri);
  GstBus *bus = gst_element_get_bus(pipe_);
  gst_bus_set_sync_handler(bus, bus_sync, NULL, NULL);
  gst_object_unref(bus);
  apply_options();
  duration = GST_CLOCK_TIME_NONE;
  seek_target = GST_CLOCK_TIME_NONE;
  start_at = at;
  prerolled = finished = false;
  gst_element_set_state(pipe_, GST_STATE_PAUSED);
  return true;
}

static void play_state(void) {
  if (pipe_ != NULL && prerolled)
    gst_element_set_state(pipe_, user_paused || ctl_paused ? GST_STATE_PAUSED : GST_STATE_PLAYING);
}

static bool is_video(str name) {
  const size_t dot = str_rfind(name, '.');
  for (size_t i = 0; dot < name.n && i < COUNTOF(video_exts); i++)
    if (str_ieq(str_at(name, dot), video_exts[i]))
      return true;
  return false;
}

/* The video before or after this one in its folder, by name like the
 * launcher sorts them; false at either end */
static bool neighbour(int step) {
  const size_t slash = str_rfind(sb_str(path), '/');
  STRBUF(sizeof(path.p)) dir;
  if (slash < path.n)
    sb_set(dir, str_head(sb_str(path), slash), S("/"));
  else
    dir.n = 0;
  /* Terminated: the tail of path */
  const char *self = path.p + dir.n;
  DIR *d = opendir(dir.n > 0 ? dir.p : ".");
  if (d == NULL)
    return false;
  STRBUF(256) best = {0};
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const str name = str_c(e->d_name);
    if (name.p[0] == '.' || !is_video(name))
      continue;
    const int c = strcasecmp(name.p, self);
    if ((step > 0 && c > 0 && (best.n == 0 || strcasecmp(name.p, best.p) < 0)) ||
        (step < 0 && c < 0 && (best.n == 0 || strcasecmp(name.p, best.p) > 0)))
      sb_set(best, name);
  }
  closedir(d);
  return best.n > 0 && sb_add(dir, sb_str(best)) && sb_set(path, sb_str(dir));
}

static void switch_file(int step) {
  remember();
  if (!neighbour(step)) {
    show(step > 0 ? S("Last video in this folder") : S("First video in this folder"));
    return;
  }
  open_file((GstClockTime)resume_get(sb_str(path)) * GST_SECOND);
  osd_poke();
}

static void seek_by(GstClockTimeDiff delta) {
  if (pipe_ == NULL || !prerolled)
    return;
  const GstClockTime pos = position();
  if (!GST_CLOCK_TIME_IS_VALID(pos))
    return;
  GstClockTimeDiff t = (GstClockTimeDiff)pos + delta;
  if (GST_CLOCK_TIME_IS_VALID(duration))
    t = MIN(t, (GstClockTimeDiff)duration - GST_SECOND);
  t = MAX(t, 0);
  /* The keyframe on the far side, so a long GOP still moves */
  const GstSeekFlags snap = delta > 0 ? GST_SEEK_FLAG_SNAP_AFTER : GST_SEEK_FLAG_SNAP_BEFORE;
  if (gst_element_seek_simple(pipe_, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT | snap, t))
    seek_target = (GstClockTime)t;
  finished = false;
  osd_poke();
}

static void bus_messages(void) {
  if (pipe_ == NULL)
    return;
  GstBus *bus = gst_element_get_bus(pipe_);
  GstMessage *m;
  while (pipe_ != NULL && (m = gst_bus_pop(bus)) != NULL) {
    switch (GST_MESSAGE_TYPE(m)) {
    case GST_MESSAGE_ASYNC_DONE:
      seek_target = GST_CLOCK_TIME_NONE;
      if (!prerolled) {
        prerolled = true;
        gint64 d;
        if (gst_element_query_duration(pipe_, GST_FORMAT_TIME, &d))
          duration = (GstClockTime)d;
        apply_options();
        if (start_at > 0 && (!GST_CLOCK_TIME_IS_VALID(duration) || start_at < duration)) {
          STRBUF(64) line;
          sb_set(line, S("Resumed at "), format_time(start_at));
          show(sb_str(line));
          /* Exactly there: a keyframe can be 10 s before */
          gst_element_seek_simple(pipe_, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, start_at);
        }
        play_state();
      }
      break;
    case GST_MESSAGE_STREAM_COLLECTION:
      if (collection != NULL)
        gst_object_unref(collection);
      gst_message_parse_stream_collection(m, &collection);
      audio_options();
      break;
    case GST_MESSAGE_STREAMS_SELECTED:
      streams_selected(m);
      break;
    case GST_MESSAGE_DURATION_CHANGED: {
      gint64 d;
      if (gst_element_query_duration(pipe_, GST_FORMAT_TIME, &d))
        duration = (GstClockTime)d;
      break;
    }
    case GST_MESSAGE_EOS:
      finished = true;
      remember();
      if (neighbour(1)) {
        open_file((GstClockTime)resume_get(sb_str(path)) * GST_SECOND);
        osd_poke();
      } else {
        show(S("The end"));
        quit = ctl >= 0;
      }
      break;
    case GST_MESSAGE_ERROR: {
      GError *err = NULL;
      gchar *debug = NULL;
      gst_message_parse_error(m, &err, &debug);
      fprintf(stderr, "mediaplayer: %s: %s (%s)\n", GST_OBJECT_NAME(GST_MESSAGE_SRC(m)), err->message,
              debug ? debug : "");
      const GstClockTime pos = prerolled ? position() : start_at;
      if (!software) {
        use_software();
        open_file(GST_CLOCK_TIME_IS_VALID(pos) ? pos : 0);
        show(S("Software decoding"));
      } else {
        close_file();
        show(str_c(err->message));
        finished = true;
      }
      g_error_free(err);
      g_free(debug);
      break;
    }
    default:
      break;
    }
    gst_message_unref(m);
  }
  gst_object_unref(bus);
}

/* --- input --------------------------------------------------------------- */

/* The built-in pad (pad.h); the seek button held, -1 for none */
static int pad_fd = -1;
static int held = -1;
static uint64_t repeat_at;
static bool menu_down;

static GstClockTimeDiff seek_step(int b) {
  switch (b) {
  case APPCTL_LEFT: return -SEEK_SHORT;
  case APPCTL_RIGHT: return SEEK_SHORT;
  case APPCTL_L1: return -SEEK_LONG;
  case APPCTL_R1: return SEEK_LONG;
  default: return 0;
  }
}

static void press(int b) {
  /* Menu + anything is the launcher's */
  if (b == APPCTL_MENU)
    menu_down = true;
  if (menu_down || ctl_paused)
    return;
  if (seek_step(b) != 0) {
    seek_by(seek_step(b));
    held = b;
    repeat_at = ms_now() + REPEAT_DELAY_MS;
    return;
  }
  switch (b) {
  case APPCTL_A:
  case APPCTL_START:
    if (finished)
      break;
    user_paused = !user_paused;
    play_state();
    if (user_paused)
      remember();
    osd_poke();
    break;
  case APPCTL_B: quit = true; break;
  case APPCTL_UP: switch_file(-1); break;
  case APPCTL_DOWN: switch_file(1); break;
  case APPCTL_X:
  case APPCTL_Y:
  case APPCTL_SELECT:
    osd_until = ms_now() < osd_until ? 0 : ms_now() + OSD_SHOWN_MS;
    break;
  default: break;
  }
}

static void release(int b) {
  if (b == APPCTL_MENU)
    menu_down = false;
  if (b == held)
    held = -1;
}

/* What the pad has queued; `drop`: not ours (the launcher had the screen) */
static void read_pad(bool drop) {
  struct input_event e;
  while (pad_fd >= 0 && read(pad_fd, &e, sizeof(e)) == sizeof(e)) {
    const int b = e.type == EV_KEY && e.value != 2 ? pad_button(e.code) : -1;
    if (b >= 0 && !drop)
      (e.value ? press : release)(b);
  }
}

static void input(void) {
  read_pad(false);
  if (held >= 0 && ms_now() >= repeat_at) {
    seek_by(seek_step(held));
    repeat_at = ms_now() + REPEAT_MS;
  }
}

/* --- launcher ------------------------------------------------------------ */

/* One request, answered; false once the launcher is gone */
static bool control(void) {
  appctl_msg m;
  const ssize_t n = recv(ctl, &m, sizeof(m), MSG_DONTWAIT);
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
    return false;
  if (n != sizeof(m))
    return true;
  m.result = 0;
  switch (m.op) {
  case APPCTL_PAUSE:
    ctl_paused = true;
    held = -1;
    menu_down = false;
    play_state();
    remember();
    break;
  case APPCTL_RESUME:
    ctl_paused = false;
    if (m.arg)
      win_map(&window);
    /* Presses that were the launcher's */
    read_pad(true);
    play_state();
    break;
  case APPCTL_OPTIONS:
    apply_options();
    save_options();
    break;
  default: m.result = -ENOSYS;
  }
  send(ctl, &m, sizeof(m), MSG_NOSIGNAL);
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s FILE\n", argv[0]);
    return 2;
  }
  char cwd[sizeof(path.p)];
  if (argv[1][0] == '/' || getcwd(cwd, sizeof(cwd)) == NULL)
    sb_set(path, str_c(argv[1]));
  else
    sb_set(path, str_c(cwd), S("/"), str_c(argv[1]));

  const char *config = getenv("XDG_CONFIG_HOME");
  const str home = getenv("HOME") ? str_c(getenv("HOME")) : S("/tmp");
  if (config != NULL && config[0] == '/')
    sb_set(opt_path, str_c(config), S("/mediaplayer.opt"));
  else
    sb_set(opt_path, home, S("/.config/mediaplayer.opt"));
  sb_set(state_path, home, S("/.local/state/mediaplayer/resume"));
  mkdirs(opt_path.p);
  mkdirs(state_path.p);

  gst_init(&argc, &argv);
  if (!open_ctl() || !font_init() || !window_init()) {
    fprintf(stderr, "mediaplayer: can't start\n");
    return 1;
  }
  pad_fd = pad_open(O_NONBLOCK);
  open_file((GstClockTime)resume_get(sb_str(path)) * GST_SECOND);
  osd_poke();

  uint64_t saved = ms_now();
  while (!quit) {
    struct pollfd fds[2] = {{pad_fd, POLLIN, 0}, {ctl, POLLIN, 0}};
    /* ponytail: the GStreamer bus and the OSD's clock are polled at 50 Hz;
     * gst_bus_get_pollfd and a timer when idle wakeups matter */
    if (!win_wait(fds, 2, 20))
      break;
    for (int why, value; win_woken(&why, &value);)
      quit |= why == WIN_QUIT;
    if (ctl >= 0 && fds[1].revents && !control())
      break;
    input();
    bus_messages();
    osd_draw();
    /* A crash or a dead battery costs at most this much */
    if (ms_now() - saved > 30000 && !user_paused && !ctl_paused) {
      remember();
      saved = ms_now();
    }
  }
  remember();
  close_file();
  return 0;
}
