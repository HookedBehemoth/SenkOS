// SPDX-License-Identifier: MIT
/*
 * sleepd: suspend on lid close or power-key press, stay asleep while the lid is closed.
 * Only reacts to events, never to the state at startup, so a misread lid can't
 * put the handheld to sleep at boot. The actual suspend is /usr/sbin/suspend.
 * Also the unprivileged launcher's way to suspend, reboot or power off: a line
 * "suspend", "reboot" or "poweroff" written to the FIFO /run/power (anbernic.init).
 * And to USB mass storage: "ums sd", "ums data" or "ums stop" runs /usr/sbin/ums; and the
 * Storage settings: "usb on|off|mtp|ums|smb|sd|data" runs /usr/sbin/usb.
 */
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAXDEV 16
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
		char req[64];
		ssize_t len;
		while (power >= 0 && (len = read(power, req, sizeof(req) - 1)) > 0) {
			req[len] = '\0';
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
			else if (strncmp(req, "usb ", 4) == 0) {
				/* One of these words only: it goes to a shell */
				static const char *const usb[] = {"on", "off", "mtp", "ums", "smb", "sd", "data"};
				req[strcspn(req, "\n")] = '\0';
				for (size_t i = 0; i < sizeof(usb) / sizeof(*usb); i++)
					if (strcmp(req + 4, usb[i]) == 0) {
						char cmd[64];
						snprintf(cmd, sizeof(cmd), "/usr/sbin/usb %s &", usb[i]);
						system(cmd);
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
		do {
			if (system("/usr/sbin/suspend") != 0)
				sleep(1);	/* suspend refused: don't spin */
		} while (lid_closed(lid));
		quiet_until = now() + 1.5;
	}
}
