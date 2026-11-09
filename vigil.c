/*
 * SPDX-License-Identifier: GPL-3.0-only (see LICENSE)
 *
 * vigil - read-only, real-time system status display for a Linux TTY.
 * Meant to run in place of getty (e.g. vigil@tty1.service). No login, no input.
 *
 * Trust model ("almost zero trust"):
 *  - never execs, never opens sockets, never writes anywhere but the tty
 *  - drops root to an unprivileged uid right after setup, verifies it can't regain it
 *  - sets no_new_privs
 *  - all keyboard input is discarded (echo, signals, line editing disabled)
 *  - when started as root, the config file must be root-owned and not group/world-writable
 *  - every value read from /proc, /sys, /etc is size-bounded, range-checked and
 *    stripped of control bytes before it reaches the terminal; config values are
 *    ASCII-only and validated (no escape sequences can be injected)
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
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef VIGIL_VERSION
#define VIGIL_VERSION "dev"
#endif
#define CONF_DEFAULT "/etc/vigil.conf"
#define MAXC 256
#define MAXL 300
#define LINEW 1024

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
static char *strip(char *s) {
	while (*s == ' ' || *s == '\t' || *s == '\r') s++;
	size_t l = strlen(s);
	while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r')) s[--l] = 0;
	return s;
}

/* ---------- configuration ---------- */
enum { S_LOAD, S_PSI, S_CPU, S_MEM, S_SWAP, S_DISK, S_NET, S_BAT, S_CORES, S_N };
static const char *SECN[S_N] = {"load", "psi", "cpu", "mem", "swap", "disk", "net", "bat", "cores"};

typedef struct {
	int interval, noblank, color, warn, crit, bw, disk_auto, ndisks, disk_max, nsec;
	long uid;
	char fill[3], empty, lbr, rbr;
	char col[3][12], cempty[12], ctitle[12], cdim[12];
	char disks[16][128];
	int sec[S_N];
} cfg_t;
static cfg_t C;
static char E_f[3][24], E_e[24], E_t[24], E_d[24], E_r[8]; /* precomputed escapes ("" when colour is off) */

static const struct { const char *n, *c; } COLS[] = {
	{"black", "30"}, {"red", "31"}, {"green", "32"}, {"yellow", "33"}, {"blue", "34"}, {"magenta", "35"},
	{"cyan", "36"}, {"white", "37"}, {"gray", "90"}, {"bright-red", "91"}, {"bright-green", "92"},
	{"bright-yellow", "93"}, {"bright-blue", "94"}, {"bright-magenta", "95"}, {"bright-cyan", "96"},
	{"bright-white", "97"}, {"none", ""}};
static int setcol(char *dst, const char *v) {
	for (size_t i = 0; i < sizeof COLS / sizeof *COLS; i++)
		if (!strcmp(v, COLS[i].n)) { snprintf(dst, 12, "%s", COLS[i].c); return 1; }
	return 0;
}
static void style_classic(void) {
	C.color = 1; C.fill[0] = C.fill[1] = C.fill[2] = '#'; C.empty = '.'; C.lbr = '['; C.rbr = ']';
	setcol(C.col[0], "green"); setcol(C.col[1], "yellow"); setcol(C.col[2], "red");
	setcol(C.cempty, "gray"); setcol(C.ctitle, "cyan"); setcol(C.cdim, "gray");
}
static int set_style(const char *n) {
	if (!strcmp(n, "classic")) style_classic();
	else if (!strcmp(n, "mono")) { style_classic(); C.color = 0; }
	else if (!strcmp(n, "levels")) { style_classic(); C.color = 0; C.fill[0] = '-'; C.fill[1] = '='; C.fill[2] = '*'; }
	else return 0;
	return 1;
}
static void defaults(void) {
	memset(&C, 0, sizeof C);
	C.interval = 1; C.uid = 65534; C.warn = 60; C.crit = 85; C.disk_auto = 1; C.disk_max = 8;
	for (int i = 0; i < S_N; i++) C.sec[i] = i;
	C.nsec = S_N;
	style_classic();
}
static void esc(char *d, const char *pre, const char *code) {
	d[0] = 0;
	if (!C.color || (!*pre && !*code)) return;
	snprintf(d, 24, "\033[%s%s%sm", pre, (*pre && *code) ? ";" : "", code);
}
static void finalize(void) {
	for (int i = 0; i < 3; i++) esc(E_f[i], "", C.col[i]);
	esc(E_e, "", C.cempty); esc(E_t, "1", C.ctitle); esc(E_d, "", C.cdim);
	snprintf(E_r, sizeof E_r, "%s", C.color ? "\033[0m" : "");
	if (C.warn > C.crit) C.warn = C.crit;
}

