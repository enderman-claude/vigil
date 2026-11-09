/*
 * vigil - read-only, real-time system status display for a Linux TTY.
 * Meant to run in place of getty (e.g. vigil@tty1.service). No login, no input.
 *
 * Trust model ("almost zero trust"):
 *  - never execs, never opens sockets, never writes anywhere but the tty
 *  - drops root to an unprivileged uid right after setup, verifies it can't regain it
 *  - sets no_new_privs
 *  - all keyboard input is discarded (echo, signals, line editing disabled)
 *  - every value read from /proc, /sys, /etc is size-bounded, range-checked and
 *    stripped of control bytes before it reaches the terminal
 *  The only things trusted: the kernel's /proc and /sys, and the tty it was given.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAXC 256
#define MAXL 300
#define LINEW 1024
#define VERSION "0.1.0"

static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

/* ---------- safe file reading / sanitising ---------- */
static int rd(const char *path, char *b, size_t n) {
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
	if (fd < 0) return -1;
	ssize_t r = read(fd, b, n - 1);
	close(fd);
	if (r < 0) return -1;
	b[r] = 0;
	return (int)r;
}
static void clean(char *s) { /* printable ASCII only */
	for (; *s; s++) if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e) *s = '?';
}
static void trim(char *s) {
	size_t l = strlen(s);
	while (l && (s[l - 1] == '\n' || s[l - 1] == ' ' || s[l - 1] == '"')) s[--l] = 0;
}

/* ---------- metrics ---------- */
typedef struct { unsigned long long busy, total; } cpu_t;
static cpu_t cur[MAXC + 1], prv[MAXC + 1];
static int ncpu;

static void read_cpu(void) {
	static char b[65536];
	if (rd("/proc/stat", b, sizeof b) < 0) return;
	memcpy(prv, cur, sizeof cur);
	ncpu = 0;
	char *l = b;
	while (l && !strncmp(l, "cpu", 3)) {
		char *p = l + 3, *nl = strchr(l, '\n');
		int idx = 0;
		if (*p != ' ') {
			char *e; long v = strtol(p, &e, 10);
			if (e == p || v < 0 || v >= MAXC) goto next;
			idx = (int)v + 1; p = e;
			if (idx > ncpu) ncpu = idx;
		}
		unsigned long long v[8] = {0};
		if (sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu",
		           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
			unsigned long long idle = v[3] + v[4], tot = 0;
			for (int i = 0; i < 8; i++) tot += v[i];
			cur[idx].total = tot; cur[idx].busy = tot - idle;
		}
	next:
		l = nl ? nl + 1 : NULL;
	}
}
static double cpu_pct(int i) {
	unsigned long long dt = cur[i].total - prv[i].total, db = cur[i].busy - prv[i].busy;
	if (cur[i].total < prv[i].total || !dt) return 0;
	return db > dt ? 100 : 100.0 * db / dt;
}
static long long meminfo(const char *b, const char *key) {
	const char *p = strstr(b, key);
	if (!p) return -1;
	return (long long)strtoull(p + strlen(key), NULL, 10);
}
static double psi(const char *path) {
	char b[256], *p;
	if (rd(path, b, sizeof b) < 0 || !(p = strstr(b, "avg10="))) return -1;
	return atof(p + 6);
}
static int temp_c(void) {
	int best = -999;
	for (int i = 0; i < 16; i++) {
		char p[64], b[32];
		snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", i);
		if (rd(p, b, sizeof b) < 0) continue;
		long v = atol(b) / 1000;
		if (v > -50 && v < 200 && v > best) best = (int)v;
	}
	return best;
}
static int battery(int *pct, char *st, size_t n) {
	static const char *names[] = {"BAT0", "BAT1", "BATT"};
	for (int i = 0; i < 3; i++) {
		char p[96], b[64];
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", names[i]);
		if (rd(p, b, sizeof b) < 0) continue;
		int v = atoi(b);
		if (v < 0 || v > 100) continue;
		*pct = v;
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", names[i]);
		if (rd(p, st, n) < 0) st[0] = 0;
		trim(st); clean(st);
		return 1;
	}
	return 0;
}
static void net_totals(unsigned long long *rx, unsigned long long *tx) {
	static char b[32768];
	*rx = *tx = 0;
	if (rd("/proc/net/dev", b, sizeof b) < 0) return;
	char *l = strchr(b, '\n');
	if (l) l = strchr(l + 1, '\n');
	while (l && *++l) {
		char *c = strchr(l, ':'), *nl = strchr(l, '\n');
		if (!c || (nl && c > nl)) break;
		while (*l == ' ') l++;
		if (strncmp(l, "lo:", 3)) {
			unsigned long long r, t;
			if (sscanf(c + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &r, &t) == 2) { *rx += r; *tx += t; }
		}
		l = nl;
	}
}

