/*
 * Copyright (C) 2013 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2013 John Crispin <blogic@openwrt.org>
 * Copyright (C) 2017 Mateusz Banaszek
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/watchdog.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

#include <unistd.h>
#include <stdlib.h>

#include <libubox/uloop.h>

#include "procd.h"
#include "watchdog.h"

#define WDT_PATH	"/dev/watchdog"

#define WATCHDOG_CLIENT "watchdog-client"
#define WATCHDOG_CLIENT_RESTART_CODE (64)
#define WDTE_FREQUENCY (12) // fires every (WDTE_FREQUENCY * wdt_frequency) seconds
#define WDTE_FAILURES_THRESHOLD (15)

static struct uloop_timeout wdt_timeout;
static int wdt_fd = -1;
static int wdt_frequency = 5;
static int wdte_failures = 0;
static int wdte_cycle = 0;


/*
	Runs watchdog-client.
	Returns it's exit code or 2 if something went wrong.
*/
static int watchdog_run_client(void)
{
	int state = system(WATCHDOG_CLIENT);

	if (state == -1)
		return 2;
	if (!WIFEXITED(state))
		return 2;

	return(WEXITSTATUS(state));
}

/*
	Software "extension" of the hardware watchdog:
	allows less frequent running watchdog-client.
*/
static void watchdog_extended(void) {
	wdte_cycle++;

	if (wdte_cycle >= WDTE_FREQUENCY) {
		wdte_cycle = 0;

		int state = watchdog_run_client();
		switch (state) {
			case 0:
				wdte_failures = 0;
				break;
			case WATCHDOG_CLIENT_RESTART_CODE:
				ERROR("watchdog-client responded with RESTART code\n");
				wdte_failures = WDTE_FAILURES_THRESHOLD; // we want to restart the device
				break;
			default:
				ERROR("watchdog-client responded with code %i\n", state);
				wdte_failures++;
				break;
		}

	}
}

void watchdog_ping(void)
{
	DEBUG(4, "Ping\n");
	if (wdt_fd >= 0 && write(wdt_fd, "X", 1) < 0)
		ERROR("WDT failed to write: %s\n", strerror(errno));
}

static void watchdog_timeout_cb(struct uloop_timeout *t)
{
	watchdog_extended();

	if (wdte_failures >= WDTE_FAILURES_THRESHOLD) {
		ERROR("watchdog is restarting the device!\n");
		uloop_timeout_cancel(t); // don't shedule the next time == watchdog will restart the device
	} else {
		watchdog_ping();
		uloop_timeout_set(t, wdt_frequency * 1000);
	}
}

void watchdog_set_stopped(bool val)
{
	if (val)
		uloop_timeout_cancel(&wdt_timeout);
	else
		watchdog_timeout_cb(&wdt_timeout);
}

bool watchdog_get_stopped(void)
{
	return !wdt_timeout.pending;
}

int watchdog_timeout(int timeout)
{
	if (wdt_fd < 0)
		return 0;

	if (timeout) {
		DEBUG(4, "Set watchdog timeout: %ds\n", timeout);
		ioctl(wdt_fd, WDIOC_SETTIMEOUT, &timeout);
	}
	ioctl(wdt_fd, WDIOC_GETTIMEOUT, &timeout);

	return timeout;
}

int watchdog_frequency(int frequency)
{
	if (wdt_fd < 0)
		return 0;

	if (frequency) {
		DEBUG(4, "Set watchdog frequency: %ds\n", frequency);
		wdt_frequency = frequency;
	}

	return wdt_frequency;
}

char* watchdog_fd(void)
{
	static char fd_buf[3];

	if (wdt_fd < 0)
		return NULL;
	snprintf(fd_buf, sizeof(fd_buf), "%d", wdt_fd);

	return fd_buf;
}

void watchdog_init(int preinit)
{
	char *env = getenv("WDTFD");

	if (wdt_fd >= 0)
		return;

	wdt_timeout.cb = watchdog_timeout_cb;
	if (env) {
		DEBUG(2, "Watchdog handover: fd=%s\n", env);
		wdt_fd = atoi(env);
		unsetenv("WDTFD");
	} else {
		wdt_fd = open("/dev/watchdog", O_WRONLY);
	}

	if (wdt_fd < 0)
		return;

	if (!preinit)
		fcntl(wdt_fd, F_SETFD, fcntl(wdt_fd, F_GETFD) | FD_CLOEXEC);

	LOG("- watchdog -\n");
	watchdog_timeout(30);
	watchdog_timeout_cb(&wdt_timeout);

	DEBUG(4, "Opened watchdog with timeout %ds\n", watchdog_timeout(0));
}


void watchdog_no_cloexec(void)
{
	if (wdt_fd < 0)
		return;

	fcntl(wdt_fd, F_SETFD, fcntl(wdt_fd, F_GETFD) & ~FD_CLOEXEC);
}