static int pnum(const char *v, long lo, long hi, long *o) {
	char *e;
	if (!*v) return 0;
	long x = strtol(v, &e, 10);
	if (*e || x < lo || x > hi) return 0;
	*o = x; return 1;
}
static int pbool(const char *v) {
	if (!strcmp(v, "yes") || !strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "1")) return 1;
	if (!strcmp(v, "no") || !strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "0")) return 0;
	return -1;
}
static int pchar(const char *v, char *o, int allow_none) {
	if (!strcmp(v, "space")) { *o = ' '; return 1; }
	if (allow_none && !strcmp(v, "none")) { *o = 0; return 1; }
	if (strlen(v) == 1 && v[0] > 0x20 && v[0] < 0x7f) { *o = v[0]; return 1; }
	return 0;
}

static void conf_set(char *k, char *v, int pass, int ln) {
	int ok = 1; long n; char c;
	if (!strcmp(k, "style")) { if (pass == 0 && !set_style(v)) ok = 0; }
	else if (pass == 0) return;
	else if (!strcmp(k, "interval")) { if ((ok = pnum(v, 1, 60, &n))) C.interval = (int)n; }
	else if (!strcmp(k, "uid")) { if ((ok = pnum(v, 1, 65535, &n))) C.uid = n; }
	else if (!strcmp(k, "disable_blanking")) { int b = pbool(v); if ((ok = b >= 0)) C.noblank = b; }
	else if (!strcmp(k, "color")) { int b = pbool(v); if ((ok = b >= 0)) C.color = b; }
	else if (!strcmp(k, "fill")) { if ((ok = pchar(v, &c, 0) && c != ' ')) C.fill[0] = C.fill[1] = C.fill[2] = c; }
	else if (!strcmp(k, "fill_low")) { if ((ok = pchar(v, &c, 0) && c != ' ')) C.fill[0] = c; }
	else if (!strcmp(k, "fill_mid")) { if ((ok = pchar(v, &c, 0) && c != ' ')) C.fill[1] = c; }
	else if (!strcmp(k, "fill_high")) { if ((ok = pchar(v, &c, 0) && c != ' ')) C.fill[2] = c; }
	else if (!strcmp(k, "empty")) { if ((ok = pchar(v, &c, 0))) C.empty = c; }
	else if (!strcmp(k, "left")) { if ((ok = pchar(v, &c, 1))) C.lbr = c; }
	else if (!strcmp(k, "right")) { if ((ok = pchar(v, &c, 1))) C.rbr = c; }
	else if (!strcmp(k, "color_low")) ok = setcol(C.col[0], v);
	else if (!strcmp(k, "color_mid")) ok = setcol(C.col[1], v);
	else if (!strcmp(k, "color_high")) ok = setcol(C.col[2], v);
	else if (!strcmp(k, "color_empty")) ok = setcol(C.cempty, v);
	else if (!strcmp(k, "color_title")) ok = setcol(C.ctitle, v);
	else if (!strcmp(k, "color_dim")) ok = setcol(C.cdim, v);
	else if (!strcmp(k, "warn")) { if ((ok = pnum(v, 1, 100, &n))) C.warn = (int)n; }
	else if (!strcmp(k, "crit")) { if ((ok = pnum(v, 1, 100, &n))) C.crit = (int)n; }
	else if (!strcmp(k, "bar_width")) { if ((ok = pnum(v, 0, 40, &n) && (n == 0 || n >= 4))) C.bw = (int)n; }
	else if (!strcmp(k, "disk_max")) { if ((ok = pnum(v, 1, 16, &n))) C.disk_max = (int)n; }
	else if (!strcmp(k, "sections")) {
		int s[S_N], ns = 0, seen = 0;
		for (char *t = strtok(v, ","); t; t = strtok(NULL, ",")) {
			t = strip(t);
			int i; for (i = 0; i < S_N; i++) if (!strcmp(t, SECN[i])) break;
			if (i == S_N) { clean(t); fprintf(stderr, "vigil: config line %d: unknown section '%.20s'\n", ln, t); continue; }
			if (seen & (1 << i)) continue;
			seen |= 1 << i; s[ns++] = i;
		}
		if ((ok = ns > 0)) { memcpy(C.sec, s, sizeof s); C.nsec = ns; }
	}
	else if (!strcmp(k, "disks")) {
		if (!strcmp(v, "auto")) { C.disk_auto = 1; C.ndisks = 0; }
		else {
			char tmp[16][128]; int nd = 0;
			for (char *t = strtok(v, ","); t && ok; t = strtok(NULL, ",")) {
				t = strip(t);
				if (t[0] != '/' || strlen(t) >= 128 || nd >= 16) { ok = 0; break; }
				for (char *p = t; *p; p++) if ((unsigned char)*p < 0x20 || *p == 0x7f) ok = 0;
				if (ok) snprintf(tmp[nd++], 128, "%s", t);
			}
			if (ok && nd) { memcpy(C.disks, tmp, sizeof tmp); C.ndisks = nd; C.disk_auto = 0; } else ok = 0;
		}
	}
	else { fprintf(stderr, "vigil: config line %d: unknown key '%.30s'\n", ln, k); return; }
	if (!ok) fprintf(stderr, "vigil: config line %d: bad value for '%.30s'\n", ln, k);
}

