VERSION=0.5
NAME=ntp-refclock

CC=gcc
CFLAGS=-O2 -g -Wall
DEFAULT_USER=nobody
DEFAULT_ROOTDIR=/var/empty

prefix = /usr/local
sbindir = $(prefix)/sbin
mandir = $(prefix)/share/man
man8dir = $(mandir)/man8

NTP_SRC=ntp
NTP_BUILD=$(NTP_SRC)

NTP_OBJS=$(NTP_BUILD)/ntpd/ntp_refclock.o $(NTP_BUILD)/ntpd/refclock_*.o
NTP_LDFLAGS=-lm -L$(NTP_BUILD)/libntp -lntp -L$(NTP_BUILD)/ntpd -lntpd \
	  $(shell test -e $(NTP_BUILD)/libparse/libparse.a && \
		  echo -L$(NTP_BUILD)/libparse -lparse)
OBJS=main.o refclock.o sock.o stubs.o
EXTRA_FILES=refclock_names.h COPYRIGHT

NTP_RELEASE:=$(shell awk -F '[. p"]' \
		     '/ VERSION /{ printf "%d%02d%02d%02d", $$4, $$5, $$6, $$7 }' \
		     $(NTP_SRC)/config.h)

CPPFLAGS=-I$(NTP_BUILD) -I$(NTP_SRC)/include -I$(NTP_SRC)/lib/isc/include \
	 -I$(NTP_SRC)/lib/isc/unix/include -I$(NTP_SRC)/libntp/lib/isc/include \
	 -I$(NTP_SRC)/libntp/lib/isc/unix/include \
	 -DPROGRAM_NAME=\"$(NAME)\" -DPROGRAM_VERSION=\"$(VERSION)\" \
	 -DNTP_RELEASE=$(NTP_RELEASE) \
	 -DDEFAULT_USER=\"$(DEFAULT_USER)\" \
	 -DDEFAULT_ROOTDIR=\"$(DEFAULT_ROOTDIR)\"

all: $(NAME) COPYRIGHT

$(NAME): $(OBJS) $(NTP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(NTP_LDFLAGS) $(LDFLAGS)

refclock.c: refclock.h refclock_names.h

refclock_names.h: $(NTP_SRC)/include/ntp.h
	@echo Generating refclock_names.h
	@echo 'struct { unsigned char type; const char *name;' > $@
	@echo '} static refclock_names[] = {' >> $@
	@grep '#define.*REFCLK_' $^ | \
	  sed 's/[^_]*REFCLK_\([^ \t]*\)[ \t]*\([0-9]*\).*/\t{\2, "\1"},/' >> $@
	@echo '};' >> $@

COPYRIGHT: $(NTP_SRC)/COPYRIGHT
	cp -p $^ $@

install:
	mkdir -p $(sbindir) $(man8dir)
	install $(NAME) $(sbindir)
	install -p -m 644 $(NAME).8 $(man8dir)

clean:
	-rm -rf $(OBJS) $(NAME) $(EXTRA_FILES)
