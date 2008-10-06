/*
 * Copyright © 2007, 2008 Christopher Eby <kreed@kreed.org>
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

#define _XOPEN_SOURCE 500

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <shadow.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/xf86vmode.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include "lock.xbm"

static bool do_fork = false;
static bool do_lock = false;
static int idle_time = 180;
static char *background = "#000000";
static char *failure = "#aa0000";
static char *foreground = "#4fa060";

static Display *dpy = NULL;
static int screen;
static Window window;
static Window trap;
static int lock_keycode;
static const char *password;
static pthread_t sleeper_thread = -1;

static bool fading = false;
static bool failing = false;
static bool locked = false;
static bool control_pid_prop = false;

static XColor background_color;
static XColor failure_color;
static XColor foreground_color;

static int xsync_event_base;
static XSyncAlarm idle_alarm = None;
static XSyncAlarm reset_alarm = None;
static XSyncValue timeout;
static XSyncCounter idle;

static void
cleanup(int sig)
{
	if (dpy) {
		if (control_pid_prop)
			XDeleteProperty(dpy, XDefaultRootWindow(dpy), XInternAtom(dpy, "_INERTIA_RUNNING", False));
		DPMSSetTimeouts(dpy, 0, 0, idle_time);
	}

	if (sig)
		exit(EXIT_SUCCESS);
	else if (dpy)
		/* this will block if we're inside a SIGTERM */
		XCloseDisplay(dpy);
}

static void
die(const char *errstr)
{
	cleanup(0);
	fputs(errstr, stderr);
	fflush(stderr);
	exit(EXIT_FAILURE);
}

static void
set_cursor(XColor *color)
{
	Window root = XRootWindow(dpy, screen);

	Pixmap pixmap = XCreateBitmapFromData(dpy, window, lock_bits, lock_width, lock_height);
	Cursor cursor = XCreatePixmapCursor(dpy, pixmap, pixmap, color, color, 0, 0);

	XFreePixmap(dpy, pixmap);

	unsigned len = 1000;
	while (--len && XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
	       GrabModeAsync, GrabModeAsync, trap, cursor, CurrentTime) != GrabSuccess)
		usleep(1000);

	XFreeCursor(dpy, cursor);
}

static void
lock()
{
	locked = true;

	DPMSSetTimeouts(dpy, 0, 0, 10);

	Window root = XRootWindow(dpy, screen);
	Visual *visual = XDefaultVisual(dpy, screen);
	int screen_width = XDisplayWidth(dpy, screen);
	int screen_height = XDisplayHeight(dpy, screen);

	XSetWindowAttributes wa;
	wa.override_redirect = 1;
	wa.background_pixel = background_color.pixel;

	window = XCreateWindow(dpy, root, 0, 0, screen_width, screen_height,
			0, CopyFromParent, CopyFromParent, visual,
			CWOverrideRedirect | CWBackPixel, &wa);
	XMapRaised(dpy, window);

	trap = XCreateWindow(dpy, root, 0, 0, screen_width - lock_width,
	                     screen_height - lock_height, 0, CopyFromParent,
	                     CopyFromParent, visual, CWOverrideRedirect, &wa);
	XMapRaised(dpy, trap);

	XWarpPointer(dpy, PointerWindow, window, 0, 0, 0, 0,
	             (screen_width - lock_width) / 2, (screen_height - lock_height) / 2);

	set_cursor(&foreground_color);

	unsigned len = 1000;
	while (--len && XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime)
	       != GrabSuccess)
		usleep(1000);
}

static void*
reset_cursor(void *ptr)
{
	failing = true;
	usleep(500000);
	set_cursor(&foreground_color);
	failing = false;
	return EXIT_SUCCESS;
}

static void
unlock()
{
	DPMSSetTimeouts(dpy, 0, 0, 0);

	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);
	XDestroyWindow(dpy, window);
	XDestroyWindow(dpy, trap);

	if (sleeper_thread != -1)
		pthread_cancel(sleeper_thread);

	locked = false;
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

static void fade();