static void conf_load(const char *path, int explicit_path) {
	char b[8192], t[8192];
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW);
	if (fd < 0) {
		if (explicit_path || errno != ENOENT) fprintf(stderr, "vigil: cannot read config %s: %s\n", path, strerror(errno));
		return;
	}
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { fprintf(stderr, "vigil: config %s is not a regular file, ignored\n", path); close(fd); return; }
	if (geteuid() == 0 && (st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)))) {
		fprintf(stderr, "vigil: config %s must be root-owned and not group/world-writable, ignored\n", path);
		close(fd); return;
	}
	ssize_t n = read(fd, b, sizeof b - 1);
	close(fd);
	if (n < 0 || n >= (ssize_t)sizeof b - 1) { fprintf(stderr, "vigil: config %s unreadable or too large, ignored\n", path); return; }
	b[n] = 0;
	for (int pass = 0; pass < 2; pass++) { /* pass 0: style preset only; pass 1: everything else */
		memcpy(t, b, (size_t)n + 1);
		int ln = 0;
		for (char *l = t, *nx; l && *l; l = nx) {
			nx = strchr(l, '\n'); if (nx) *nx++ = 0;
			ln++;
			l = strip(l);
			if (!*l || *l == '#' || *l == ';') continue;
			char *eq = strchr(l, '=');
			if (!eq) { if (pass) fprintf(stderr, "vigil: config line %d: expected key = value\n", ln); continue; }
			*eq = 0;
			char *k = strip(l), *v = strip(eq + 1);
			clean(k);
			conf_set(k, v, pass, ln);
		}
	}
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

/* mounts: real block-device (and zfs) filesystems, one entry per device (shortest mountpoint wins) */
typedef struct { char mp[128], dev[96], fs[24]; } mnt_t;
static void unesc(char *s) { /* /proc/mounts octal escapes, e.g. \040 */
	char *o = s;
	while (*s) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
			*o++ = (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) | (s[3] - '0')); s += 4;
		} else *o++ = *s++;
	}
	*o = 0;
}
static int mounts(mnt_t *m, int max) {
	static char b[65536];
	if (rd("/proc/self/mounts", b, sizeof b) < 0) return 0;
	int n = 0;
	for (char *l = b, *nx; l && *l; l = nx) {
		nx = strchr(l, '\n'); if (nx) *nx++ = 0;
		char dev[96], mp[128], fs[24];
		if (sscanf(l, "%95s %127s %23s", dev, mp, fs) != 3) continue;
		int real = !strncmp(dev, "/dev/", 5) && strncmp(dev, "/dev/loop", 9) && strncmp(dev, "/dev/ram", 8);
		if ((!real && strcmp(fs, "zfs")) || !strcmp(fs, "squashfs") || !strcmp(fs, "erofs")) continue;
		unesc(mp);
		int j; for (j = 0; j < n; j++) if (!strcmp(m[j].dev, dev)) break;
		if (j < n) { if (strlen(mp) < strlen(m[j].mp)) { snprintf(m[j].mp, sizeof m[j].mp, "%s", mp); snprintf(m[j].fs, sizeof m[j].fs, "%s", fs); } continue; }
		if (n < max) { snprintf(m[n].dev, sizeof m[n].dev, "%s", dev); snprintf(m[n].mp, sizeof m[n].mp, "%s", mp); snprintf(m[n].fs, sizeof m[n].fs, "%s", fs); n++; }
	}
	return n;
}

