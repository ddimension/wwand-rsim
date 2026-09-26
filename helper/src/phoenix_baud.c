/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 André Valentin <avalentin@marcant.net>
 */
#include <errno.h>
#include <sys/ioctl.h>
/* the kernel's own definitions: musl's <sys/ioctl.h> has no TCGETS2, and
 * glibc's only by way of the same <asm/ioctls.h> */
#include <asm/ioctls.h>
#include <asm/termbits.h>

#include "phoenix.h"

/* Card rates are clock/372 (9622 baud at 3.579545 MHz), which no Bxxxx
 * constant matches; the nearest one (9600) is 0.2 % off and works, but at
 * 6 MHz (16129 baud) the nearest (19200) is 19 % off and does not. Linux
 * takes an exact rate only through termios2 with BOTHER. */
int phoenix_set_baud(int fd, unsigned baud)
{
	struct termios2 t;

	if (ioctl(fd, TCGETS2, &t) < 0)
		return -errno;
	t.c_cflag &= ~(tcflag_t)CBAUD;
	t.c_cflag |= BOTHER;
	t.c_ispeed = baud;
	t.c_ospeed = baud;
	/* input speed follows output: clear the separate input rate bits */
	t.c_cflag &= ~(tcflag_t)(CBAUD << IBSHIFT);
	if (ioctl(fd, TCSETS2, &t) < 0)
		return -errno;
	return 0;
}