static void
handle_xalarm_event(XSyncAlarmNotifyEvent *ev)
{
	int overflow;
	XSyncValue reset_timeout;
	XSyncValue minus_one;

	if (ev->alarm == idle_alarm) {
		XSyncIntToValue(&minus_one, -1);
		XSyncValueAdd(&reset_timeout, ev->counter_value, minus_one, &overflow);
		get_alarm(&reset_alarm, XSyncNegativeComparison, reset_timeout);
		if (!fading && !locked)
			fade();
	} else if (ev->alarm == reset_alarm)
		get_alarm(&idle_alarm, XSyncPositiveComparison, timeout);
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

	XFlush(dpy);

	const int steps = 1200;
	double ratio = 1.0;
	const double inc = 1.0 / steps;

	struct timespec sleep;
	sleep.tv_sec = 0;
	sleep.tv_nsec = 2000000000 / steps;

	unsigned j;
	XEvent event;
	bool event_recieved = false;

	while (ratio > 0.01) {
		if (XCheckTypedEvent(dpy, xsync_event_base + XSyncAlarmNotify, &event)) {
			event_recieved = true;
			break;
		}
		if (XCheckTypedEvent(dpy, KeyPress, &event) && event.xkey.keycode == lock_keycode)
			break;

		for (j = 0; j != size; ++j) {
			red[j] = ired[j] * ratio;
			green[j] = igreen[j] * ratio;
			blue[j] = iblue[j] * ratio;
		}
		XF86VidModeSetGammaRamp(dpy, screen, size, red, green, blue);

		nanosleep(&sleep, NULL);
		ratio -= inc;
	}

	XF86VidModeSetGammaRamp(dpy, screen, size, ired, igreen, iblue);

	free(red);
	free(green);
	free(blue);
	free(ired);
	free(igreen);
	free(iblue);

	if (event_recieved)
		handle_xalarm_event((XSyncAlarmNotifyEvent*)&event);
	else if (fading)
		lock();

	fading = false;
}

static void
parse_args(int argc, char **argv)
{
	for (;;)
		switch (getopt(argc, argv, "ildt:b:f:x:")) {
		case -1:
			return;
		case 'l':
			do_lock = true;
			break;
		case 'd':
			do_fork = true;
			break;
		case 't':
			idle_time = atoi(optarg);
			break;
		case 'b':
			background = optarg;
			break;
		case 'f':
			foreground = optarg;
			break;
		case 'x':
			failure = optarg;
			break;
		default:
			die("Usage: inertia [-t nsecs]\n\n"
				"Options:\n"
				"	-l	lock on start or lock the running instance\n"
				"	-d	daemonize\n"
				"	-t	lock the screen after ARG seconds (default 60)\n"
				"	-f	use ARG as the screen lock foreground color\n"
				"	-b	use ARG as the screen lock background color\n"
				"	-x	use ARG as the screen lock fail color\n");
		}
}