/* ---------- formatting ---------- */
static void human(char *d, size_t n, double v, const char *suffix) {
	static const char *u[] = {"B", "K", "M", "G", "T"};
	int i = 0;
	while (v >= 1024 && i < 4) { v /= 1024; i++; }
	snprintf(d, n, i ? "%.1f%s%s" : "%.0f%s%s", v, u[i], suffix);
}
/* fillp = how much of the bar is filled; lvlp = which colour/char level (low/mid/high) */
static void bar2(char *d, size_t n, double fillp, double lvlp, int w) {
	if (w > 64) w = 64;
	if (w < 1) w = 1;
	if (fillp < 0) fillp = 0;
	if (fillp > 100) fillp = 100;
	int f = (int)(fillp * w / 100.0 + 0.5), lv = lvlp >= C.crit ? 2 : lvlp >= C.warn ? 1 : 0;
	size_t k = 0;
#define PUT(...) do { int r_ = snprintf(d + k, n - k, __VA_ARGS__); if (r_ > 0 && (size_t)r_ < n - k) k += (size_t)r_; } while (0)
	if (C.lbr) PUT("%c", C.lbr);
	PUT("%s", E_f[lv]);
	for (int i = 0; i < f; i++) PUT("%c", C.fill[lv]);
	PUT("%s", E_e);
	for (int i = f; i < w; i++) PUT("%c", C.empty);
	PUT("%s", E_r);
	if (C.rbr) PUT("%c", C.rbr);
#undef PUT
}
static void bar(char *d, size_t n, double pct, int w) { bar2(d, n, pct, pct, w); }

static char lines[MAXL][LINEW];
static int nl;
static void L(const char *fmt, ...) {
	if (nl >= MAXL) return;
	va_list ap; va_start(ap, fmt);
	vsnprintf(lines[nl++], LINEW, fmt, ap);
	va_end(ap);
}
static char os[128] = "Linux";

