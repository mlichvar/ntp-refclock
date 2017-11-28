/*
 * Copyright (C) 2017  Miroslav Lichvar <mlichvar@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <assert.h>
#include <stdarg.h>
#include <sys/timex.h>
#include <time.h>

#include <config.h>
#include <ntpd.h>

#include "refclock.h"
#include "stubs.h"

char const *progname;
u_long current_time;
u_char sys_leap;
char *sys_phone[10];

/* Used by IRIG and WWV, but does not seem to be configurable in ntpd */
double clock_codec = 0.0;

/* ACTS backup is always active */
struct peer *sys_peer = NULL;

s_char sys_precision;
double last_offset = 0.0;
double sys_mindisp = 1e-3;
volatile u_long packets_received;
int hardpps_enable;

static FILE *clockstats_file = NULL;

/* Called by refclock_control() */
struct peer *findexistingpeer(sockaddr_u *addr, const char *hostname,
			      struct peer *start_peer, int mode,
			      u_char cast_flags) {
	struct peer *peer;

	peer = refclock_get_peer();
	if (!peer)
		return NULL;

	if (SOCK_EQ(addr, &peer->srcadr))
		return peer;

	return NULL;
}

/* Called by refclock_receive() */
void clock_filter(struct peer *peer, double sample_offset,
		  double sample_delay, double sample_disp) {
	DPRINTF(2, ("clock_filter: offset %f delay %f disp %f\n",
		    sample_offset, sample_delay, sample_disp));
}

/* Called by refclock_transmit() */
void poll_update(struct peer *peer, u_char mpoll) {
	peer->nextdate += 1U << peer->hpoll;
}

int io_addclock(struct refclockio *rio) {
	return 1;
}

void io_closeclock(struct refclockio *rio) {
}

int mprintf_clock_stats(sockaddr_u *addr, const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	record_clock_stats(addr, buf);

	return ret;
}

int mprintf_event(int evcode, struct peer *p, const char *fmt, ...) {
	return 0;
}

void record_clock_stats(sockaddr_u *addr, const char *text) {
	struct timespec ts;

	if (!clockstats_file)
		return;

	if (clock_gettime(CLOCK_REALTIME, &ts))
		return;

	fprintf(clockstats_file, "%d %.3f %s %s\n",
		(int)(ts.tv_sec / 86400 + 40587),
		ts.tv_sec % 86400 + ts.tv_nsec / 1e9, stoa(addr), text);
	fflush(clockstats_file);
}

void report_event(int err, struct peer *peer, const char *str) {
}

char *add_var(struct ctl_var **kv, u_long size, u_short def) {
	assert(0);
	return NULL;
}

void set_var(struct ctl_var **kv, const char *data, u_long size, u_short def) {
	assert(0);
}

void free_varlist(struct ctl_var *kv) {
	assert(!kv);
}

const char *get_ext_sys_var(const char *tag) {
	return NULL;
}

int clockstats_open(const char *path) {
	if (strcmp(path, "-") == 0) {
		clockstats_file = stdout;
	} else {
		clockstats_file = fopen(path, "a");
		if (!clockstats_file) {
			fprintf(stderr, "Could not open %s: %m\n", path);
			return 0;
		}
	}

	return 1;
}

void clockstats_close(void) {
	if (clockstats_file && clockstats_file != stdout)
		fclose(clockstats_file);
}


/* Set the sys_leap variable according to the system clock status to enable
   the drivers to use PPS */
void sys_leap_update(void) {
	struct ntptimeval ntv;
	int state, sys_leap;

	state = ntp_gettime(&ntv);

	if (state < 0 || ntv.maxerror > 400000) {
		sys_leap = LEAP_NOTINSYNC;
	} else {
		switch (state) {
		case TIME_INS:
			sys_leap = LEAP_ADDSECOND;
			break;
		case TIME_DEL:
			sys_leap = LEAP_DELSECOND;
			break;
		default:
			sys_leap = LEAP_NOWARNING;
		}
	}

	DPRINTF(2, ("sys_leap_update: leap %d\n", sys_leap));
}

int sys_phone_add(char *number) {
	int i;

	/* The last entry is always NULL */
	for (i = 0; i < sizeof sys_phone / sizeof sys_phone[0] - 1; i++) {
		if (!sys_phone[i]) {
			sys_phone[i] = number;
			return 1;
		}
	}

	fprintf(stderr, "Too many phone numbers\n");

	return 0;
}
