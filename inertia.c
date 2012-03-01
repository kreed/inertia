/*
 * Copyright © 2007-2009 Christopher Eby <kreed@kreed.org>
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

#define _XOPEN_SOURCE 600

#include <ctype.h>
#include <fcntl.h>
#include <shadow.h>
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
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include "lock.xbm"

#define STRINGIFY_P(num) #num
#define STRINGIFY(num) STRINGIFY_P(num)

#define IDLE_TIME 180
static int idle_time = IDLE_TIME;
static bool do_fork = false;
static bool do_lock = false;
static char *lock_str = NULL;
static char *idle_tester = NULL;

static char entry[256];
static unsigned entry_len = 0;
static const char *password;
static struct timeval fail_timeout = { 0 };
static struct timeval *loop_timeout = NULL;

static bool fading = false;
static bool locked = false;

static Display *dpy = NULL;
static int screen;
static Window window;
static Atom quit_atom = 0;
static Atom lock_atom = 0;
static int lock_keycode = 0;

static XColor background_color = { 0 };
static XColor foreground_color = { 0, .red = 0xaaaa };

static Pixmap lock_pixmap;
static int lock_x;
static int lock_y;

static int xsync_event_base;
static XSyncAlarm idle_alarm = None;
static XSyncAlarm reset_alarm = None;
static XSyncValue idle_timeout;
static XSyncCounter idle;

static void unlock();
static int grab_event(struct timeval *timeout);

static void
cleanup(int sig)
{
	if (dpy) {
		if (locked)
			unlock();

		DPMSSetTimeouts(dpy, 0, 0, idle_time);
		XCloseDisplay(dpy);
	}

	exit(EXIT_FAILURE);
}

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
	cleanup(0);
}

static void
create_lock(long fg, long bg)
{
	lock_pixmap = XCreatePixmapFromBitmapData(dpy, window, lock_bits, lock_width, lock_height, fg, bg, 24);
}

static void
draw_lock()
{
	int screen_width = XDisplayWidth(dpy, screen);
	int screen_height = XDisplayHeight(dpy, screen);
	XClearArea(dpy, window, 0, 0, screen_width, screen_height, false);
	GC gc = XCreateGC(dpy, window, 0, NULL);
	if (lock_x + lock_width > screen_width)
		lock_x = screen_width - lock_width;
	if (lock_y + lock_height > screen_height)
		lock_y = screen_height - lock_height;
	XCopyArea(dpy, lock_pixmap, window, gc, 0, 0, lock_width, lock_height, lock_x, lock_y);
	XFreeGC(dpy, gc);
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

	create_lock(foreground_color.pixel, background_color.pixel);
	lock_x = (screen_width - lock_width) / 2;
	lock_y = (screen_height - lock_height) / 2;
	draw_lock();
	XWarpPointer(dpy, PointerWindow, window, 0, 0, 0, 0, lock_x, lock_y);

	XColor invisColor = { 0 };
	char invis[] = { 0 };
	Pixmap pixmap = XCreateBitmapFromData(dpy, window, invis, 1, 1);
	Cursor cursor = XCreatePixmapCursor(dpy, pixmap, pixmap, &invisColor, &invisColor, 0, 0);

	unsigned len = 1000;
	while (--len && XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
	       GrabModeAsync, GrabModeAsync, window, cursor, CurrentTime) != GrabSuccess)
		usleep(1000);

	len = 1000;
	while (--len && XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime)
	       != GrabSuccess)
		usleep(1000);

	XFreePixmap(dpy, pixmap);
	XFreeCursor(dpy, cursor);
}

static void
unlock()
{
	DPMSSetTimeouts(dpy, 0, 0, 0);

	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);
	XDestroyWindow(dpy, window);

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
		lock();
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

static void
lock_now()
{
	Display *dpy = XOpenDisplay(NULL);
	Window root = XRootWindow(dpy, screen);
	Atom lock = XInternAtom(dpy, "_INERTIA_LOCK", False);
	XChangeProperty(dpy, root, lock, XA_CARDINAL, 32, PropModeReplace, NULL, 0);
	XSync(dpy, True);
	XCloseDisplay(dpy);
}

static void
parse_args(int argc, char **argv)
{
	for (;;)
		switch (getopt(argc, argv, "ilLdt:b:f:x:k:T:")) {
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
		case 'T':
			idle_tester = optarg;
			break;
		case 'L':
			lock_now();
			exit(EXIT_SUCCESS);
		case 'k':
			lock_str = optarg;
			break;
		default:
			die("Usage: inertia [-t nsecs]\n\n"
				"Options:\n"
				"	-l	lock on start\n"
				"	-L	attempt to lock the running instance\n"
				"	-d	daemonize\n"
				"	-t	lock the screen after ARG seconds (default " STRINGIFY(IDLE_TIME) ")\n"
				"	-T	execute the program ARG when fading to screen lock begins\n"
				"	-k	grab ARG as the lock key\n");
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

	if (!(dpy = XOpenDisplay(NULL)))
		die("interia: cannot open display; exiting.\n");

	screen = XDefaultScreen(dpy);

	Window root = XRootWindow(dpy, screen);
	quit_atom = XInternAtom(dpy, "_INERTIA_QUIT", False);
	lock_atom = XInternAtom(dpy, "_INERTIA_LOCK", False);

	XChangeProperty(dpy, root, quit_atom, XA_CARDINAL, 32, PropModeReplace, NULL, 0);

	XSelectInput(dpy, root, PropertyChangeMask);

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
	if (!XAllocColor(dpy, colormap, &background_color))
		die("inertia: invalid background color\n");
	if (!XAllocColor(dpy, colormap, &foreground_color))
		die("inertia: invalid foreground color\n");

	if (lock_str) {
		KeySym sym = XStringToKeysym(lock_str);
		if (sym == NoSymbol)
			die("inertia: failed to parse lock keystr. Exiting.\n");
		else {
			lock_keycode = XKeysymToKeycode(dpy, sym);
			XGrabKey(dpy, lock_keycode, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
		}
	}

	/* deactivate lame built-in screensaver */
	XSetScreenSaver(dpy, 0, 0, PreferBlanking, AllowExposures);

	/* disable DPMS as well; we'll handle this ourselves */
	DPMSSetTimeouts(dpy, 0, 0, 0);

	XSyncIntToValue(&idle_timeout, idle_time * 1000);
	get_alarm(&idle_alarm, XSyncPositiveComparison, idle_timeout);

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

	XFlush(dpy);

	if (do_lock)
		lock();
}