static void render(int W, int H) {
	int bw = C.bw ? C.bw : (W < 60 ? 10 : 20);
	char lbar[256];
	nl = 0;

	struct utsname u; char host[80] = "?", kern[80] = "?", mach[80] = "?";
	if (!uname(&u)) {
		snprintf(host, sizeof host, "%s", u.nodename); snprintf(kern, sizeof kern, "%s", u.release);
		snprintf(mach, sizeof mach, "%s", u.machine);
		clean(host); clean(kern); clean(mach);
	}
	time_t tt = time(NULL); struct tm tm; char clk[64] = "";
	if (localtime_r(&tt, &tm)) strftime(clk, sizeof clk, "%a %d %b %Y  %H:%M:%S", &tm);
	int pad = W - 2 - (int)strlen(host) - (int)strlen(clk); if (pad < 1) pad = 1;
	L(" %s%s%s%*s%s", E_t, host, E_r, pad, "", clk);
	double up = 0; char ub[64];
	if (rd("/proc/uptime", ub, sizeof ub) > 0) up = atof(ub);
	L(" %s%s | kernel %s | %s | up %ldd %02ldh %02ldm%s", E_d, os, kern, mach,
	  (long)(up / 86400), (long)(up / 3600) % 24, (long)(up / 60) % 60, E_r);
	L("%s", "");

	for (int si = 0; si < C.nsec; si++) {
		switch (C.sec[si]) {
		case S_LOAD: {
			char lb[128]; double l1 = 0, l5 = 0, l15 = 0; int run = 0, tot = 0;
			if (rd("/proc/loadavg", lb, sizeof lb) > 0) sscanf(lb, "%lf %lf %lf %d/%d", &l1, &l5, &l15, &run, &tot);
			int cores = ncpu > 0 ? ncpu : 1;
			bar(lbar, sizeof lbar, 100.0 * l1 / cores, bw);
			L(" load  %s %s%.2f%s %.2f %.2f  (%d cores)  run %d/%d", lbar, C.color ? "\033[1m" : "", l1, E_r, l5, l15, cores, run, tot);
		} break;
		case S_PSI: {
			double pc = psi("/proc/pressure/cpu"), pm = psi("/proc/pressure/memory"), pi = psi("/proc/pressure/io");
			if (pc >= 0 || pm >= 0 || pi >= 0)
				L(" psi   stall avg10  cpu %.1f%%  mem %.1f%%  io %.1f%%", pc < 0 ? 0 : pc, pm < 0 ? 0 : pm, pi < 0 ? 0 : pi);
		} break;
		case S_CPU: {
			int tc = temp_c(); char ts[24] = "--";
			if (tc > -999) snprintf(ts, sizeof ts, "%dC", tc);
			bar(lbar, sizeof lbar, cpu_pct(0), bw);
			L(" cpu   %s %3.0f%%   temp %s", lbar, cpu_pct(0), ts);
		} break;
		case S_MEM: case S_SWAP: {
			static char mi[8192];
			if (rd("/proc/meminfo", mi, sizeof mi) <= 0) break;
			int sw = C.sec[si] == S_SWAP;
			long long t = meminfo(mi, sw ? "SwapTotal:" : "MemTotal:"), f = meminfo(mi, sw ? "SwapFree:" : "MemAvailable:");
			if (t > 0 && f >= 0 && f <= t) {
				char a[32], c[32]; double p = 100.0 * (double)(t - f) / (double)t;
				human(a, sizeof a, (double)(t - f) * 1024, ""); human(c, sizeof c, (double)t * 1024, "");
				bar(lbar, sizeof lbar, p, bw);
				L(" %-5s %s %3.0f%%   %s / %s", sw ? "swap" : "mem", lbar, p, a, c);
			}
		} break;
		case S_DISK: {
			mnt_t m[64]; int n = 0;
			if (C.disk_auto) n = mounts(m, 64);
			else for (int i = 0; i < C.ndisks; i++) { snprintf(m[n].mp, sizeof m[n].mp, "%s", C.disks[i]); m[n].dev[0] = m[n].fs[0] = 0; n++; }
			struct { char lab[40], fs[24]; double p, used, tot; } r[16]; int nr = 0, lw = 4;
			for (int i = 0; i < n && nr < C.disk_max; i++) {
				struct statvfs sv;
				if (statvfs(m[i].mp, &sv) || !sv.f_blocks) continue;
				double tot = (double)sv.f_blocks * (double)sv.f_frsize, fr = (double)sv.f_bavail * (double)sv.f_frsize;
				r[nr].tot = tot; r[nr].used = tot - fr; r[nr].p = 100.0 * (tot - fr) / tot;
				snprintf(r[nr].fs, sizeof r[nr].fs, "%s", m[i].fs); clean(r[nr].fs);
				snprintf(r[nr].lab, sizeof r[nr].lab, "%.39s", m[i].mp); clean(r[nr].lab);
				if (strlen(r[nr].lab) > 14) { r[nr].lab[12] = r[nr].lab[13] = '.'; r[nr].lab[14] = 0; }
				if ((int)strlen(r[nr].lab) > lw) lw = (int)strlen(r[nr].lab);
				nr++;
			}
			for (int i = 0; i < nr; i++) {
				char a[32], c[32];
				human(a, sizeof a, r[i].used, ""); human(c, sizeof c, r[i].tot, "");
				bar(lbar, sizeof lbar, r[i].p, bw);
				L(" %-5s %-*s %s %3.0f%%  %s / %s  %s%s%s", i ? "" : "disk", lw, r[i].lab, lbar, r[i].p, a, c, E_d, r[i].fs, E_r);
			}
		} break;
		case S_NET: {
			static unsigned long long prx, ptx; static struct timespec pt; static int first = 1;
			struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
			double dt = (now.tv_sec - pt.tv_sec) + (now.tv_nsec - pt.tv_nsec) / 1e9;
			unsigned long long rx, tx; net_totals(&rx, &tx);
			char rs[32] = "0B/s", xs[32] = "0B/s";
			if (!first && dt > 0.01) {
				human(rs, sizeof rs, rx >= prx ? (double)(rx - prx) / dt : 0, "/s");
				human(xs, sizeof xs, tx >= ptx ? (double)(tx - ptx) / dt : 0, "/s");
			}
			prx = rx; ptx = tx; pt = now; first = 0;
			L(" net   rx %s   tx %s", rs, xs);
		} break;
		case S_BAT: {
			int bp; char bs[48];
			if (battery(&bp, bs, sizeof bs)) {
				bar2(lbar, sizeof lbar, bp, 100 - bp, bw); /* filled by charge, coloured by how drained */
				L(" bat   %s %3d%%   %s", lbar, bp, bs);
			}
		} break;
		case S_CORES: {
			if (ncpu <= 0 || nl >= H - 2) break;
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
		} break;
		}
	}
}

/* ---------- output ---------- */
static void wr(const char *p, size_t n) {
	while (n) {
		ssize_t w = write(1, p, n);
		if (w < 0) { if (errno == EINTR) continue; quit = 1; return; }
		p += w; n -= (size_t)w;
	}
}
static void emit(int H) { /* one write per frame */
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
}

