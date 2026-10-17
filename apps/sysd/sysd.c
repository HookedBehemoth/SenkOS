// SPDX-License-Identifier: MIT
/*
 * sysd: root's side of the UI, started (and again, should it die) by init (inittab).
 *
 * Suspends on lid close or a power-key press let go within HOLD_S (held longer, the launcher
 * shows the power options) and stays asleep while the lid is closed;
 * /usr/sbin/suspend picks which sleep, one the lid can end or super standby. Only reacts to
 * events, never to the state at startup, so a misread lid can't put the handheld to sleep at
 * boot.
 *
 * And what the launcher, unprivileged, asks for: org.senkos.System1 on the system bus
 * (apps/bus/org.senkos.System1.xml), from shell (the launcher) or root alone (the bus' policy,
 * org.senkos.System1.conf). Each is answered once it's done, an error saying why it failed.
 * Inside, a call is a request in words: "suspend"; "cpu KHZ" caps the CPU's clock (its
 * governor is performance: it runs at the cap); "gpu HZ" pins the GPU's (simple_ondemand
 * otherwise picks lower ones, and late), "gpu 0" frees it again; and the commands in `cmds`. The
 * backlight is the launcher's own (group video, a udev rule).
 * Without the bus (not up yet, or restarted by init) the lid and the power key still work.
 */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/pidfd.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "system1.h"

#define DROPBEAR "/etc/init.d/dropbear"
#define SSH_OFF "/data/OS/dropbear/disabled"
#define SSH_ALL "/data/OS/dropbear/everywhere"
#define LIGHT_SLEEP "/data/OS/config/light-sleep"
#define LIGHT_MODE "/uboot/light"
#define POWER_ON_MS "/data/OS/config/power-on-ms"
/* The power key held this long is the launcher's (POWER_HOLD_MS) */
#define HOLD_S 1.0
#define ARGV(...) ((const char *const[]){__VA_ARGS__, NULL})

enum { MAXDEV = 16, MAXJOB = 8, MAXWORDS = 8 };

/*
 * The request as `req`, or `req` and then a word of `words` (the words after it go along as
 * they are: the script checks them). `run` one after the other, as root and without a shell;
 * the reply is the last one's.
 */
static const struct cmd {
	const char *req, *words;
	const char *const *run[2];
} cmds[] = {
	{"reboot", NULL, {ARGV("reboot")}},
	{"poweroff", NULL, {ARGV("poweroff")}},
	/* The Storage settings */
	{"usb", "on off mtp smb", {ARGV("usb")}},
	{"wifi", "on off", {ARGV("wifi")}},
	{"clock", "zone ntp set", {ARGV("clock")}},
	/* The launcher is up: the slot works (`ab commit` keeps one on trial, an update), and
	 * Wi-Fi comes on if it was (`wifi`), not earlier: it would slow the boot */
	{"up", NULL, {ARGV("ab", "commit"), ARGV("wifi")}},
	/* SSH: off for good while SSH_OFF is there (the init script does nothing then, stop
	 * included: stopped first), on every network while SSH_ALL is, else on the USB cable's
	 * alone. /etc/default/dropbear reads both. */
	{"ssh on", NULL, {ARGV("rm", "-f", SSH_OFF), ARGV(DROPBEAR, "start")}},
	{"ssh off", NULL, {ARGV(DROPBEAR, "stop"), ARGV("touch", SSH_OFF)}},
	{"ssh all", NULL, {ARGV("touch", SSH_ALL), ARGV(DROPBEAR, "restart")}},
	{"ssh usb", NULL, {ARGV("rm", "-f", SSH_ALL), ARGV(DROPBEAR, "restart")}},
	/* `suspend` never powers the SoC off while LIGHT_SLEEP is there */
	{"sleep light", NULL, {ARGV("touch", LIGHT_SLEEP)}},
	{"sleep deep", NULL, {ARGV("rm", "-f", LIGHT_SLEEP)}},
	/* The UI's colors and the boot splash (boot.cmd): light while LIGHT_MODE is there */
	{"theme light", NULL, {ARGV("touch", LIGHT_MODE)}},
	{"theme dark", NULL, {ARGV("rm", "-f", LIGHT_MODE)}},
	/* The power key's hold to turn on, the PMU's at shutdown (save-settings) */
	{"power-on", "128 512 1000 2000", {ARGV("sh", "-c", "echo $0 > " POWER_ON_MS)}},
};

/* What runs for a call, which waits for the reply */
static struct job {
	pid_t pid;
	int pidfd;
	bool used;
	bus_req call;
	char req[64];	/* for the log */
} jobs[MAXJOB];

