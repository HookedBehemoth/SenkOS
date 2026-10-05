/* Weston module: counts the frames each output shows, for the launcher's power
 * calibration (frames a second, whichever client draws them). The count, a
 * uint32_t, is kept in $XDG_RUNTIME_DIR/frames, rewritten in place each frame.
 * Counted at each repaint's latch: frame_signal is only for frames Weston
 * renders itself, and a listener on it makes the DRM backend render every
 * frame, even a client's buffer it would put straight on a plane.
 * Loaded with `weston --modules=frames.so`. */
#include <fcntl.h>
#include <libweston/libweston.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int fd = -1;
static uint32_t count;
static struct wl_listener created;

static void frame(struct wl_listener *l, void *data) {
	(void)l; (void)data;
	count++;
	if (pwrite(fd, &count, sizeof(count), 0) != sizeof(count))
		return;
}

static void watch(struct weston_output *output) {
	struct wl_listener *l = calloc(1, sizeof(*l));
	if (l == NULL)
		return;
	l->notify = frame;
	wl_signal_add(&output->post_latch_signal, l);
}

static void output_created(struct wl_listener *l, void *data) {
	(void)l;
	watch(data);
}

WL_EXPORT int wet_module_init(struct weston_compositor *compositor, int *argc, char *argv[]) {
	(void)argc; (void)argv;
	const char *dir = getenv("XDG_RUNTIME_DIR");
	char path[256];
	snprintf(path, sizeof(path), "%s/frames", dir ? dir : "/tmp");
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		weston_log("frames: can't open %s\n", path);
		return -1;
	}
	struct weston_output *output;
	wl_list_for_each(output, &compositor->output_list, link)
		watch(output);
	created.notify = output_created;
	wl_signal_add(&compositor->output_created_signal, &created);
	return 0;
}
