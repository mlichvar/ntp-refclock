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
#include <poll.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <config.h>
#include <ntpd.h>
#include <ntp_net.h>
#include <timevalops.h>

#include "refclock.h"
#include "stubs.h"

#include "refclock_names.h"

/*
 * Design overview of ntpd refclock drivers:
 * - drivers provide:
 *   - init(): called by init_refclock()
 *   - start(): called by refclock_newpeer()
 *   - timer(): called by refclock_timer() once per second
 *   - poll(): called by refclock_transmit() once per poll
 *   - control(): called by refclock_control() to set/get variables
 *   - shutdown(): called by refclock_unpeer()
 * - drivers may set:
 *   - io.fd and io.clock_recv() or io.io_input() to read data from a fd
 *   - action() and nextaction to have a programmable timer
 * - drivers call from timer(), poll(), io.*(), action():
 *   - refclock_process*() or refclock_pps() to push a sample to the filter
 *   - refclock_receive() to process accumulated samples
 */

struct refclock_context {
	struct peer peer;
	struct timespec next_timer;
	int prev_coderecv;
};

static struct refclock_context refclock_context;

static int receive_data(int fd, struct peer *peer) {
	struct refclockio *io;
	struct recvbuf *rbuf;
	size_t buf_len;
	ssize_t len;
	l_fp recv_time;

	get_systime(&recv_time);

	rbuf = get_free_recv_buffer(
#if NTP_RELEASE >= 4020815
				    TRUE
#endif
				   );
	if (!rbuf) {
		fprintf(stderr, "Could not get recv buffer\n");
		return 0;
	}

	io = &peer->procptr->io;

	buf_len = io->datalen;
	if (buf_len == 0 || buf_len > sizeof rbuf->recv_buffer)
		buf_len = sizeof rbuf->recv_buffer;

	len = read(fd, &rbuf->recv_buffer, buf_len);

	if (len <= 0) {
		freerecvbuf(rbuf);

		if (len < 0) {
			if (errno == EAGAIN)
				return 1;
			fprintf(stderr, "read() failed: %m\n");
		} else {
			fprintf(stderr, "No more data to read\n");
		}

		return 0;
	}

	rbuf->fd = fd;
	rbuf->recv_length = len;
	rbuf->recv_peer = peer;
	rbuf->recv_time = recv_time;

	assert(!has_full_recv_buffer());

	if (!io->io_input || io->io_input(rbuf)) {
		if (io->clock_recv)
			io->clock_recv(rbuf);
	}

	freerecvbuf(rbuf);

	/* Process buffers added by the driver */
	while ((rbuf = get_full_recv_buffer())) {
		if (io->clock_recv)
			io->clock_recv(rbuf);
		freerecvbuf(rbuf);
	}

	return 1;
}

int refclock_start(struct refclock_config *conf) {
	struct refclock_context *refclock = &refclock_context;
	struct peer *peer = &refclock->peer;

	if (conf->type == 0 || conf->type >= num_refclock_conf) {
		fprintf(stderr, "Invalid refclock type %u\n", conf->type);
		return 0;
	} else if (refclock_conf[conf->type]->clock_start == noentry) {
		fprintf(stderr, "Missing driver for refclock type %u\n",
			conf->type);
		return 0;
	}

	memset(peer, 0, sizeof *peer);

	AF(&peer->srcadr) = AF_INET;
	SET_ADDR4(&peer->srcadr, REFCLOCK_ADDR | conf->type << 8 | conf->unit);
	peer->ttl = conf->mode;
	peer->hpoll = peer->minpoll = peer->maxpoll = 6;

	if (!refclock_newpeer(peer))
		return 0;

	refclock_control(&peer->srcadr, &conf->stat, NULL);

	refclock->next_timer.tv_sec = 0;
	refclock->next_timer.tv_nsec = 0;

	return 1;
}

int refclock_run(void) {
	struct refclock_context *refclock = &refclock_context;
	struct peer *peer = &refclock->peer;
	struct refclockproc *proc = peer->procptr;
	struct timespec ts_now;
	struct pollfd fd;
	int ret, timeout;

	if (clock_gettime(CLOCK_MONOTONIC, &ts_now)) {
		fprintf(stderr, "clock_gettime() failed: %m\n");
		return 0;
	}

	if (ts_now.tv_sec > refclock->next_timer.tv_sec ||
	    (ts_now.tv_sec == refclock->next_timer.tv_sec &&
	     ts_now.tv_nsec >= refclock->next_timer.tv_nsec))
		refclock->next_timer = ts_now;

	timeout = (refclock->next_timer.tv_sec - ts_now.tv_sec) * 1000 +
		(refclock->next_timer.tv_nsec - ts_now.tv_nsec) / 1000000;

	/* Some drivers change io.fd after start */
	fd.fd = proc->io.fd;
	fd.events = POLLIN | POLLPRI;

	refclock->prev_coderecv = proc->coderecv;

	ret = poll(&fd, 1, timeout);

	if (ret < 0) {
		if (errno == EINTR)
			return 1;
		fprintf(stderr, "poll() failed: %m\n");
		return 0;
	} else if (ret > 0) {
		assert(fd.revents);
		if (!receive_data(fd.fd, peer))
			return 0;
	} else {
		current_time++;
		refclock->next_timer.tv_sec++;

		sys_leap_update();

		refclock_timer(peer);

		if (peer->nextdate <= current_time)
			refclock_transmit(peer);
	}

	return 1;
}

void refclock_stop(void) {
	struct refclock_context *refclock = &refclock_context;

	refclock_unpeer(&refclock->peer);
}

int refclock_get_raw_sample(struct refclock_sample *sample) {
	struct refclock_context *refclock = &refclock_context;
	struct refclockproc *proc = refclock->peer.procptr;

	/* Check if a new offset was pushed to the filter */
	if (refclock->prev_coderecv == proc->coderecv)
		return 0;

	sample->time = lfp_stamp_to_tval(proc->lastrec, NULL);
	sample->offset = proc->filter[proc->coderecv];
	sample->leap = proc->leap;

	return 1;
}

void refclock_print_drivers(void) {
	const char *name;
	int i, j;

	for (i = 0; i < num_refclock_conf; i++) {
		if (refclock_conf[i]->clock_start == noentry)
			continue;
		for (j = 0, name = "?"; j < sizeof refclock_names /
		     sizeof refclock_names[0]; j++) {
			if (i == refclock_names[j].type) {
				name = refclock_names[j].name;
				break;
			}
		}
		printf("127.127.%d.*\t%s\n", i, name);
	}
}

struct peer *refclock_get_peer(void) {
	struct refclock_context *refclock = &refclock_context;

	return &refclock->peer;
}
