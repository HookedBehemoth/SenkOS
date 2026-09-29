/* Weston module: volume keys set the codec's Line Out, whichever client has
 * focus. The launcher watches the mixer and shows the level.
 * Loaded with `weston --modules=volume-keys.so`. */
#include <alsa/asoundlib.h>
#include <libweston/libweston.h>
#include <linux/input-event-codes.h>

#define STEPS 16

static struct {
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem;
	long min, max;
} vol;

static void key(struct weston_keyboard *kb, const struct timespec *t, uint32_t k, void *data) {
	(void)kb; (void)t; (void)data;
	long raw;
	snd_mixer_handle_events(vol.mixer); /* amixer may have changed it */
	snd_mixer_selem_get_playback_volume(vol.elem, SND_MIXER_SCHN_FRONT_LEFT, &raw);
	long range = vol.max - vol.min;
	int level = ((raw - vol.min) * STEPS + range / 2) / range;
	level += k == KEY_VOLUMEUP ? 1 : -1;
	level = level < 0 ? 0 : level > STEPS ? STEPS : level;
	snd_mixer_selem_set_playback_volume_all(vol.elem, vol.min + (level * range + STEPS / 2) / STEPS);
	snd_mixer_selem_set_playback_switch_all(vol.elem, level > 0);
}

static bool open_mixer(void) {
	snd_mixer_selem_id_t *id;
	snd_mixer_selem_id_alloca(&id);
	snd_mixer_selem_id_set_name(id, "Line Out");
	if (snd_mixer_open(&vol.mixer, 0) < 0)
		return false;
	if (snd_mixer_attach(vol.mixer, "hw:0") < 0 || snd_mixer_selem_register(vol.mixer, NULL, NULL) < 0 ||
	    snd_mixer_load(vol.mixer) < 0 || !(vol.elem = snd_mixer_find_selem(vol.mixer, id))) {
		snd_mixer_close(vol.mixer);
		return false;
	}
	snd_mixer_selem_get_playback_volume_range(vol.elem, &vol.min, &vol.max);
	return vol.max > vol.min;
}

WL_EXPORT int wet_module_init(struct weston_compositor *compositor, int *argc, char *argv[]) {
	(void)argc; (void)argv;
	if (!open_mixer()) {
		weston_log("volume-keys: no 'Line Out' mixer control on hw:0\n");
		return -1;
	}
	weston_compositor_add_key_binding(compositor, KEY_VOLUMEUP, 0, key, NULL);
	weston_compositor_add_key_binding(compositor, KEY_VOLUMEDOWN, 0, key, NULL);
	return 0;
}
