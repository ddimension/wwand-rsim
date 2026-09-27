/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "log.h"
#include "meta.h"
#include "phoenix.h"

/*
 * The Phoenix transport over a kernel tty (a USB-serial bridge with its
 * kernel driver, or a real UART).
 *
 * Test hook: RSIM_TEST_MCTRL=<path> makes set_modem write "RTS=0|1" /
 * "DTR=0|1" lines to that file instead of driving the modem-control lines,
 * and a failing tcsetattr is tolerated, because a pseudo-terminal has no
 * modem-control lines and the simulated card needs to see the reset pulse.
 * It is read only here and set only by tests/test_e2e.py; a real reader is
 * never opened with it.
 */

struct phx_tty {
	struct phx_io io;
	int fd;
	int mctrl_fd;		/* test hook, -1 on real hardware */
	struct termios tio;
	const char *dev;
	bool warned;
};

static speed_t nearest_speed(unsigned baud)
{
	static const struct { unsigned rate; speed_t code; } tab[] = {
		{ 4800, B4800 }, { 9600, B9600 }, { 19200, B19200 },
		{ 38400, B38400 }, { 57600, B57600 }, { 115200, B115200 },
	};
	size_t i, best = 0;

	for (i = 1; i < sizeof(tab) / sizeof(tab[0]); i++)
		if (abs((int)tab[i].rate - (int)baud) < abs((int)tab[best].rate - (int)baud))
			best = i;
	return tab[best].code;
}

static int tty_set_line(struct phx_io *io, unsigned baud, enum phx_parity parity,
			bool drop_errors)
{
	struct phx_tty *t = (struct phx_tty *)io;
	struct termios *tio = &t->tio;
	int r;

	tio->c_cflag &= ~(tcflag_t)(PARENB | PARODD);
	if (parity != PHX_PARITY_NONE)
		tio->c_cflag |= PARENB;
	if (parity == PHX_PARITY_ODD)
		tio->c_cflag |= PARODD;
	tio->c_iflag &= ~(tcflag_t)(INPCK | PARMRK | IGNPAR);
	if (drop_errors)
		tio->c_iflag |= INPCK | IGNPAR;
	cfsetispeed(tio, nearest_speed(baud));
	cfsetospeed(tio, nearest_speed(baud));
	if (tcsetattr(t->fd, TCSANOW, tio) < 0 && t->mctrl_fd < 0)
		return -errno;
	/* re-applied every time: whether a libc tcsetattr keeps a BOTHER
	 * rate or rewrites it from its own speed fields is not something to
	 * depend on */
	r = phoenix_set_baud(t->fd, baud);
	if (r < 0 && !t->warned) {
		log_warn("%s: exact %u baud not settable (%s), using nearest standard rate",
			 t->dev, baud, strerror(-r));
		t->warned = true;
	}
	return 0;
}

static int tty_one_line(struct phx_tty *t, const char *name, int bit, int level)
{
	if (level < 0)
		return 0;
	if (t->mctrl_fd >= 0) {
		dprintf(t->mctrl_fd, "%s=%d\n", name, level ? 1 : 0);
		return 0;
	}
	if (ioctl(t->fd, level ? TIOCMBIS : TIOCMBIC, &bit) < 0)
		return -errno;
	return 0;
}

static int tty_set_modem(struct phx_io *io, int rts, int dtr)
{
	struct phx_tty *t = (struct phx_tty *)io;
	int r = tty_one_line(t, "RTS", TIOCM_RTS, rts);

	return r ? r : tty_one_line(t, "DTR", TIOCM_DTR, dtr);
}

static int tty_get_modem(struct phx_io *io, int *cts, int *dsr, int *cd)
{
	struct phx_tty *t = (struct phx_tty *)io;
	int bits;

	if (ioctl(t->fd, TIOCMGET, &bits) < 0)
		return -errno;
	*cts = !!(bits & TIOCM_CTS);
	*dsr = !!(bits & TIOCM_DSR);
	*cd = !!(bits & TIOCM_CD);
	return 0;
}

