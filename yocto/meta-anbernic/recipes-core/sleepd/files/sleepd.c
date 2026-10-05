// SPDX-License-Identifier: MIT
/*
 * sleepd: suspend on lid close or power-key press, stay asleep while the lid is closed.
 * Only reacts to events, never to the state at startup, so a misread lid can't
 * put the handheld to sleep at boot. The actual suspend is /usr/sbin/suspend.
 * Also the unprivileged launcher's way to suspend, reboot or power off: a line
 * "suspend", "reboot" or "poweroff" written to the FIFO /run/power (anbernic.init).
 * And to USB mass storage: "ums sd", "ums data" or "ums stop" runs /usr/sbin/ums; and the
 * Storage settings: "usb on|off|mtp|ums|smb|sd|data" runs /usr/sbin/usb.
 * And the launcher's Power settings: "cpu KHZ" caps the CPU's clock (its governor is
 * performance: it runs at the cap), "gpu HZ" pins the GPU's (simple_ondemand otherwise picks
 * lower ones, and late: a cap a step down cost Mario 64 a third of its frames), "gpu 0" frees
 * it again; "brightness N" sets the backlight.
 * And the Wi-Fi settings: "wifi on|off" runs /usr/sbin/wifi. "up": the launcher is up, so the
 * slot works (`ab commit` keeps one on trial, an update), and Wi-Fi comes on if it was (`wifi`),
 * not earlier: it would slow the boot. And the time settings: "clock ARGS" runs /usr/sbin/clock.
 * And Debug's: "ssh on|off" starts or stops dropbear, off for good while
 * /data/OS/dropbear/disabled is there; "ssh all|usb" restarts it on every network (while
 * /data/OS/dropbear/everywhere is there) or the USB cable's alone. /etc/default/dropbear reads both.
 */
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAXDEV 16

/* `value`, digits only, into the first file `pattern` matches; the kernel checks the rest */
static void write_sysfs(const char *pattern, const char *value)
{
	glob_t g;

	if (value[0] == '\0' || value[strspn(value, "0123456789")] != '\0' ||
	    glob(pattern, 0, NULL, &g) != 0)
		return;
	FILE *f = fopen(g.gl_pathv[0], "w");
	if (f != NULL) {
		fputs(value, f);
		fclose(f);
	}
	globfree(&g);
}
/* For a sleep the kernel switches to a text console unless a framebuffer says it needn't, and
 * there's none (fbdev emulation is off): seatd would take the display from Weston and give it
 * back at every sleep, and a few in a row (a charger plugged in and out, the lid shut) left
 * Weston without frames. Locked, the switch fails and the sleep goes on without it. */
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