static struct termios saved; static int have_saved;
static void restore(void) {
	const char *s = "\033[0m\033[?7h\033[?25h\033[2J\033[H";
	wr(s, strlen(s));
	if (have_saved) tcsetattr(0, TCSANOW, &saved);
}

static int usage(void) {
	fprintf(stderr, "vigil " VIGIL_VERSION " - read-only TTY status display\n"
	        "usage: vigil [-c file] [-i seconds] [-u uid] [-B] [-1]\n"
	        "  -c FILE config file (default " CONF_DEFAULT ")\n"
	        "  -i N    refresh interval, 1-60 (overrides config)\n"
	        "  -u UID  uid to drop to when started as root (overrides config)\n"
	        "  -B      disable console blanking/powerdown (overrides config)\n"
	        "  -1      print one plain frame to stdout and exit (works without a tty)\n");
	exit(2);
}

int main(int argc, char **argv) {
	const char *cp = CONF_DEFAULT; int cexp = 0, once = 0, o; long n;
	defaults();
	opterr = 0;
	while ((o = getopt(argc, argv, "c:i:u:B1h")) != -1) { /* pre-pass: find -c before loading config */
		if (o == 'c') { cp = optarg; cexp = 1; } else if (o == 'h') usage();
	}
	conf_load(cp, cexp);
	opterr = 1; optind = 0; /* second pass: command line overrides config */
	while ((o = getopt(argc, argv, "c:i:u:B1h")) != -1) {
		switch (o) {
		case 'c': break;
		case 'i': if (!pnum(optarg, 1, 60, &n)) usage(); C.interval = (int)n; break;
		case 'u': if (!pnum(optarg, 1, 65535, &n)) usage(); C.uid = n; break;
		case 'B': C.noblank = 1; break;
		case '1': once = 1; break;
		default: usage();
		}
	}
	if (!once && !isatty(1)) { fprintf(stderr, "vigil: stdout is not a tty (use -1 for one plain frame)\n"); return 1; }
	if (once && !isatty(1)) C.color = 0;
	finalize();

	/* static info, read once while we may still be privileged */
	char b[4096];
	if (rd("/etc/os-release", b, sizeof b) > 0) {
		char *p = strstr(b, "\nPRETTY_NAME=");
		if (p) {
			p += 13; if (*p == '"') p++;
			char *e = strchr(p, '\n'); if (e) *e = 0;
			snprintf(os, sizeof os, "%s", p); trim(os); clean(os);
		}
	}

	if (!once) {
		/* terminal: no echo, no signals from keys, no line editing; keys are discarded */
		if (isatty(0) && tcgetattr(0, &saved) == 0) {
			struct termios t = saved; have_saved = 1;
			t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG | IEXTEN);
			t.c_iflag &= ~(tcflag_t)(IXON | IXOFF);
			t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
			tcsetattr(0, TCSANOW, &t);
		}
		if (C.noblank) wr("\033[9;0]\033[14;0]", 14);
	}

	struct sigaction sa = {0};
	sa.sa_handler = on_sig;
	sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL); sigaction(SIGQUIT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	/* drop privileges; refuse to run if root can be regained */
	if (geteuid() == 0) {
		if (setgroups(0, NULL) || setgid((gid_t)C.uid) || setuid((uid_t)C.uid)) { perror("vigil: drop privileges"); return 1; }
	}
	if (setuid(0) == 0 && geteuid() == 0) { fprintf(stderr, "vigil: privilege drop failed\n"); return 1; }
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

	read_cpu();
	if (once) {
		struct timespec ts = {0, 250000000};
		nanosleep(&ts, NULL);
		read_cpu();
		int W = 100, H = MAXL; struct winsize ws;
		if (isatty(1) && ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20) { W = ws.ws_col; H = ws.ws_row; }
		render(W, H);
		for (int i = 0; i < nl; i++) puts(lines[i]);
		return 0;
	}

	wr("\033[2J\033[?25l\033[?7l", 15);
	while (!quit) {
		read_cpu();
		struct winsize ws; int W = 80, H = 24;
		if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20 && ws.ws_row >= 3) { W = ws.ws_col; H = ws.ws_row; }
		render(W, H);
		emit(H);
		if (have_saved) tcflush(0, TCIFLUSH); /* discard anything typed */
		struct timespec ts = {C.interval, 0};
		nanosleep(&ts, NULL); /* interrupted by signals; loop re-checks quit */
	}
	restore();
	return 0;
}