/* "ok", or "err WHY" */
static void reply(bus_req *call, const char *text)
{
	if (strncmp(text, "err ", 4) == 0)
		bus_reply_error(call, "org.senkos.System1.Error.Failed", text + 4);
	else
		bus_reply(call);
}

static void reply_status(bus_req *call, const char *req, int status)
{
	char text[32] = "ok";

	if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
		snprintf(text, sizeof(text), "err exit %d", WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		snprintf(text, sizeof(text), "err signal %d", WTERMSIG(status));
	syslog(LOG_INFO, "%s: %s", req, text);
	reply(call, text);
}

/* The child's: `run`'s commands one after the other, then exits with the last one's status */
static void run_all(const char *const *const *run, int n, char **extra)
{
	for (int i = 0; i < n; i++) {
		char *argv[MAXWORDS * 2];
		int argc = 0;

		for (const char *const *a = run[i]; *a != NULL; a++)
			argv[argc++] = (char *)*a;
		/* the request's words: the last command's */
		for (char **e = extra; i == n - 1 && *e != NULL; e++)
			argv[argc++] = *e;
		argv[argc] = NULL;
		if (i == n - 1) {
			execvp(argv[0], argv);
			_exit(127);
		}
		pid_t pid = fork();
		if (pid == 0) {
			execvp(argv[0], argv);
			_exit(127);
		}
		waitpid(pid, NULL, 0);
	}
	_exit(0);
}

/* `value`, digits only, into the first file `pattern` matches; the kernel checks the rest */
static int write_sysfs(const char *pattern, const char *value)
{
	glob_t g;
	int ret = 0;

	if (value == NULL || value[0] == '\0' || value[strspn(value, "0123456789")] != '\0')
		return EINVAL;
	if (glob(pattern, 0, NULL, &g) != 0)
		return ENOENT;
	int fd = open(g.gl_pathv[0], O_WRONLY | O_CLOEXEC);
	if (fd < 0 || write(fd, value, strlen(value)) < 0)
		ret = errno;
	if (fd >= 0)
		close(fd);
	globfree(&g);
	return ret;
}

/* For a sleep the kernel switches to a text console unless a framebuffer says it needn't, and
 * there's none (fbdev emulation is off): seatd would take the display from Weston and give it
 * back at every sleep. Locked, the switch fails and the sleep goes on without it. */
static void lock_vt(int on)
{
	int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);

	if (fd >= 0) {
		ioctl(fd, on ? VT_LOCKSWITCH : VT_UNLOCKSWITCH, 0);
		close(fd);
	}
}

#define TEST(bits, n) ((bits)[(n) / 8] & (1 << ((n) % 8)))

