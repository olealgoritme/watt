/*
 * powmon-top.c - Real-time power monitoring TUI (like htop for watts)
 *
 * Continuously polls the powmon kernel module and displays
 * per-PID power attribution in a sortable ncurses interface.
 *
 * Build: gcc -Wall -O2 -I include -o powmon-top powmon-top.c -lncurses
 * Run:   ./powmon-top [-i interval_ms] [-n max_pids]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <ncurses.h>
#include <inttypes.h>

#include "powmon.h"

#define MAX_DISPLAY_PIDS 256
#define REFRESH_MS       1000

struct pid_display {
	int32_t  pid;
	char     comm[16];
	double   cpu_power_w;
	double   dram_power_w;
	double   total_power_w;
	double   cpu_energy_j;
	double   dram_energy_j;
	double   total_energy_j;
	double   cpu_time_s;
};

static volatile int running = 1;
static int dev_fd = -1;

static void sighandler(int sig)
{
	(void)sig;
	running = 0;
}

/* Scan /proc for all PIDs and track them */
static void ensure_tracking_all(void)
{
	ioctl(dev_fd, POWMON_IOC_TRACK_ALL);
}

/* Collect data for all tracked PIDs by scanning /proc */
static int collect_pid_data(struct pid_display *out, int max)
{
	DIR *proc;
	struct dirent *ent;
	int count = 0;

	proc = opendir("/proc");
	if (!proc)
		return 0;

	while ((ent = readdir(proc)) != NULL && count < max) {
		pid_t pid;
		char *endptr;
		struct powmon_query_pid query;

		pid = strtol(ent->d_name, &endptr, 10);
		if (*endptr != '\0' || pid <= 0)
			continue;

		query.pid = pid;
		if (ioctl(dev_fd, POWMON_IOC_GET_PID, &query) < 0)
			continue;

		struct powmon_pid_energy *r = &query.result;

		/* Skip PIDs with zero power */
		if (r->total_power_uw == 0 && r->total_energy_uj == 0)
			continue;

		out[count].pid = r->pid;
		memcpy(out[count].comm, r->comm, sizeof(r->comm));
		out[count].cpu_power_w = (double)r->cpu_power_uw / 1e6;
		out[count].dram_power_w = (double)r->dram_power_uw / 1e6;
		out[count].total_power_w = (double)r->total_power_uw / 1e6;
		out[count].cpu_energy_j = (double)r->cpu_energy_uj / 1e6;
		out[count].dram_energy_j = (double)r->dram_energy_uj / 1e6;
		out[count].total_energy_j = (double)r->total_energy_uj / 1e6;
		out[count].cpu_time_s = (double)r->cpu_time_ns / 1e9;
		count++;
	}

	closedir(proc);
	return count;
}

/* Sort by total power descending */
static int cmp_total_power(const void *a, const void *b)
{
	const struct pid_display *pa = a, *pb = b;
	if (pb->total_power_w > pa->total_power_w) return 1;
	if (pb->total_power_w < pa->total_power_w) return -1;
	return 0;
}

/* Get package-level power summary */
static void get_package_summary(double *pkg_w, double *dram_w,
				double *pkg_j, double *dram_j)
{
	struct powmon_system_info info;
	*pkg_w = *dram_w = *pkg_j = *dram_j = 0;

	if (ioctl(dev_fd, POWMON_IOC_GET_INFO, &info) < 0)
		return;

	for (uint32_t i = 0; i < info.nr_packages; i++) {
		struct powmon_query_package query = { .package_id = i };
		if (ioctl(dev_fd, POWMON_IOC_GET_PACKAGE, &query) < 0)
			continue;

		struct powmon_package_energy *r = &query.result;
		for (int d = 0; d < POWMON_DOMAIN_COUNT; d++) {
			if (!(r->domain_mask & (1 << d)))
				continue;
			if (d == POWMON_DOMAIN_PKG) {
				*pkg_w += (double)r->domains[d].power_uw / 1e6;
				*pkg_j += (double)r->domains[d].energy_uj / 1e6;
			} else if (d == POWMON_DOMAIN_DRAM) {
				*dram_w += (double)r->domains[d].power_uw / 1e6;
				*dram_j += (double)r->domains[d].energy_uj / 1e6;
			}
		}
	}
}

