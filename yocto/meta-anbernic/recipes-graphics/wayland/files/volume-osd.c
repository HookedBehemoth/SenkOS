/* Weston module: volume keys set the codec's Line Out, and a bar of solid
 * rectangles shows the level on top of every client for a moment.
 * Loaded with `weston --modules=volume-osd.so`. */
#include <alsa/asoundlib.h>
#include <libweston/libweston.h>
#include <libweston/shell-utils.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

#define STEPS 16
#define SEG_W 12
#define SEG_H 20
#define GAP 3
#define PAD 12
#define ICON 30 /* speaker + space */
#define SHOW_MS 1500

static struct {
	struct weston_compositor *compositor;
	struct weston_layer layer;
	struct wl_event_source *timer;
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem;
	long min, max;
	/* back, speaker body, speaker cone, then per step: full, empty */
	struct weston_curtain *back, *body, *cone, *full[STEPS], *empty[STEPS];
} osd;

static int label(struct weston_surface *s, char *buf, size_t len) {
	(void)s;
	return snprintf(buf, len, "volume osd");
}

static struct weston_curtain *rect(int x, int y, int w, int h, float v, float a) {
	struct weston_curtain_params p = {
		.get_label = label, .r = v, .g = v, .b = v, .a = a,
		.pos = {.c = weston_coord(x, y)}, .width = w, .height = h,
	};
	return weston_shell_utils_curtain_create(osd.compositor, &p);
}

/* Built on first use: the output (and so the position) exists by then. */
static bool build(void) {
	struct weston_output *out = weston_shell_utils_get_default_output(osd.compositor);
	if (!out)
		return false;
	int w = PAD + ICON + STEPS * (SEG_W + GAP) - GAP + PAD, h = PAD + SEG_H + PAD;
	int x = out->pos.c.x + (out->width - w) / 2, y = out->pos.c.y + 40;
	osd.back = rect(x, y, w, h, 0.1, 0.85);
	x += PAD;
	y += PAD;
	osd.body = rect(x, y + 6, 7, 8, 0.95, 1);
	osd.cone = rect(x + 7, y + 1, 6, 18, 0.95, 1);
	x += ICON;
	for (int i = 0; i < STEPS; i++, x += SEG_W + GAP) {
		osd.full[i] = rect(x, y, SEG_W, SEG_H, 0.95, 1);
		osd.empty[i] = rect(x, y, SEG_W, SEG_H, 0.3, 1);
	}
	return true;
}

static void show(int level) {
	if (!osd.back && !build())
		return;
	/* move_to_layer puts a view on top: background first */
	weston_view_move_to_layer(osd.back->view, &osd.layer.view_list);
	weston_view_move_to_layer(osd.body->view, &osd.layer.view_list);
	weston_view_move_to_layer(osd.cone->view, &osd.layer.view_list);
	for (int i = 0; i < STEPS; i++) {
		weston_view_move_to_layer((i < level ? osd.empty : osd.full)[i]->view, NULL);
		weston_view_move_to_layer((i < level ? osd.full : osd.empty)[i]->view, &osd.layer.view_list);
	}
	weston_compositor_schedule_repaint(osd.compositor);
	wl_event_source_timer_update(osd.timer, SHOW_MS);
}

static int hide(void *data) {
	(void)data;
	struct weston_view *v, *next;
	wl_list_for_each_safe(v, next, &osd.layer.view_list.link, layer_link.link)
		weston_view_move_to_layer(v, NULL);
	weston_compositor_schedule_repaint(osd.compositor);
	return 0;
}

static void key(struct weston_keyboard *kb, const struct timespec *t, uint32_t k, void *data) {
	(void)kb; (void)t; (void)data;
	long raw;
	snd_mixer_handle_events(osd.mixer); /* amixer may have changed it */
	snd_mixer_selem_get_playback_volume(osd.elem, SND_MIXER_SCHN_FRONT_LEFT, &raw);
	long range = osd.max - osd.min;
	int level = ((raw - osd.min) * STEPS + range / 2) / range;
	level += k == KEY_VOLUMEUP ? 1 : -1;
	level = level < 0 ? 0 : level > STEPS ? STEPS : level;
	snd_mixer_selem_set_playback_volume_all(osd.elem, osd.min + (level * range + STEPS / 2) / STEPS);
	snd_mixer_selem_set_playback_switch_all(osd.elem, level > 0);
	show(level);
}

static bool open_mixer(void) {
	snd_mixer_selem_id_t *id;
	snd_mixer_selem_id_alloca(&id);
	snd_mixer_selem_id_set_name(id, "Line Out");
	if (snd_mixer_open(&osd.mixer, 0) < 0)
		return false;
	if (snd_mixer_attach(osd.mixer, "hw:0") < 0 || snd_mixer_selem_register(osd.mixer, NULL, NULL) < 0 ||
	    snd_mixer_load(osd.mixer) < 0 || !(osd.elem = snd_mixer_find_selem(osd.mixer, id))) {
		snd_mixer_close(osd.mixer);
		return false;
	}
	snd_mixer_selem_get_playback_volume_range(osd.elem, &osd.min, &osd.max);
	return osd.max > osd.min;
}

WL_EXPORT int wet_module_init(struct weston_compositor *compositor, int *argc, char *argv[]) {
	(void)argc; (void)argv;
	osd.compositor = compositor;
	if (!open_mixer()) {
		weston_log("volume-osd: no 'Line Out' mixer control on hw:0\n");
		return -1;
	}
	weston_layer_init(&osd.layer, compositor);
	weston_layer_set_position(&osd.layer, WESTON_LAYER_POSITION_TOP_UI);
	osd.timer = wl_event_loop_add_timer(wl_display_get_event_loop(compositor->wl_display), hide, NULL);
	weston_compositor_add_key_binding(compositor, KEY_VOLUMEUP, 0, key, NULL);
	weston_compositor_add_key_binding(compositor, KEY_VOLUMEDOWN, 0, key, NULL);
	return 0;
}