static int lid_closed(int fd)
{
	unsigned char sw[SW_MAX / 8 + 1] = { 0 };

	return fd >= 0 && ioctl(fd, EVIOCGSW(sizeof(sw)), sw) >= 0 && TEST(sw, SW_LID);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* 0 when asleep and back, else the first suspend's status. Again while the lid is closed. */
static int suspend(int lid)
{
	/* Just closed: a sleep the lid can end. Still closed after it: super standby. */
	const char *arg = lid_closed(lid) ? "lid" : NULL;
	int first = -1;

	lock_vt(1);
	do {
		int status = -1;
		pid_t pid = fork();
		if (pid == 0) {
			execl("/usr/sbin/suspend", "suspend", arg, (char *)NULL);
			_exit(127);
		}
		if (pid > 0)
			waitpid(pid, &status, 0);
		if (first == -1)
			first = status;
		if (status != 0)
			sleep(1);	/* suspend refused: don't spin */
		arg = NULL;
	} while (lid_closed(lid));
	lock_vt(0);
	return first;
}

/* A request of `call`'s (copied to answer later); 1 when it asks for a suspend (the reply is the
 * caller's) */
static int request(bus_req *call, const char *req)
{
	syslog(LOG_INFO, "%s", req);
	if (strcmp(req, "suspend") == 0)
		return 1;

	char *words[MAXWORDS + 1], *save;
	int nw = 0;
	char copy[256];
	strcpy(copy, req);
	for (char *w = strtok_r(copy, " ", &save); w != NULL && nw < MAXWORDS; w = strtok_r(NULL, " ", &save))
		words[nw++] = w;
	words[nw] = NULL;

	int err = -1;
	if (nw == 2 && strcmp(words[0], "cpu") == 0)
		err = write_sysfs("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq", words[1]);
	else if (nw == 2 && strcmp(words[0], "gpu") == 0) {
		/* 0: no limit, for either; both, whichever fails */
		err = write_sysfs("/sys/class/devfreq/*.gpu/max_freq", words[1]);
		int min = write_sysfs("/sys/class/devfreq/*.gpu/min_freq", words[1]);
		if (!err)
			err = min;
	}
	if (err >= 0) {
		char text[64] = "ok";
		if (err)
			snprintf(text, sizeof(text), "err %s", strerror(err));
		reply(call, text);
		return 0;
	}

	for (size_t i = 0; i < sizeof(cmds) / sizeof(*cmds); i++) {
		const struct cmd *c = &cmds[i];
		char **extra;

		if (c->words == NULL && strcmp(req, c->req) == 0)
			extra = words + nw;
		else if (c->words != NULL && nw >= 2 && strcmp(words[0], c->req) == 0) {
			/* a whole word of the list */
			const char *w = strstr(c->words, words[1]);
			size_t l = strlen(words[1]);
			if (w == NULL || (w != c->words && w[-1] != ' ') || (w[l] != ' ' && w[l] != '\0'))
				break;
			extra = words + 1;
		} else
			continue;

		struct job *j = NULL;
		for (int k = 0; k < MAXJOB && j == NULL; k++)
			if (!jobs[k].used)
				j = &jobs[k];
		if (j == NULL) {
			reply(call, "err busy");
			return 0;
		}
		int nrun = c->run[1] != NULL ? 2 : 1;
		pid_t pid = fork();
		if (pid == 0)
			run_all(c->run, nrun, extra);
		int pidfd = pid > 0 ? pidfd_open(pid, 0) : -1;
		if (pidfd < 0) {
			if (pid > 0)
				waitpid(pid, NULL, 0);
			reply(call, "err fork");
			return 0;
		}
		*j = (struct job){ .pid = pid, .pidfd = pidfd, .used = true, .call = *call };
		snprintf(j->req, sizeof(j->req), "%s", req);
		return 0;
	}
	syslog(LOG_WARNING, "unknown request: %s", req);
	reply(call, "err unknown");
	return 0;
}

/* A Suspend's call, answered once awake. The press that woke us, or a second Suspend meanwhile,
 * isn't another before `quiet_until`. */
static bus_req asked;
static bool asking;
static double quiet_until;

/* The bus' calls (system1.h), each a request in words: `cmds` and the scripts check them */
static int ask(bus_req *m, const char *fmt, ...)
{
	char req[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(req, sizeof(req), fmt, ap);
	va_end(ap);
	if (request(m, req)) {
		if (!asking && now() > quiet_until) {
			asked = *m;
			asking = true;
		} else
			reply(m, "ok");
	}
	return 0;
}

/* A string argument: one word, the words after the request's go to the script as they are */
static int ask_word(bus_req *m, const char *req, const char *w)
{
	char why[64];

	if (w[0] == '\0' || strpbrk(w, " \t\n") != NULL || strlen(w) > 64) {
		snprintf(why, sizeof(why), "%s: one word", req);
		return bus_reply_error(m, BUS_ERROR_INVALID_ARGS, why);
	}
	return ask(m, "%s %s", req, w);
}

int system1_suspend(bus_req *m, void *userdata)
{
	return ask(m, "suspend");
}

int system1_reboot(bus_req *m, void *userdata)
{
	return ask(m, "reboot");
}

int system1_power_off(bus_req *m, void *userdata)
{
	return ask(m, "poweroff");
}

int system1_launcher_up(bus_req *m, void *userdata)
{
	return ask(m, "up");
}

int system1_set_cpu_max(bus_req *m, void *userdata, uint32_t khz)
{
	return ask(m, "cpu %u", khz);
}

int system1_set_gpu_freq(bus_req *m, void *userdata, uint32_t hz)
{
	return ask(m, "gpu %u", hz);
}

int system1_set_usb(bus_req *m, void *userdata, const char *state)
{
	return ask_word(m, "usb", state);
}

int system1_set_wifi(bus_req *m, void *userdata, bool on)
{
	return ask(m, "wifi %s", on ? "on" : "off");
}

int system1_set_time_zone(bus_req *m, void *userdata, const char *zone)
{
	return ask_word(m, "clock zone", zone);
}

int system1_set_ntp(bus_req *m, void *userdata, bool on)
{
	return ask(m, "clock ntp %s", on ? "on" : "off");
}

int system1_set_time(bus_req *m, void *userdata, int64_t seconds)
{
	return ask(m, "clock set %lld", (long long)seconds);
}

int system1_set_ssh(bus_req *m, void *userdata, bool on)
{
	return ask(m, "ssh %s", on ? "on" : "off");
}

int system1_set_ssh_everywhere(bus_req *m, void *userdata, bool on)
{
	return ask(m, "ssh %s", on ? "all" : "usb");
}

int system1_set_light_sleep(bus_req *m, void *userdata, bool on)
{
	return ask(m, "sleep %s", on ? "light" : "deep");
}

int system1_set_light_theme(bus_req *m, void *userdata, bool on)
{
	return ask(m, "theme %s", on ? "light" : "dark");
}

int system1_set_power_on_ms(bus_req *m, void *userdata, uint32_t ms)
{
	return ask(m, "power-on %u", ms);
}

/* On the system bus as org.senkos.System1, or not */
static bool bus_up(bus_conn *bus)
{
	if (bus_open(bus, BUS_SYSTEM) < 0)
		return false;
	system1_serve(bus, NULL);
	if (bus_request_name(bus, SYSTEM1_NAME) < 0) {
		bus_close(bus);
		return false;
	}
	syslog(LOG_INFO, "on the system bus");
	return true;
}

int main(void)
{
	/* the devices, the socket, the jobs */
	struct pollfd pfd[MAXDEV + 1 + MAXJOB];
	int n = 0, lid = -1;
	/* power_down: when the key went down, 0 when up or the press woke us */
	double power_down = 0;
	glob_t g;

	openlog("sysd", LOG_PID, LOG_DAEMON);
	/* the scripts' stdout too; their stderr is init's console */
	int null = open("/dev/null", O_RDWR);
	dup2(null, 0);
	dup2(null, 1);
	if (null > 2)
		close(null);

	if (glob("/dev/input/event*", 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc && n < MAXDEV; i++) {
			unsigned char key[KEY_MAX / 8 + 1] = { 0 }, sw[SW_MAX / 8 + 1] = { 0 };
			int fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);

			if (fd < 0)
				continue;
			ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
			ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sw)), sw);
			if (!TEST(sw, SW_LID) && !TEST(key, KEY_POWER)) {
				close(fd);
				continue;
			}
			if (TEST(sw, SW_LID))
				lid = fd;
			syslog(LOG_INFO, "%s:%s%s", g.gl_pathv[i], TEST(sw, SW_LID) ? " lid" : "",
			       TEST(key, KEY_POWER) ? " power-key" : "");
			pfd[n++] = (struct pollfd){ .fd = fd, .events = POLLIN };
		}
		globfree(&g);
	}
	syslog(LOG_INFO, "lid is %s", lid < 0 ? "absent" : lid_closed(lid) ? "closed" : "open");

	static bus_conn bus;
	bool up = false;

	for (;;) {
		struct input_event ev;
		int sleep_now;

		/* the bus again a second after it went (init starts it again) */
		if (!up)
			up = bus_up(&bus);
		pfd[n] = (struct pollfd){ .fd = up ? bus_fd(&bus) : -1, .events = up ? bus_events(&bus) : 0 };
		for (int k = 0; k < MAXJOB; k++)
			pfd[n + 1 + k] = (struct pollfd){ .fd = jobs[k].used ? jobs[k].pidfd : -1, .events = POLLIN };
		if (poll(pfd, n + 1 + MAXJOB, up ? -1 : 1000) < 0)
			continue;

		for (int k = 0; k < MAXJOB; k++) {
			struct job *j = &jobs[k];
			int status;
			if (!j->used || !(pfd[n + 1 + k].revents & POLLIN) || waitpid(j->pid, &status, WNOHANG) != j->pid)
				continue;
			reply_status(&j->call, j->req, status);
			close(j->pidfd);
			j->used = false;
		}
		if (up && pfd[n].revents) {
			int r = bus_process(&bus);
			if (r < 0) {
				syslog(LOG_WARNING, "system bus: %s", strerror(-r));
				bus_close(&bus);
				up = false;
			}
		}
		sleep_now = asking;
		for (int i = 0; i < n; i++)
			while (read(pfd[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type == EV_SW && ev.code == SW_LID && ev.value == 1)
					sleep_now = 1;
				if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1)
					power_down = now() > quiet_until ? now() : 0;
				if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 0) {
					sleep_now |= power_down > 0 && now() - power_down < HOLD_S;
					power_down = 0;
				}
			}
		if (!sleep_now)
			continue;
		int status = suspend(lid);
		quiet_until = now() + 1.5;
		if (asking)
			reply_status(&asked, "suspend", status);
		asking = false;
	}
}
