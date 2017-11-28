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

#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <config.h>
#include <ntpd.h>

#include "sock.h"

#define SOCK_MAGIC 0x534f434b

/* Copied from chrony-3.2/refclock_sock.c */
struct sock_sample {
	/* Time of the measurement (system time) */
	struct timeval tv;

	/* Offset between the true time and the system time (in seconds) */
	double offset;

	/* Non-zero if the sample is from a PPS signal, i.e. another source
	   is needed to obtain seconds */
	int pulse;

	/* 0 - normal, 1 - insert leap second, 2 - delete leap second */
	int leap;

	/* Padding, ignored */
	int _pad;

	/* Protocol identifier (0x534f434b) */
	int magic;
};

int sock_open(const char *path) {
	struct sockaddr_un sun;
	int fd;

	sun.sun_family = AF_UNIX;
	if (snprintf(sun.sun_path, sizeof sun.sun_path, "%s", path) >=
	    sizeof (sun.sun_path))
		return -1;

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	if (connect(fd, (struct sockaddr *)&sun, sizeof (sun)) < 0) {
		fprintf(stderr, "Could not connect to %s: %m\n", sun.sun_path);
		close(fd);
		return -1;
	}

	DPRINTF(2, ("sock connected to %s\n", sun.sun_path));
	return fd;
}

int sock_send_sample(int fd, struct timeval *tv, double offset, int leap) {
	struct sock_sample sample;

	sample.tv = *tv;
	sample.offset = offset;
	sample.pulse = 0;
	sample.leap = leap;
	sample._pad = 0;
	sample.magic = SOCK_MAGIC;

	if (send(fd, &sample, sizeof sample, 0) != sizeof sample) {
		fprintf(stderr, "Could not send sample: %m\n");
		return 0;
	}

	return 1;
}

int sock_close(int fd) {
	return !close(fd);
}
