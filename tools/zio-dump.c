/*
 * Trivial utility that reports data from ZIO input channels
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>

#include <linux/zio.h>
#include <linux/zio-user.h>

unsigned char buf[1024*1024];
char *prgname;

void read_channel(int cfd, int dfd, FILE *log)
{
	int err = 0;
	struct zio_control ctrl;
	int i, j;

	i = read(cfd, &ctrl, sizeof(ctrl));
	switch (i) {
	case -1:
		fprintf(stderr, "%s: control read: %s\n",
			prgname, strerror(errno));
		exit(1);
	case 0:
		fprintf(stderr, "%s: control read: unexpected EOF\n",
			prgname);
		exit(1);
	default:
		fprintf(stderr, "%s: ctrl read: %i bytes (expected %i)\n",
			prgname, i, sizeof(ctrl));
		/* continue anyways */
	case sizeof(ctrl):
		break; /* ok */
	}

	/* Fail badly if the version is not the right one */
	if (ctrl.major_version != ZIO_MAJOR_VERSION)
		err++;
	if (ZIO_MAJOR_VERSION == 0 && ctrl.minor_version != ZIO_MINOR_VERSION)
		err++;
	if (err) {
		fprintf(stderr, "%s: kernel has zio %i.%i, "
			"but I'm compiled for %i.%i\n", prgname,
			ctrl.major_version, ctrl.minor_version,
			ZIO_MAJOR_VERSION, ZIO_MINOR_VERSION);
		exit(1);
	}
	if (ctrl.minor_version != ZIO_MINOR_VERSION) {
		static int warned;

		if (!warned++)
			fprintf(stderr, "%s: warning: minor version mismatch\n",
				prgname);
	}

	printf("Ctrl: version %i.%i, trigger %.16s, dev %.16s, "
	       "cset %i, chan %i\n",
	       ctrl.major_version, ctrl.minor_version,
	       ctrl.triggername, ctrl.devname, ctrl.cset_i,
	       ctrl.chan_i);
	printf("Ctrl: seq %i, n %i, size %i, bits %i, "
	       "flags %08x (%s)\n",
	       ctrl.seq_num,
	       ctrl.nsamples,
	       ctrl.ssize,
	       ctrl.sbits,
	       ctrl.flags,
	       ctrl.flags & ZIO_CONTROL_LITTLE_ENDIAN
	       ? "little-endian" :
	       ctrl.flags & ZIO_CONTROL_BIG_ENDIAN
	       ? "big-endian" : "unknown-endian");
	printf("Ctrl: stamp %lli.%09lli (%lli)\n",
	       (long long)ctrl.tstamp.secs,
	       (long long)ctrl.tstamp.ticks,
	       (long long)ctrl.tstamp.bins);
	/* FIXME: some control information is missing */

	i = read(dfd, buf, sizeof(buf));
	if (i < 0) {
		fprintf(stderr, "%s: data read: %s\n",
			prgname, strerror(errno));
		return; /* next ctrl, let's see... */
	}
	if (!i) {
		fprintf(stderr, "%s: data read: unexpected EOF\n", prgname);
		return;
	}
	if (i != ctrl.nsamples * ctrl.ssize) {
		if (i == sizeof(buf)) {
			fprintf(stderr, "%s: buffer too small: "
				"please fix me and recompile\n", prgname);
			/* FIXME: empty the data channel */
		} else {
			fprintf(stderr, "%s: ctrl: read %i bytes "
				"(exp %i)\n", prgname, i,
				ctrl.nsamples * ctrl.ssize);
		}
		/* continue anyways */
	}
	fwrite(buf, 1, i, log);

	/* report data to stdout */
	for (j = 0; j < i; j++) {
		if (!(j & 0xf))
			printf("Data:");
		printf(" %02x", buf[j]);
		if ((j & 0xf) == 0xf || j == i - 1)
			putchar('\n');
	}
	putchar('\n');
}

int main(int argc, char **argv)
{
	FILE *f;
	char *outfname;
	int *cfd; /* control file descriptors */
	int *dfd; /* data file descriptors */
	fd_set control_set, ready_set;
	int i, j, maxfd, ndev;

	if (argc < 3 || (argc & 1) != 1) {
		fprintf(stderr, "%s: Wrong number of arguments\n"
			"Use: \"%s <ctrl-file> <data-file> [...]\"\n",
			argv[0], argv[0]);
		exit(1);
	}
	prgname = argv[0];
	cfd = malloc(argc / 2 * sizeof(*cfd));
	dfd = malloc(argc / 2 * sizeof(*dfd));
	if (!cfd || !dfd) {
		fprintf(stderr, "%s: malloc: %s\n", prgname, strerror(errno));
		exit(1);
	}

	/* Open all pairs, and build the fd_set for later select() */
	FD_ZERO(&control_set);
	for (i = 1, j = 0; i < argc; i += 2, j++) {
		cfd[j] = open(argv[i], O_RDONLY);
		dfd[j] = open(argv[i + 1], O_RDONLY);
		if (cfd[j] < 0) {
			fprintf(stderr, "%s: %s: %s\n", prgname, argv[i],
				strerror(errno));
			exit(1);
		}
		if (dfd[j] < 0) {
			fprintf(stderr, "%s: %s: %s\n", prgname, argv[i + 1],
				strerror(errno));
			exit(1);
		}
		/* ctrl file is used in select, data file is non-blocking */
		FD_SET(cfd[j], &control_set);
		fcntl(dfd[j], F_SETFL, fcntl(dfd[j], F_GETFL) | O_NONBLOCK);
		maxfd = dfd[j];
	}
	ndev = j;

	/* always log data we read to some filename */
	outfname = getenv("ZIO_DUMP_TO");
	if (!outfname)
		outfname = "/dev/null";
	f = fopen(outfname, "w");
	if (!f) {
		fprintf(stderr, "%s: %s: %s\n", prgname, outfname,
			strerror(errno));
		exit(1);
	}
	/* ensure proper strace information and output to pipes */
	setlinebuf(stdout);
	setbuf(f, NULL);

	/* now read control and then data, forever */
	while (1) {
		ready_set = control_set;
		i = select(maxfd + 1, &ready_set, NULL, NULL, NULL);
		if (i < 0 && errno == EINTR)
			continue;
		if (i < 0) {
			fprintf(stderr, "%s: select(): %s\n", prgname,
				strerror(errno));
			exit(1);
		}
		for (j = 0; j < ndev; j++)
			if (FD_ISSET(cfd[j], &ready_set))
				read_channel(cfd[j], dfd[j], f);
	}
}
