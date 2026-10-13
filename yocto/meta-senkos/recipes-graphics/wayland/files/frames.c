/* Weston module: the launcher's hooks into the outputs. Loaded with
 * `weston --modules=frames.so`; both files are in $XDG_RUNTIME_DIR.
 *
 * frames: the frames each output shows, for the launcher's power calibration
 * (frames a second, whichever client draws them). A uint32_t, rewritten in
 * place each frame. Counted at each repaint's latch: frame_signal is only for
 * frames Weston renders itself, and a listener on it makes the DRM backend
 * render every frame, even a client's buffer it would put straight on a plane.
 *
 * refresh: a FIFO; a line "MHZ" (the refresh rate in mHz, as wl_output gives
 * it) switches each output to its mode of that rate at its size, 0 to the
 * mode it started with. Clients hear of it as of any mode change. */
#include <fcntl.h>
#include <libweston/libweston.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fd = -1;
static uint32_t count;
static struct wl_listener created;

/* Each output's mode at start: "0" */
struct output_start {
	struct wl_listener destroyed;
	struct weston_output *output;
	struct weston_mode *mode;
	struct wl_list link;
};
static struct wl_list starts;

static void frame(struct wl_listener *l, void *data) {
	(void)l; (void)data;
	count++;
	if (pwrite(fd, &count, sizeof(count), 0) != sizeof(count))
		return;
}

static void output_destroyed(struct wl_listener *l, void *data) {
	(void)data;
	struct output_start *start = wl_container_of(l, start, destroyed);
	wl_list_remove(&start->link);
	wl_list_remove(&start->destroyed.link);
	free(start);
}

static void watch(struct weston_output *output) {
	struct wl_listener *l = calloc(1, sizeof(*l));
	struct output_start *start = calloc(1, sizeof(*start));
	if (l == NULL || start == NULL) {
		free(l);
		free(start);
		return;
	}
	l->notify = frame;
	wl_signal_add(&output->post_latch_signal, l);
	start->output = output;
	start->mode = output->current_mode;
	start->destroyed.notify = output_destroyed;
	wl_signal_add(&output->destroy_signal, &start->destroyed);
	wl_list_insert(&starts, &start->link);
}

static void output_created(struct wl_listener *l, void *data) {
	(void)l;
	watch(data);
}

static void set_refresh(int mhz) {
	struct output_start *start;
	wl_list_for_each(start, &starts, link) {
		struct weston_output *output = start->output;
		struct weston_mode *mode, *want = mhz == 0 ? start->mode : NULL;
		if (want == NULL)
			wl_list_for_each(mode, &output->mode_list, link)
				if (mode->width == output->current_mode->width &&
				    mode->height == output->current_mode->height && mode->refresh == mhz)
					want = mode;
		if (want == NULL || want == output->current_mode)
			continue;
		weston_log("frames: %s at %d mHz\n", output->name, want->refresh);
		weston_output_mode_set_native(output, want, output->current_scale);
	}
}

/* The last whole line read wins */
static int refresh_readable(int rfd, uint32_t mask, void *data) {
	(void)mask; (void)data;
	char buf[256];
	ssize_t n = read(rfd, buf, sizeof(buf) - 1);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	int mhz = -1;
	for (char *line = buf, *end; (end = strchr(line, '\n')) != NULL; line = end + 1)
		mhz = atoi(line);
	if (mhz >= 0)
		set_refresh(mhz);
	return 0;
}

WL_EXPORT int wet_module_init(struct weston_compositor *compositor, int *argc, char *argv[]) {
	(void)argc; (void)argv;
	wl_list_init(&starts);
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

	/* Read and write: never at EOF without a writer */
	snprintf(path, sizeof(path), "%s/refresh", dir ? dir : "/tmp");
	unlink(path);
	int rfd = mkfifo(path, 0660) == 0 ? open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC) : -1;
	if (rfd < 0)
		weston_log("frames: can't make %s\n", path);
	else
		wl_event_loop_add_fd(wl_display_get_event_loop(compositor->wl_display), rfd, WL_EVENT_READABLE,
				     refresh_readable, NULL);
	return 0;
}