static int tty_write(struct phx_io *io, const uint8_t *buf, size_t len)
{
	struct phx_tty *t = (struct phx_tty *)io;
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(t->fd, buf + off, len - off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n ? -errno : -EIO;
		off += (size_t)n;
	}
	return 0;
}

static int tty_read(struct phx_io *io, uint8_t *buf, size_t len, unsigned timeout_ms)
{
	struct phx_tty *t = (struct phx_tty *)io;
	struct pollfd pfd = { .fd = t->fd, .events = POLLIN };
	ssize_t n;
	int r;

	do
		r = poll(&pfd, 1, (int)timeout_ms);
	while (r < 0 && errno == EINTR);
	if (r < 0)
		return -errno;
	if (r == 0)
		return 0;
	/* a USB-serial adapter pulled out hangs up its tty */
	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
		return -ENODEV;
	n = read(t->fd, buf, len);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return 0;
	if (n < 0)
		return -errno;
	return n ? (int)n : -ENODEV;
}

static void tty_flush_input(struct phx_io *io)
{
	struct phx_tty *t = (struct phx_tty *)io;

	tcflush(t->fd, TCIFLUSH);
}

static void tty_close(struct phx_io *io)
{
	struct phx_tty *t = (struct phx_tty *)io;

	close(t->fd);
	if (t->mctrl_fd >= 0)
		close(t->mctrl_fd);
	free(t);
}

static void tty_info(struct phx_io *io, struct jw *w)
{
	struct phx_tty *t = (struct phx_tty *)io;
	struct tty_meta m;

	if (!meta_tty_read("", t->dev, &m))
		meta_tty_write(w, &m);
}

static const struct phx_io_ops tty_ops = {
	.set_line = tty_set_line,
	.set_modem = tty_set_modem,
	.get_modem = tty_get_modem,
	.write = tty_write,
	.read = tty_read,
	.flush_input = tty_flush_input,
	.close = tty_close,
	.info = tty_info,
};

struct phx_io *phx_tty_open(const char *dev)
{
	struct phx_tty *t = calloc(1, sizeof(*t));
	const char *mctrl = getenv("RSIM_TEST_MCTRL");
	int fl;

	if (!t)
		return NULL;
	t->io.ops = &tty_ops;
	t->dev = dev;
	t->mctrl_fd = -1;
	/* O_NONBLOCK only so that open does not wait for carrier; the
	 * descriptor is used blocking with poll() and VMIN=VTIME=0 */
	t->fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (t->fd < 0) {
		log_err("%s: %s", dev, strerror(errno));
		free(t);
		return NULL;
	}
	fl = fcntl(t->fd, F_GETFL);
	if (fl >= 0)
		fcntl(t->fd, F_SETFL, fl & ~O_NONBLOCK);
	if (mctrl) {
		t->mctrl_fd = open(mctrl, O_WRONLY | O_CLOEXEC);
		if (t->mctrl_fd < 0) {
			log_err("%s: %s", mctrl, strerror(errno));
			goto fail;
		}
		log_warn("test mode: modem-control lines go to %s", mctrl);
	}
	if (tcgetattr(t->fd, &t->tio) < 0) {
		log_err("%s: not a tty: %s", dev, strerror(errno));
		goto fail;
	}
	cfmakeraw(&t->tio);
	/* ISO/IEC 7816-3:2006 §7.1/§7.2: 8 data bits, a parity bit, and a
	 * guard time of 2 etu, which two stop bits provide */
	t->tio.c_cflag &= ~(tcflag_t)(CSIZE | CRTSCTS | HUPCL);
	t->tio.c_cflag |= CS8 | CSTOPB | CREAD | CLOCAL;
	t->tio.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);
	t->tio.c_cc[VMIN] = 0;
	t->tio.c_cc[VTIME] = 0;
	return &t->io;

fail:
	if (t->mctrl_fd >= 0)
		close(t->mctrl_fd);
	close(t->fd);
	free(t);
	return NULL;
}