/* ---------- formatting ---------- */
static void human(char *d, size_t n, double v, const char *suffix) {
	static const char *u[] = {"B", "K", "M", "G", "T"};
	int i = 0;
	while (v >= 1024 && i < 4) { v /= 1024; i++; }
	snprintf(d, n, i ? "%.1f%s%s" : "%.0f%s%s", v, u[i], suffix);
}
static const char *HASH = "################################################################";
static const char *DOTS = "................................................................";
static void bar(char *d, size_t n, double pct, int w) {
	if (w > 64) w = 64;
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	int f = (int)(pct * w / 100.0 + 0.5);
	snprintf(d, n, "[\033[%sm%.*s\033[90m%.*s\033[0m]",
	         pct >= 85 ? "31" : pct >= 60 ? "33" : "32", f, HASH, w - f, DOTS);
}
static char lines[MAXL][LINEW];
static int nl;
static void L(const char *fmt, ...) {
	if (nl >= MAXL) return;
	va_list ap; va_start(ap, fmt);
	vsnprintf(lines[nl++], LINEW, fmt, ap);
	va_end(ap);
}

/* ---------- output ---------- */
static void wr(const char *p, size_t n) {
	while (n) {
		ssize_t w = write(1, p, n);
		if (w < 0) { if (errno == EINTR) continue; quit = 1; return; }
		p += w; n -= (size_t)w;
	}
}

static struct termios saved; static int have_saved;
static void restore(void) {
	const char *s = "\033[0m\033[?7h\033[?25h\033[2J\033[H";
	wr(s, strlen(s));
	if (have_saved) tcsetattr(0, TCSANOW, &saved);
}

static void usage(void) {
	fprintf(stderr, "vigil " VERSION " - read-only TTY status display\n"
	        "usage: vigil [-i seconds] [-u uid] [-B]\n"
	        "  -i N   refresh interval, 1-60 (default 1)\n"
	        "  -u UID unprivileged uid to drop to when started as root (default 65534)\n"
	        "  -B     disable console blanking/powerdown (Linux console only)\n");
	exit(2);
}

