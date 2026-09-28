#define _GNU_SOURCE
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <pty.h>
#include <stdio.h>
#include <pthread.h>

/* openpty over /dev/ptmx, unlockpt and ptsname_r */
int openpty(int *pm, int *ps, char *name, const struct termios *tio, const struct winsize *ws)
{
	int m, s, cs;
	char buf[32];

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);
	m = open("/dev/ptmx", O_RDWR|O_NOCTTY);
	if (m < 0) goto fail0;
	if (grantpt(m) || unlockpt(m) || ptsname_r(m, buf, sizeof buf)) goto fail;
	if ((s = open(buf, O_RDWR|O_NOCTTY)) < 0) goto fail;
	if (!name) name = buf;
	else snprintf(name, sizeof buf, "%s", buf);
	if (tio) tcsetattr(s, TCSANOW, tio);
	if (ws) ioctl(s, TIOCSWINSZ, ws);
	*pm = m;
	*ps = s;
	pthread_setcancelstate(cs, 0);
	return 0;
fail:
	close(m);
fail0:
	pthread_setcancelstate(cs, 0);
	return -1;
}