static bool
pointer_in_hotspot(Display *dpy)
{
	int x;
	int y;
	Window black;
	int hole;

	black = XRootWindow(dpy, screen);
	return XQueryPointer(dpy, black, &black, &black, &x, &y, &hole, &hole, (unsigned*)&hole) && y == 0 && x == 0;
}

static void
invert()
{
	long fg, bg;
	if (loop_timeout) {
		// normal
		fg = foreground_color.pixel;
		bg = background_color.pixel;
	} else {
		// fail
		fg = background_color.pixel;
		bg = foreground_color.pixel;
	}
	XSetWindowBackground(dpy, window, bg);
	create_lock(fg, bg);
	loop_timeout = loop_timeout ? NULL : &fail_timeout;
	draw_lock();
}

static int
grab_event(struct timeval *timeout)
{
	XEvent ev;

	if (XNextEventTimeout(dpy, &ev, timeout) == -1) {
		return -1;
	} else {
		switch (ev.type) {
		case PropertyNotify:
			if (ev.xproperty.atom == quit_atom)
				cleanup(0);
			else if (ev.xproperty.atom == lock_atom && !locked) {
				lock();
				return 1;
			}
			break;
		case KeyPress: {
			if (ev.xkey.keycode == lock_keycode && !locked) {
				lock();
				return 1;
			}

			char buf[32];
			KeySym ksym;
			int keys;

			buf[0] = '\0';
			keys = XLookupString(&ev.xkey, buf, sizeof buf, &ksym, 0);

			if (IsFunctionKey(ksym) || IsKeypadKey(ksym) || IsMiscFunctionKey(ksym)
					|| IsPFKey(ksym) || IsPrivateKeypadKey(ksym))
				break;

			switch(ksym) {
			case XK_Return:
				entry[entry_len] = '\0';
				if (!strcmp(crypt(entry, password), password))
					unlock();
				else {
					if (!loop_timeout)
						invert();
					fail_timeout.tv_usec = 500000;
				}
			case XK_Escape:
				entry_len = 0;
				break;
			case XK_BackSpace:
				if (entry_len)
					--entry_len;
				break;
			default:
				if (keys && !iscntrl((int)buf[0]) && entry_len + keys < sizeof entry) {
					memcpy(entry + entry_len, buf, keys);
					entry_len += keys;
				}
				break;
			}
			break;
		}
		case MotionNotify:
			lock_x = ev.xmotion.x;
			lock_y = ev.xmotion.y;
			draw_lock();
			break;
		default:
			if (ev.type == xsync_event_base + XSyncAlarmNotify) {
				XSyncAlarmNotifyEvent *e = (XSyncAlarmNotifyEvent*)&ev;

				if (e->alarm == idle_alarm) {
					int overflow;
					XSyncValue reset_timeout;
					XSyncValue minus_one;

					XSyncIntToValue(&minus_one, -1);
					XSyncValueAdd(&reset_timeout, e->counter_value, minus_one, &overflow);
					get_alarm(&reset_alarm, XSyncNegativeComparison, reset_timeout);

					if (!fading && !locked && !pointer_in_hotspot(dpy)) {
						if (idle_tester && fork() == 0) {
							execlp(idle_tester, idle_tester, NULL);
							exit(EXIT_SUCCESS);
						}
						fade();
					}
				} else if (e->alarm == reset_alarm) {
					get_alarm(&idle_alarm, XSyncPositiveComparison, idle_timeout);
					return 1;
				}
			}
		}
	}

	return 0;
}

int
main(int argc, char **argv)
{
	parse_args(argc, argv);
	initialize();
	for (;;)
		if (grab_event(loop_timeout) == -1)
			invert();
	return EXIT_SUCCESS;
}