int main(int argc, char **argv) {
	int interval = 1, noblank = 0; long uid = 65534;
	for (int o; (o = getopt(argc, argv, "i:u:Bh")) != -1;) {
		char *e;
		switch (o) {
		case 'i': interval = (int)strtol(optarg, &e, 10); if (*e || interval < 1 || interval > 60) usage(); break;
		case 'u': uid = strtol(optarg, &e, 10); if (*e || uid < 1 || uid > 65535) usage(); break;
		case 'B': noblank = 1; break;
		default: usage();
		}
	}
	if (!isatty(1)) { fprintf(stderr, "vigil: stdout is not a tty\n"); return 1; }

	/* static info, read once while we may still be privileged */
	char os[128] = "Linux", b[4096];
	if (rd("/etc/os-release", b, sizeof b) > 0) {
		char *p = strstr(b, "\nPRETTY_NAME=");
		if (p) {
			p += 13; if (*p == '"') p++;
			char *e = strchr(p, '\n'); if (e) *e = 0;
			snprintf(os, sizeof os, "%s", p); trim(os); clean(os);
		}
	}

	/* terminal: no echo, no signals from keys, no line editing; keys are discarded */
	if (isatty(0) && tcgetattr(0, &saved) == 0) {
		struct termios t = saved; have_saved = 1;
		t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG | IEXTEN);
		t.c_iflag &= ~(tcflag_t)(IXON | IXOFF);
		t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
		tcsetattr(0, TCSANOW, &t);
	}
	if (noblank) wr("\033[9;0]\033[14;0]", 14);

	struct sigaction sa = {0};
	sa.sa_handler = on_sig;
	sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL); sigaction(SIGQUIT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	/* drop privileges; refuse to run if root can be regained */
	if (geteuid() == 0) {
		if (setgroups(0, NULL) || setgid((gid_t)uid) || setuid((uid_t)uid)) { perror("vigil: drop privileges"); return 1; }
	}
	if (setuid(0) == 0 && geteuid() == 0 && uid != 0) { fprintf(stderr, "vigil: privilege drop failed\n"); return 1; }
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

	wr("\033[2J\033[?25l\033[?7l", 15);
	unsigned long long prx = 0, ptx = 0; struct timespec pt = {0}; int first = 1;
	read_cpu();

	while (!quit) {
		struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
		double dt = first ? 0 : (now.tv_sec - pt.tv_sec) + (now.tv_nsec - pt.tv_nsec) / 1e9;
		read_cpu();

		struct winsize ws;
		int W = 80, H = 24;
		if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20 && ws.ws_row >= 3) { W = ws.ws_col; H = ws.ws_row; }
		int bw = W < 60 ? 10 : 20;
		nl = 0;

		/* header */
		struct utsname u; char host[80] = "?", kern[80] = "?", mach[80] = "?";
		if (!uname(&u)) {
			snprintf(host, sizeof host, "%s", u.nodename); snprintf(kern, sizeof kern, "%s", u.release);
			snprintf(mach, sizeof mach, "%s", u.machine);
			clean(host); clean(kern); clean(mach);
		}
		time_t tt = time(NULL); struct tm tm; char clk[64] = "";
		if (localtime_r(&tt, &tm)) strftime(clk, sizeof clk, "%a %d %b %Y  %H:%M:%S", &tm);
		int pad = W - 2 - (int)strlen(host) - (int)strlen(clk); if (pad < 1) pad = 1;
		L(" \033[1;36m%s\033[0m%*s\033[0m%s", host, pad, "", clk);

		double up = 0; char ub[64];
		if (rd("/proc/uptime", ub, sizeof ub) > 0) up = atof(ub);
		long d = (long)(up / 86400), h = (long)(up / 3600) % 24, m = (long)(up / 60) % 60;
		L(" \033[90m%s | kernel %s | %s | up %ldd %02ldh %02ldm\033[0m", os, kern, mach, d, h, m);
		L("%s", "");

		/* load */
		char lb[128]; double l1 = 0, l5 = 0, l15 = 0; int run = 0, tot = 0;
		if (rd("/proc/loadavg", lb, sizeof lb) > 0) sscanf(lb, "%lf %lf %lf %d/%d", &l1, &l5, &l15, &run, &tot);
		double pc = psi("/proc/pressure/cpu"), pm = psi("/proc/pressure/memory"), pi = psi("/proc/pressure/io");
		int cores = ncpu > 0 ? ncpu : 1;
		double lp = 100.0 * l1 / cores;
		char lbar[256]; bar(lbar, sizeof lbar, lp, bw);
		L(" load  %s \033[1m%.2f\033[0m %.2f %.2f  (%d cores)  run %d/%d", lbar, l1, l5, l15, cores, run, tot);
		if (pc >= 0 || pm >= 0 || pi >= 0) L(" psi   stall avg10  cpu %.1f%%  mem %.1f%%  io %.1f%%", pc < 0 ? 0 : pc, pm < 0 ? 0 : pm, pi < 0 ? 0 : pi);

		/* cpu */
		int tc = temp_c(); char ts[24] = "--";
		if (tc > -999) snprintf(ts, sizeof ts, "%dC", tc);
		bar(lbar, sizeof lbar, cpu_pct(0), bw);
		L(" cpu   %s %3.0f%%   temp %s", lbar, cpu_pct(0), ts);

		/* memory */
		static char mi[8192];
		if (rd("/proc/meminfo", mi, sizeof mi) > 0) {
			long long mt = meminfo(mi, "MemTotal:"), ma = meminfo(mi, "MemAvailable:");
			long long st = meminfo(mi, "SwapTotal:"), sf = meminfo(mi, "SwapFree:");
			char a[32], c[32];
			if (mt > 0 && ma >= 0 && ma <= mt) {
				double p = 100.0 * (double)(mt - ma) / (double)mt;
				human(a, sizeof a, (double)(mt - ma) * 1024, ""); human(c, sizeof c, (double)mt * 1024, "");
				bar(lbar, sizeof lbar, p, bw);
				L(" mem   %s %3.0f%%   %s / %s", lbar, p, a, c);
			}
			if (st > 0 && sf >= 0 && sf <= st) {
				double p = 100.0 * (double)(st - sf) / (double)st;
				human(a, sizeof a, (double)(st - sf) * 1024, ""); human(c, sizeof c, (double)st * 1024, "");
				bar(lbar, sizeof lbar, p, bw);
				L(" swap  %s %3.0f%%   %s / %s", lbar, p, a, c);
			}
		}

		/* disk */
		struct statvfs sv;
		if (!statvfs("/", &sv) && sv.f_blocks) {
			double tot_b = (double)sv.f_blocks * sv.f_frsize, free_b = (double)sv.f_bavail * sv.f_frsize;
			double p = 100.0 * (tot_b - free_b) / tot_b; char a[32], c[32];
			human(a, sizeof a, tot_b - free_b, ""); human(c, sizeof c, tot_b, "");
			bar(lbar, sizeof lbar, p, bw);
			L(" disk  %s %3.0f%%   %s / %s  (/)", lbar, p, a, c);
		}

		/* network */
		unsigned long long rx, tx; net_totals(&rx, &tx);
		char rs[32] = "0B/s", xs[32] = "0B/s";
		if (!first && dt > 0.01) {
			human(rs, sizeof rs, rx >= prx ? (double)(rx - prx) / dt : 0, "/s");
			human(xs, sizeof xs, tx >= ptx ? (double)(tx - ptx) / dt : 0, "/s");
		}
		prx = rx; ptx = tx; pt = now; first = 0;
		L(" net   rx %s   tx %s", rs, xs);

		/* battery */
		int bp; char bs[48];
		if (battery(&bp, bs, sizeof bs)) {
			bar(lbar, sizeof lbar, 100 - bp, bw); /* colour by how drained it is */
			L(" bat   %s %3d%%   %s", lbar, bp, bs);
		}

		/* per-core */
		if (ncpu > 0 && nl < H - 2) {
			L("%s", "");
			int cw = 18, ncol = (W - 1) / (cw + 2); if (ncol < 1) ncol = 1; if (ncol > 8) ncol = 8;
			for (int i = 0; i < ncpu && nl < H; i += ncol) {
				char row[LINEW] = " "; size_t rl = 1;
				for (int j = i; j < i + ncol && j < ncpu; j++) {
					char cb[160]; bar(cb, sizeof cb, cpu_pct(j + 1), 8);
					int n = snprintf(row + rl, sizeof row - rl, "%2d %s %3.0f%%  ", j, cb, cpu_pct(j + 1));
					if (n < 0 || (size_t)n >= sizeof row - rl) break;
					rl += (size_t)n;
				}
				L("%s", row);
			}
		}

		/* single write per frame */
		static char out[MAXL * LINEW / 4 + 4096]; size_t ol = 0;
		ol += (size_t)snprintf(out + ol, sizeof out - ol, "\033[H");
		int show = nl < H ? nl : H;
		for (int i = 0; i < show; i++) {
			int n = snprintf(out + ol, sizeof out - ol, "%s\033[K%s", lines[i], i + 1 < show ? "\n" : "");
			if (n < 0 || (size_t)n >= sizeof out - ol) break;
			ol += (size_t)n;
		}
		if (ol < sizeof out - 8) ol += (size_t)snprintf(out + ol, sizeof out - ol, "\033[J");
		wr(out, ol);

		if (have_saved) tcflush(0, TCIFLUSH); /* discard anything typed */
		struct timespec ts2 = {interval, 0};
		nanosleep(&ts2, NULL); /* interrupted by signals, loop re-checks quit */
	}
	restore();
	return 0;
}