int main(void)
{
	struct pollfd pfd[MAXDEV + 1];
	int n = 0, lid = -1;
	double quiet_until = 0;
	glob_t g;

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
			printf("sleepd: %s:%s%s\n", g.gl_pathv[i], TEST(sw, SW_LID) ? " lid" : "",
			       TEST(key, KEY_POWER) ? " power-key" : "");
			pfd[n++] = (struct pollfd){ .fd = fd, .events = POLLIN };
		}
		globfree(&g);
	}
	if (!n) {
		fprintf(stderr, "sleepd: no lid switch or power key found\n");
		return 1;
	}
	/* O_RDWR: a writer closing it doesn't leave the FIFO at EOF */
	int power = open("/run/power", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (power >= 0)
		pfd[n] = (struct pollfd){ .fd = power, .events = POLLIN };
	printf("sleepd: lid is %s\n", lid < 0 ? "absent" : lid_closed(lid) ? "closed" : "open");
	fflush(stdout);

	for (;;) {
		struct input_event ev;
		int sleep_now = 0;

		if (poll(pfd, n + (power >= 0), -1) < 0)
			continue;
		char buf[512];
		ssize_t len;
		while (power >= 0 && (len = read(power, buf, sizeof(buf) - 1)) > 0) {
			buf[len] = '\0';
			/* A request a line; one read can bring several */
			for (char *req = strtok(buf, "\n"); req != NULL; req = strtok(NULL, "\n")) {
				if (strncmp(req, "suspend", 7) == 0)
					sleep_now = 1;
				else if (strncmp(req, "reboot", 6) == 0)
					system("reboot");
				else if (strncmp(req, "poweroff", 8) == 0)
					system("poweroff");
				/* In the background: `ums data` takes the UI down and up again */
				else if (strncmp(req, "ums sd", 6) == 0)
					system("/usr/sbin/ums sd &");
				else if (strncmp(req, "ums data", 8) == 0)
					system("/usr/sbin/ums data &");
				else if (strncmp(req, "ums stop", 8) == 0)
					system("/usr/sbin/ums stop &");
				else if (strncmp(req, "cpu ", 4) == 0)
					write_sysfs("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq", req + 4);
				else if (strncmp(req, "gpu ", 4) == 0) {
					/* 0: no limit, for either */
					write_sysfs("/sys/class/devfreq/*.gpu/max_freq", req + 4);
					write_sysfs("/sys/class/devfreq/*.gpu/min_freq", req + 4);
				}
				else if (strncmp(req, "brightness ", 11) == 0)
					write_sysfs("/sys/class/backlight/*/brightness", req + 11);
				else if (strcmp(req, "wifi on") == 0)
					system("/usr/sbin/wifi on &");
				else if (strcmp(req, "wifi off") == 0)
					system("/usr/sbin/wifi off &");
				else if (strcmp(req, "up") == 0)
					system("/usr/sbin/ab commit; /usr/sbin/wifi </dev/null >/dev/null 2>&1 &");
				/* Stopped first: with the file there the init script does nothing, stop included */
				else if (strcmp(req, "ssh on") == 0)
					system("rm -f /data/OS/dropbear/disabled; /etc/init.d/dropbear start >/dev/null 2>&1 &");
				else if (strcmp(req, "ssh off") == 0)
					system("/etc/init.d/dropbear stop >/dev/null 2>&1; touch /data/OS/dropbear/disabled");
				else if (strcmp(req, "ssh all") == 0)
					system("touch /data/OS/dropbear/everywhere; /etc/init.d/dropbear restart >/dev/null 2>&1 &");
				else if (strcmp(req, "ssh usb") == 0)
					system("rm -f /data/OS/dropbear/everywhere; /etc/init.d/dropbear restart >/dev/null 2>&1 &");
				else if (strncmp(req, "clock ", 6) == 0) {
					/* Words of these only: it goes to a shell (`clock` checks the rest) */
					char cmd[160];
					if (strspn(req + 6, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/_+- ") ==
					    strlen(req + 6) && strlen(req + 6) < 100) {
						snprintf(cmd, sizeof(cmd), "/usr/sbin/clock %s </dev/null >/dev/null 2>&1 &", req + 6);
						system(cmd);
					}
				}
				else if (strncmp(req, "usb ", 4) == 0) {
					/* One of these words only: it goes to a shell */
					static const char *const usb[] = {"on", "off", "mtp", "ums", "smb", "sd", "data"};
					for (size_t i = 0; i < sizeof(usb) / sizeof(*usb); i++)
						if (strcmp(req + 4, usb[i]) == 0) {
							char cmd[64];
							snprintf(cmd, sizeof(cmd), "/usr/sbin/usb %s &", usb[i]);
							system(cmd);
						}
				}
			}
		}
		for (int i = 0; i < n; i++)
			while (read(pfd[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type == EV_SW && ev.code == SW_LID && ev.value == 1)
					sleep_now = 1;
				/* the press that woke us (or one queued meanwhile) must not re-suspend */
				if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1 && now() > quiet_until)
					sleep_now = 1;
			}
		if (!sleep_now)
			continue;
		lock_vt(1);
		do {
			if (system("/usr/sbin/suspend") != 0)
				sleep(1);	/* suspend refused: don't spin */
		} while (lid_closed(lid));
		lock_vt(0);
		quiet_until = now() + 1.5;
	}
}
