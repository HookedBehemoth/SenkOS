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
 * And what the launcher, unprivileged, asks for: a SOCK_SEQPACKET socket, /run/sysd.sock, a
 * request a connection, from shell (the launcher) or root alone (SO_PEERCRED). The reply once
 * it's done: "ok", or "err WHY". The requests: "suspend"; "cpu KHZ" caps the CPU's clock (its
 * governor is performance: it runs at the cap); "gpu HZ" pins the GPU's (simple_ondemand
 * otherwise picks lower ones, and late), "gpu 0" frees it again; "brightness N" sets the backlight; and the commands in `cmds`.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/pidfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define SOCKET "/run/sysd.sock"
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

/* What runs for a client, which waits for the reply */
static struct job {
	pid_t pid;
	int pidfd, client;
	char req[64];	/* for the log */
} jobs[MAXJOB];

static void reply(int client, const char *text)
{
	send(client, text, strlen(text), MSG_NOSIGNAL);
	close(client);
}

static void reply_status(int client, const char *req, int status)
{
	char text[32] = "ok";

	if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
		snprintf(text, sizeof(text), "err exit %d", WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		snprintf(text, sizeof(text), "err signal %d", WTERMSIG(status));
	syslog(LOG_INFO, "%s: %s", req, text);
	reply(client, text);
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

/* A request from `client`; 1 when it asks for a suspend (the reply is the caller's) */
static int request(int client, char *req, uid_t shell)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);

	/* read first: closed unread, the client would get a reset rather than the reply */
	ssize_t n = recv(client, req, 255, 0);
	if (n <= 0) {
		close(client);
		return 0;
	}
	req[n] = '\0';
	if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
	    (cred.uid != 0 && cred.uid != shell)) {
		syslog(LOG_WARNING, "uid %u denied: %s", cred.uid, req);
		reply(client, "err denied");
		return 0;
	}
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
	} else if (nw == 2 && strcmp(words[0], "brightness") == 0)
		err = write_sysfs("/sys/class/backlight/*/brightness", words[1]);
	if (err >= 0) {
		char text[64] = "ok";
		if (err)
			snprintf(text, sizeof(text), "err %s", strerror(err));
		reply(client, text);
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
			if (jobs[k].client < 0)
				j = &jobs[k];
		if (j == NULL) {
			reply(client, "err busy");
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
			reply(client, "err fork");
			return 0;
		}
		*j = (struct job){ .pid = pid, .pidfd = pidfd, .client = client };
		snprintf(j->req, sizeof(j->req), "%s", req);
		return 0;
	}
	syslog(LOG_WARNING, "unknown request: %s", req);
	reply(client, "err unknown");
	return 0;
}

int main(void)
{
	/* the devices, the socket, the jobs */
	struct pollfd pfd[MAXDEV + 1 + MAXJOB];
	int n = 0, lid = -1;
	/* power_down: when the key went down, 0 when up or the press woke us */
	double quiet_until = 0, power_down = 0;
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

	struct passwd *pw = getpwnam("shell");
	uid_t shell = pw != NULL ? pw->pw_uid : 0;
	struct sockaddr_un addr = { .sun_family = AF_UNIX, .sun_path = SOCKET };
	int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	unlink(SOCKET);
	if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    chmod(SOCKET, 0666) != 0 || listen(sock, 16) != 0) {
		syslog(LOG_ERR, SOCKET ": %m");
		sleep(5);	/* init starts us again */
		return 1;
	}
	pfd[n] = (struct pollfd){ .fd = sock, .events = POLLIN };
	for (int k = 0; k < MAXJOB; k++)
		jobs[k].client = -1;

	for (;;) {
		struct input_event ev;
		int sleep_now = 0, asked = -1;

		for (int k = 0; k < MAXJOB; k++)
			pfd[n + 1 + k] = (struct pollfd){ .fd = jobs[k].client >= 0 ? jobs[k].pidfd : -1, .events = POLLIN };
		if (poll(pfd, n + 1 + MAXJOB, -1) < 0)
			continue;

		for (int k = 0; k < MAXJOB; k++) {
			struct job *j = &jobs[k];
			int status;
			if (j->client < 0 || !(pfd[n + 1 + k].revents & POLLIN) || waitpid(j->pid, &status, WNOHANG) != j->pid)
				continue;
			reply_status(j->client, j->req, status);
			close(j->pidfd);
			j->client = -1;
		}
		if (pfd[n].revents & POLLIN) {
			int client = accept4(sock, NULL, NULL, SOCK_CLOEXEC);
			/* a client that connects and doesn't ask mustn't hold us up */
			struct timeval tv = { .tv_sec = 1 };
			char req[256];
			if (client >= 0 && setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
			    request(client, req, shell)) {
				/* the press that woke us, or a second request meanwhile, isn't another */
				if (now() > quiet_until) {
					sleep_now = 1;
					asked = client;
				} else
					reply(client, "ok");
			}
		}
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
		if (asked >= 0)
			reply_status(asked, "suspend", status);
	}
}