int main(int argc, char **argv)
{
	int refresh_ms = REFRESH_MS;
	struct pid_display pids[MAX_DISPLAY_PIDS];
	int opt;

	while ((opt = getopt(argc, argv, "i:h")) != -1) {
		switch (opt) {
		case 'i':
			refresh_ms = atoi(optarg);
			if (refresh_ms < 100) refresh_ms = 100;
			break;
		case 'h':
		default:
			fprintf(stderr, "Usage: %s [-i interval_ms]\n", argv[0]);
			return 1;
		}
	}

	dev_fd = open("/dev/powmon", O_RDWR);
	if (dev_fd < 0) {
		fprintf(stderr, "Cannot open /dev/powmon: %s\n"
			"Is the powmon module loaded?\n",
			strerror(errno));
		return 1;
	}

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	/* Enable tracking all */
	ensure_tracking_all();

	/* Init ncurses */
	initscr();
	cbreak();
	noecho();
	nodelay(stdscr, TRUE);
	curs_set(0);
	keypad(stdscr, TRUE);

	if (has_colors()) {
		start_color();
		init_pair(1, COLOR_GREEN, COLOR_BLACK);   /* header */
		init_pair(2, COLOR_YELLOW, COLOR_BLACK);   /* high power */
		init_pair(3, COLOR_RED, COLOR_BLACK);      /* very high */
		init_pair(4, COLOR_CYAN, COLOR_BLACK);     /* summary */
		init_pair(5, COLOR_WHITE, COLOR_BLUE);     /* title bar */
	}

	while (running) {
		int rows, cols;
		double pkg_w, dram_w, pkg_j, dram_j;
		int count;
		int ch;

		getmaxyx(stdscr, rows, cols);
		erase();

		/* Title bar */
		attron(COLOR_PAIR(5) | A_BOLD);
		mvhline(0, 0, ' ', cols);
		mvprintw(0, 1, " powmon-top - Per-Process Power Monitor ");
		attroff(COLOR_PAIR(5) | A_BOLD);

		/* Package summary */
		get_package_summary(&pkg_w, &dram_w, &pkg_j, &dram_j);
		attron(COLOR_PAIR(4));
		mvprintw(2, 1, "System: CPU %.2f W (%.3f J)  "
			 "DRAM %.2f W (%.3f J)  "
			 "Total %.2f W",
			 pkg_w, pkg_j, dram_w, dram_j, pkg_w + dram_w);
		attroff(COLOR_PAIR(4));

		/* Header */
		attron(COLOR_PAIR(1) | A_BOLD);
		mvprintw(4, 1, "%-7s %-16s %10s %10s %10s %10s %10s",
			 "PID", "COMMAND",
			 "CPU(W)", "DRAM(W)", "TOTAL(W)",
			 "ENERGY(J)", "CPU TIME");
		mvhline(5, 1, '-', cols - 2);
		attroff(COLOR_PAIR(1) | A_BOLD);

		/* Collect and sort */
		count = collect_pid_data(pids, MAX_DISPLAY_PIDS);
		qsort(pids, count, sizeof(pids[0]), cmp_total_power);

		/* Display PIDs */
		for (int i = 0; i < count && i < rows - 8; i++) {
			int color = 0;
			if (pids[i].total_power_w > 5.0)
				color = 3;  /* red: > 5W */
			else if (pids[i].total_power_w > 1.0)
				color = 2;  /* yellow: > 1W */

			if (color)
				attron(COLOR_PAIR(color));

			mvprintw(6 + i, 1,
				 "%-7d %-16s %10.3f %10.3f %10.3f %10.3f %10.1f",
				 pids[i].pid,
				 pids[i].comm,
				 pids[i].cpu_power_w,
				 pids[i].dram_power_w,
				 pids[i].total_power_w,
				 pids[i].total_energy_j,
				 pids[i].cpu_time_s);

			if (color)
				attroff(COLOR_PAIR(color));
		}

		/* Footer */
		attron(A_DIM);
		mvprintw(rows - 1, 1,
			 "q: quit  r: reset  %d processes  refresh: %dms",
			 count, refresh_ms);
		attroff(A_DIM);

		refresh();

		/* Handle input */
		ch = getch();
		if (ch == 'q' || ch == 'Q')
			break;
		if (ch == 'r' || ch == 'R')
			ioctl(dev_fd, POWMON_IOC_RESET_STATS);

		usleep(refresh_ms * 1000);
	}

	endwin();
	close(dev_fd);

	printf("powmon-top exited cleanly\n");
	return 0;
}
