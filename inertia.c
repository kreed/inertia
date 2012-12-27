/*
 * Copyright © 2007-2012 Christopher Eby <kreed@kreed.org>
 *
 * This file is part of Inertia.
 *
 * Inertia is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * Inertia is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See <http://www.gnu.org/licenses/> for the full license text.
 */

#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/xf86vmode.h>

#define STRINGIFY_P(num) #num
#define STRINGIFY(num) STRINGIFY_P(num)

#define DEFAULT_IDLE_TIME 300
#define DEFAULT_CMD "systemctl suspend"
static int idle_time = DEFAULT_IDLE_TIME;
static char *idle_exec = DEFAULT_CMD;
static bool do_fork = false;

static bool fading = false;

static Display *dpy = NULL;
static int screen;

static int xsync_event_base;
static XSyncAlarm idle_alarm = None;
static XSyncAlarm reset_alarm = None;
static XSyncValue idle_timeout;
static XSyncCounter idle;

static int grab_event(struct timeval *timeout);

static int
XNextEventTimeout(Display *dpy, XEvent *ev, struct timeval *timeout)
{
	fd_set fds;
	int fd = ConnectionNumber(dpy);

	XFlush(dpy);

	for (;;) {
		if (XPending(dpy)) {
			XNextEvent(dpy, ev);
			return 0;
		}

		FD_ZERO(&fds);
		FD_SET(fd, &fds);

		select(fd + 1, &fds, NULL, NULL, timeout);

		if (timeout && timeout->tv_sec == 0 && timeout->tv_usec == 0)
			return -1;
	}

	return 1;
}

static void
die(const char *errstr)
{
	fputs(errstr, stderr);
	fflush(stderr);
	exit(EXIT_FAILURE);
}

static void
get_alarm(XSyncAlarm *alarm, XSyncTestType type, XSyncValue value)
{
	XSyncAlarmAttributes attrs;

	XSyncValue delta;
	XSyncIntToValue(&delta, 0);

	static const unsigned long flags = XSyncCACounter | XSyncCATestType | XSyncCAValue | XSyncCADelta;

	attrs.trigger.counter = idle;
	attrs.trigger.test_type = type;
	attrs.trigger.wait_value = value;
	attrs.delta = delta;

	if (*alarm)
		XSyncChangeAlarm(dpy, *alarm, flags, &attrs);
	else
		*alarm = XSyncCreateAlarm(dpy, flags, &attrs);
}

static void
fade()
{
	fading = true;

	int size;
	XF86VidModeGetGammaRampSize(dpy, screen, &size);

	unsigned short *red = malloc(sizeof(unsigned short) * size);
	unsigned short *green = malloc(sizeof(unsigned short) * size);
	unsigned short *blue = malloc(sizeof(unsigned short) * size);

	unsigned short *ired = malloc(sizeof(unsigned short) * size);
	unsigned short *igreen = malloc(sizeof(unsigned short) * size);
	unsigned short *iblue = malloc(sizeof(unsigned short) * size);

	XF86VidModeGetGammaRamp(dpy, screen, size, ired, igreen, iblue);

	static double ratio_step = 1.0 / 2400;
	static const int time_step = 1600;
	struct timeval sleep = { 0 };
	double ratio;
	unsigned j;

	for (ratio = 1.0; ratio > 0.01; ratio -= ratio_step) {
		for (j = 0; j != size; ++j) {
			red[j] = ired[j] * ratio;
			green[j] = igreen[j] * ratio;
			blue[j] = iblue[j] * ratio;
		}

		XF86VidModeSetGammaRamp(dpy, screen, size, red, green, blue);

		sleep.tv_usec = time_step;
		if (grab_event(&sleep) == 1) {
			fading = false;
			break;
		}
	}

	if (fading) {
		if (fork() == 0) {
			system(idle_exec);
			exit(EXIT_SUCCESS);
		}
		DPMSEnable(dpy);
		usleep(100000);
		DPMSForceLevel(dpy, DPMSModeOff);
		fading = false;
	}

	XF86VidModeSetGammaRamp(dpy, screen, size, ired, igreen, iblue);

	free(red);
	free(green);
	free(blue);
	free(ired);
	free(igreen);
	free(iblue);
}

static int
grab_event(struct timeval *timeout)
{
	XEvent ev;

	if (XNextEventTimeout(dpy, &ev, timeout) == -1) {
		return -1;
	} if (ev.type == xsync_event_base + XSyncAlarmNotify) {
		XSyncAlarmNotifyEvent *e = (XSyncAlarmNotifyEvent*)&ev;

		if (e->alarm == idle_alarm) {
			int overflow;
			XSyncValue reset_timeout;
			XSyncValue minus_one;

			XSyncIntToValue(&minus_one, -1);
			XSyncValueAdd(&reset_timeout, e->counter_value, minus_one, &overflow);
			get_alarm(&reset_alarm, XSyncNegativeComparison, reset_timeout);

			if (!fading) {
				fade();
			}
		} else if (e->alarm == reset_alarm) {
			get_alarm(&idle_alarm, XSyncPositiveComparison, idle_timeout);
			return 1;
		}
	}

	return 0;
}

int
main(int argc, char **argv)
{
	for (;;) {
		switch (getopt(argc, argv, "dt:e:")) {
		case -1:
			goto start;
		case 'd':
			do_fork = true;
			break;
		case 't':
			idle_time = atoi(optarg);
			break;
		case 'e':
			idle_exec = optarg;
			break;
		default:
			die("Usage: inertia [-t nsecs]\n\n"
				"Options:\n"
				"    -d       daemonize\n"
				"    -t secs  lock the screen after ARG seconds (default " STRINGIFY(DEFAULT_IDLE_TIME) ")\n"
				"    -e cmd   what to exec after screen fade ends (default \"" DEFAULT_CMD "\")\n");
		}
	}

start:
	if (!(dpy = XOpenDisplay(NULL)))
		die("inertia: cannot open display; exiting.\n");

	screen = XDefaultScreen(dpy);

	int xsync_error_base;
	int xsync_major = SYNC_MAJOR_VERSION;
	int xsync_minor = SYNC_MINOR_VERSION;

	if (!XSyncQueryExtension(dpy, &xsync_event_base, &xsync_error_base) || !XSyncInitialize(dpy, &xsync_major, &xsync_minor))
		die("inertia: No XSync extension; exiting.\n");

	idle = None;
	int i;
	XSyncSystemCounter *counters = XSyncListSystemCounters(dpy, &i);
	while (i--)
		if (!strcmp(counters[i].name, "IDLETIME"))
			idle = counters[i].counter;
	XSyncFreeSystemCounterList(counters);

	if (idle == None)
		die("inertia: No IDLETIME counter! xorg-server 1.3 and higher should support it. Exiting.\n");

	XSyncIntToValue(&idle_timeout, idle_time * 1000);
	get_alarm(&idle_alarm, XSyncPositiveComparison, idle_timeout);

	if (do_fork) {
		int pid = fork();
		if (pid == -1)
			die("inertia: Failed to fork; exiting.\n");
		if (pid > 0)
			exit(EXIT_SUCCESS);

		int fd = open("/dev/null", O_RDWR);
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
	}

	chdir("/");

	struct sigaction sig_act;
	memset(&sig_act, 0x0, sizeof(struct sigaction));
	sig_act.sa_flags = SA_NOCLDWAIT;
	sigaction(SIGCHLD, &sig_act, NULL);

	XFlush(dpy);

	for (;;) {
		grab_event(NULL);
	}

	return EXIT_SUCCESS;
}

