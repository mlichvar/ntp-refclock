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

#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include <config.h>
#include <ntpd.h>
#include <recvbuff.h>

#include "refclock.h"
#include "sock.h"
#include "stubs.h"

static int quit_signal;

static int drop_root_privileges(const char *user, const char *dir) {
	struct passwd *pw;

	if ((pw = getpwnam(user)) == NULL) {
		fprintf(stderr,
			"Could not get passwd entry for user %s\n", user);
		return 0;
	}

	if (chroot(dir)) {
		fprintf(stderr,
			"Could not change root directory to %s: %m\n", dir);
		return 0;
	}

	if (chdir("/")) {
		fprintf(stderr, "chdir(/) failed: %m\n");
		return 0;
	}

	if (setgroups(0, NULL)) {
		fprintf(stderr, "setgroups() failed: %m\n");
		return 0;
	}

	if (setgid(pw->pw_gid)) {
		fprintf(stderr, "setgid(%d) failed: %m\n", pw->pw_gid);
		return 0;
	}

	if (setuid(pw->pw_uid)) {
		fprintf(stderr, "setuid(%d) failed: %m\n", pw->pw_uid);
		return 0;
	}

	return 1;
}

static void handle_signal(int signal) {
	quit_signal = signal;
}

static int set_signal_handler(void) {
	struct sigaction sa;
	int i, signals[] = {SIGINT, SIGTERM, SIGQUIT, SIGHUP};

	sa.sa_handler = handle_signal;
	sa.sa_flags = SA_RESTART;

	if (sigemptyset(&sa.sa_mask) < 0) {
		fprintf(stderr, "sigemptyset() failed: %m\n");
		return 0;
	}

	for (i = 0; i < sizeof signals / sizeof signals[0]; i++) {
		if (sigaction(signals[i], &sa, NULL) < 0) {
			fprintf(stderr, "sigaction() failed: %m\n");
			return 0;
		}
	}

	return 1;
}

static void print_help(const char *name) {
	fprintf(stderr,
		"Usage: %s [OPTION]... 127.127.TYPE.UNIT [DRIVER-OPTION]...\n"
		"\nDriver options:\n"
		"  mode MODE\n"
		"  time1 FUDGE\n"
		"  time2 FUDGE\n"
		"  flag1 0|1\n"
		"  flag2 0|1\n"
		"  flag3 0|1\n"
		"  flag4 0|1\n"
		"\nOptions:\n"
		"  -s SOCKET\tSend samples to chrony refclock SOCKET\n"
		"  -u USER\tRun as USER (default: " DEFAULT_USER ")\n"
		"  -r DIR\tChange root directory to DIR (default: " DEFAULT_ROOTDIR ")\n"
		"  -c FILE\tWrite reference clock statistics to FILE\n"
		"  -p NUMBER\tSpecify phone NUMBER for modem drivers\n"
		"  -d\t\tIncrease debug level\n"
		"  -l\t\tPrint available drivers\n"
		"  -v\t\tPrint version\n"
		"  -h\t\tPrint usage\n",
		name);
}