static void
initialize()
{
	if (geteuid() != 0)
		die("inertia: I don't have root privelages. Inertia may not be suid root.\n");

	struct spwd *auth_data = getspnam(getenv("USER"));
	password = strdup(auth_data->sp_pwdp);
	endspent();

	if (setgid(getgid()) == -1 || setuid(getuid()) == -1)
		die("inertia: cannot drop privileges; exiting.\n");

	XInitThreads();
	if (!(dpy = XOpenDisplay(0)))
		die("interia: cannot open display; exiting.\n");

	screen = XDefaultScreen(dpy);

	Window root = XDefaultRootWindow(dpy);
	Atom inert = XInternAtom(dpy, "_INERTIA_RUNNING", True);
	lock_keycode = XKeysymToKeycode(dpy, XK_Insert);

	if (inert) {
		Atom type;
		unsigned long dummy_long;
		int dummy_int;
		long *data;

		XGetWindowProperty(dpy, root, inert, 0, 1, False, XA_CARDINAL, &type,
		                   &dummy_int, &dummy_long, &dummy_long, (unsigned char**)&data);

		if (type) {
			pid_t pid = data[0];

			if (getsid(pid) != -1) {
				if (do_lock) {
					XKeyEvent event;
					event.display = dpy;
					event.root = event.window = root;
					event.subwindow = None;
					event.time = CurrentTime;
					event.x = event.y = event.x_root = event.y_root = 1;
					event.same_screen = True;
					event.type = KeyPress;
					event.keycode = lock_keycode;
					event.state = Mod1Mask | Mod2Mask | Mod5Mask | LockMask;
					XSendEvent(dpy, root, False, KeyPressMask, (XEvent*)&event);
					XFlush(dpy);
					exit(EXIT_SUCCESS);
				} else
					die("intertia: another instance is already running on this display; exiting.\n");
			}

			XFree(data);
		}
	} else
		inert = XInternAtom(dpy, "_INERTIA_RUNNING", False);

	control_pid_prop = true;

	struct sigaction sigact;
	sigaction(SIGTERM, NULL, &sigact);
	sigact.sa_handler = cleanup;
	sigaction(SIGTERM, &sigact, NULL);

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

	Colormap colormap = XDefaultColormap(dpy, screen);
	if (!XParseColor(dpy, colormap, foreground, &foreground_color))
		die("inertia: invalid foreground color\n");
	if (!XParseColor(dpy, colormap, failure, &failure_color))
		die("inertia: invalid fail color\n");
	if (!XParseColor(dpy, colormap, background, &background_color) || !XAllocColor(dpy, colormap, &background_color))
		die("inertia: invalid background color\n");

	/* deactivate lame built-in screensaver */
	XSetScreenSaver(dpy, 0, 0, PreferBlanking, AllowExposures);

	/* disable DPMS as well; we'll handle this ourselves */
	DPMSSetTimeouts(dpy, 0, 0, 0);

	XSyncIntToValue(&timeout, idle_time * 1000);
	get_alarm(&idle_alarm, XSyncPositiveComparison, timeout);

	if (do_fork) {
		int pid = fork();
		if (pid == -1)
			die("inertia: Failed to fork; exiting.\n");
		if (pid > 0)
			exit(EXIT_SUCCESS);

		i = open("/dev/null", O_RDWR);
		dup2(i, 0);
		dup2(i, 1);
		dup2(i, 2);
	}

	chdir("/");

	XGrabKey(dpy, lock_keycode, Mod1Mask | Mod2Mask | Mod5Mask | LockMask, root, True, GrabModeAsync, GrabModeAsync);
	XSelectInput(dpy, root, KeyPressMask);

	long data[1];
	data[0] = getpid();

	XChangeProperty(dpy, root, inert, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&data, 1);
	XFlush(dpy);

	if (do_lock)
		lock();
}

static void
main_loop()
{
	XEvent ev;

	unsigned len = 0;
	char buf[32];
	char entry[256];
	KeySym ksym;
	int keys;

	while (!XNextEvent(dpy, &ev)) {
		switch (ev.type) {
		case KeyPress:
			if (ev.xkey.keycode == lock_keycode) {
				if (!locked)
					lock();
				break;
			}

			buf[0] = '\0';
			keys = XLookupString(&ev.xkey, buf, sizeof buf, &ksym, 0);

			if (IsFunctionKey(ksym) || IsKeypadKey(ksym) || IsMiscFunctionKey(ksym)
					|| IsPFKey(ksym) || IsPrivateKeypadKey(ksym))
				break;

			switch(ksym) {
			case XK_Return:
				entry[len] = '\0';
				if (!strcmp(crypt(entry, password), password))
					unlock();
				else {
					if (!failing)
						set_cursor(&failure_color);
					else
						pthread_cancel(sleeper_thread);
					pthread_create(&sleeper_thread, NULL, reset_cursor, NULL);
					pthread_detach(sleeper_thread);
				}
			case XK_Escape:
				len = 0;
				break;
			case XK_BackSpace:
				if (len)
					--len;
				break;
			default:
				if (keys && !iscntrl((int)buf[0]) && len + keys < sizeof entry) {
					memcpy(entry + len, buf, keys);
					len += keys;
				}
				break;
			}
			break;
		default:
			if (ev.type == xsync_event_base + XSyncAlarmNotify)
				handle_xalarm_event((XSyncAlarmNotifyEvent*)&ev);
		}
	}
}

int
main(int argc, char **argv)
{
	parse_args(argc, argv);
	initialize();
	main_loop();
	return EXIT_SUCCESS;
}