static int parse_refclock_args(int argc, char **argv,
			       struct refclock_config *conf) {
	char *name, *val;
	int i;

	if (argc < 1 ||
	    sscanf(argv[0], "127.127.%u.%u", &conf->type, &conf->unit) != 2) {
		fprintf(stderr, "Could not parse refclock address\n");
		return 0;
	}

	for (i = 1; i < argc; i += 2) {
		name = argv[i];

		if (i + 1 >= argc) {
			fprintf(stderr, "Missing argument for %s\n", name);
			return 0;
		}

		val = argv[i + 1];

		if (!strcmp(name, "mode")) {
			conf->mode = atoi(val);
		} else if (!strcmp(name, "time1")) {
			conf->stat.fudgetime1 = atof(val);
			conf->stat.haveflags |= CLK_HAVETIME1;
		} else if (!strcmp(name, "time2")) {
			conf->stat.fudgetime2 = atof(val);
			conf->stat.haveflags |= CLK_HAVETIME2;
		} else if (!strcmp(name, "flag1")) {
			conf->stat.flags |= atoi(val) ? CLK_FLAG1 : 0;
			conf->stat.haveflags |= CLK_HAVEFLAG1;
		} else if (!strcmp(name, "flag2")) {
			conf->stat.flags |= atoi(val) ? CLK_FLAG2 : 0;
			conf->stat.haveflags |= CLK_HAVEFLAG2;
		} else if (!strcmp(name, "flag3")) {
			conf->stat.flags |= atoi(val) ? CLK_FLAG3 : 0;
			conf->stat.haveflags |= CLK_HAVEFLAG3;
		} else if (!strcmp(name, "flag4")) {
			conf->stat.flags |= atoi(val) ? CLK_FLAG4 : 0;
			conf->stat.haveflags |= CLK_HAVEFLAG4;
		} else {
			fprintf(stderr, "Unknown refclock option %s\n", name);
			return 0;
		}
	}

	return 1;
}

static void print_sample(struct refclock_sample *sample) {
	printf("SAMPLE: time=%lld.%06u offset=%+.9f leap=%d\n",
	       (long long)sample->time.tv_sec,
	       (unsigned int)sample->time.tv_usec,
	       sample->offset, sample->leap);
}

int main(int argc, char **argv) {
	struct refclock_config conf;
	struct refclock_sample sample;
	const char *user, *dir;
	int opt, sock;

	user = DEFAULT_USER;
	dir = DEFAULT_ROOTDIR;
	sock = -1;

	memset(&conf, 0, sizeof conf);

	while ((opt = getopt(argc, argv, "+c:dlp:r:s:u:vh")) != -1) {
		switch (opt) {
		case 'c':
			if (!clockstats_open(optarg))
				return 1;
			break;
		case 'd':
			debug++;
			break;
		case 'l':
			refclock_print_drivers();
			return 0;
		case 'p':
			if (!sys_phone_add(optarg))
				return 1;
			break;
		case 'r':
			dir = optarg;
			break;
		case 's':
			sock = sock_open(optarg);
			if (sock < 0)
				return 1;
			break;
		case 'u':
			user = optarg;
			break;
		case 'v':
			printf("%s %s (ntp-%s)\n",
			       PROGRAM_NAME, PROGRAM_VERSION, VERSION);
			return 0;
		default:
			print_help(argv[0]);
			return opt != 'h';
		}
	}

	if (optind >= argc) {
		print_help(argv[0]);
		return 1;
	}

	if (!parse_refclock_args(argc - optind, argv + optind, &conf))
		return 1;

	progname = argv[0];

	init_logging(progname, 0, 0);
	syslogit = FALSE;
	msyslog_term = TRUE;
	msyslog_include_timestamp = FALSE;
	msyslog_term_pid = FALSE;
	init_lib();
	init_refclock();
	init_recvbuff(4);

	if (!set_signal_handler())
		return 1;

	if (!refclock_start(&conf))
		return 1;

	if (geteuid() == 0 && !drop_root_privileges(user, dir))
		return 1;

	for (quit_signal = 0; !quit_signal; ) {
		if (!refclock_run())
			break;

		if (!refclock_get_raw_sample(&sample))
			continue;

		if (sock < 0 || debug > 0)
			print_sample(&sample);

		if (sock >= 0 && !sock_send_sample(sock, &sample.time,
						   sample.offset, sample.leap))
			break;
	}

	refclock_stop();
	clockstats_close();

	if (sock >= 0)
		sock_close(sock);

	if (!quit_signal) {
		fprintf(stderr, "Exiting on error\n");
		return 2;
	}

	fprintf(stderr, "Exiting on signal %d\n", quit_signal);

	return 0;
}
