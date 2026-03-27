/*
 * watt.c — Per-Process Power Monitor TUI
 * ========================================
 * Built on flux.h (Elm Architecture TUI) and the powmon kernel module.
 *
 * 4 tabs:
 *   1. Overview  — system-wide RAPL gauges + sparklines per domain
 *   2. Processes — top-N PIDs sorted by power, scrollable table
 *   3. Cores     — per-core power heatmap with package breakdown
 *   4. Config    — sampling interval, tracking mode, module info
 *
 * Build:  gcc -O2 -std=c99 -o watt watt.c -lpthread
 * Run:    sudo ./watt
 *
 * Requires the powmon kernel module loaded (/dev/powmon).
 */

#define FLUX_IMPL
#include "flux.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "powmon.h"

/* ═══════════════════════════════════════════════════════════════
 * PALETTE
 * ═══════════════════════════════════════════════════════════════ */
#define CY  "\x1b[38;2;82;213;196m"
#define AM  "\x1b[38;2;255;188;68m"
#define PU  "\x1b[38;2;180;130;255m"
#define GR  "\x1b[38;2;105;220;150m"
#define RE  "\x1b[38;2;255;100;100m"
#define BL  "\x1b[38;2;100;160;255m"
#define OR  "\x1b[38;2;255;140;80m"
#define PK  "\x1b[38;2;255;120;180m"
#define DM  "\x1b[38;2;75;95;115m"
#define WH  "\x1b[38;2;215;225;245m"
#define BD  "\x1b[1m"
#define IT  "\x1b[3m"
#define RS  "\x1b[0m"

#define BG0 "\x1b[48;2;11;14;20m"
#define BG1 "\x1b[48;2;17;22;32m"
#define BG2 "\x1b[48;2;24;32;48m"
#define BGX "\x1b[48;2;45;65;100m"

/* ═══════════════════════════════════════════════════════════════
 * TABS
 * ═══════════════════════════════════════════════════════════════ */
#define SCR_OVERVIEW  0
#define SCR_PROCS     1
#define SCR_CORES     2
#define SCR_CONFIG    3
#define SCR_N         4

static const char *TAB_ICONS[]  = {"⚡","◉","▦","⚙"};
static const char *TAB_LABELS[] = {"Overview","Processes","Cores","Config"};

/* ═══════════════════════════════════════════════════════════════
 * TICK IDs
 * ═══════════════════════════════════════════════════════════════ */
#define MSG_TICK_POLL   1

/* ═══════════════════════════════════════════════════════════════
 * SPARKLINE RING
 * ═══════════════════════════════════════════════════════════════ */
#define SPARK_LEN 40

typedef struct {
    float ring[SPARK_LEN];
    int   head;
} SparkRing;

static void spark_push(SparkRing *s, float v) {
    s->ring[s->head] = v;
    s->head = (s->head + 1) % SPARK_LEN;
}

/* ═══════════════════════════════════════════════════════════════
 * PID ENTRY (for display)
 * ═══════════════════════════════════════════════════════════════ */
#define MAX_DISP_PIDS 256

typedef struct {
    int32_t  pid;
    char     comm[16];
    double   cpu_w;
    double   dram_w;
    double   total_w;
    double   total_j;
    double   cpu_time_s;
} PidRow;

/* ═══════════════════════════════════════════════════════════════
 * APP STATE
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
    int screen;
    int dev_fd;
    int connected;
    int poll_ms;
    int tick;

    struct powmon_system_info sysinfo;

    double pkg_w[POWMON_MAX_PACKAGES];
    double dram_w[POWMON_MAX_PACKAGES];
    double core_w[POWMON_MAX_PACKAGES];
    double pkg_j[POWMON_MAX_PACKAGES];
    double dram_j[POWMON_MAX_PACKAGES];

    double sys_pkg_w;
    double sys_dram_w;
    double sys_total_w;
    double sys_pkg_j;
    double sys_dram_j;

    SparkRing spark_pkg;
    SparkRing spark_dram;
    SparkRing spark_total;
    float     peak_total_w;

    double core_power[POWMON_MAX_CORES];
    double core_max_w;

    PidRow   pids[MAX_DISP_PIDS];
    int      pid_count;
    int      proc_scroll;
    int      proc_cursor;
    int      proc_sort;

    int      track_mode;
    FluxInput interval_input;
    int       config_focus;
    char      status_msg[128];
    int       status_tick;
} App;

static int has_dram(App *a) {
    return a->sysinfo.domain_mask & (1 << POWMON_DOMAIN_DRAM);
}

/* ═══════════════════════════════════════════════════════════════
 * DEVICE I/O
 * ═══════════════════════════════════════════════════════════════ */
static int dev_open(App *a) {
    a->dev_fd = open("/dev/powmon", O_RDWR);
    if (a->dev_fd < 0) { a->connected = 0; return -1; }
    a->connected = 1;
    if (ioctl(a->dev_fd, POWMON_IOC_GET_INFO, &a->sysinfo) < 0) {
        a->connected = 0; close(a->dev_fd); a->dev_fd = -1; return -1;
    }
    ioctl(a->dev_fd, POWMON_IOC_TRACK_ALL);
    a->track_mode = 1;
    return 0;
}

static void dev_poll(App *a) {
    if (!a->connected || a->dev_fd < 0) return;
    ioctl(a->dev_fd, POWMON_IOC_GET_INFO, &a->sysinfo);

    a->sys_pkg_w = a->sys_dram_w = 0;
    a->sys_pkg_j = a->sys_dram_j = 0;

    for (uint32_t i = 0; i < a->sysinfo.nr_packages && i < POWMON_MAX_PACKAGES; i++) {
        struct powmon_query_package qp = { .package_id = i };
        if (ioctl(a->dev_fd, POWMON_IOC_GET_PACKAGE, &qp) < 0) continue;
        struct powmon_package_energy *r = &qp.result;
        a->pkg_w[i] = a->dram_w[i] = a->core_w[i] = 0;
        a->pkg_j[i] = a->dram_j[i] = 0;
        for (int d = 0; d < POWMON_DOMAIN_COUNT; d++) {
            if (!(r->domain_mask & (1 << d))) continue;
            double w = (double)r->domains[d].power_uw / 1e6;
            double j = (double)r->domains[d].energy_uj / 1e6;
            if (d == POWMON_DOMAIN_PKG)  { a->pkg_w[i]  = w; a->pkg_j[i]  = j; }
            if (d == POWMON_DOMAIN_DRAM) { a->dram_w[i] = w; a->dram_j[i] = j; }
            if (d == POWMON_DOMAIN_CORE) { a->core_w[i] = w; }
        }
        a->sys_pkg_w  += a->pkg_w[i];
        a->sys_dram_w += a->dram_w[i];
        a->sys_pkg_j  += a->pkg_j[i];
        a->sys_dram_j += a->dram_j[i];
    }
    a->sys_total_w = a->sys_pkg_w + a->sys_dram_w;

    float max_w = 150.0f;
    spark_push(&a->spark_pkg,   (float)(a->sys_pkg_w   / max_w));
    spark_push(&a->spark_dram,  (float)(a->sys_dram_w  / max_w));
    spark_push(&a->spark_total, (float)(a->sys_total_w / max_w));
    if ((float)a->sys_total_w > a->peak_total_w)
        a->peak_total_w = (float)a->sys_total_w;

    a->core_max_w = 0;
    for (uint32_t c = 0; c < a->sysinfo.nr_cores && c < POWMON_MAX_CORES; c++) {
        struct powmon_query_core qc = { .core_id = c };
        if (ioctl(a->dev_fd, POWMON_IOC_GET_CORE, &qc) == 0) {
            a->core_power[c] = (double)qc.result.power_uw / 1e6;
            if (a->core_power[c] > a->core_max_w) a->core_max_w = a->core_power[c];
        }
    }

    a->pid_count = 0;
    DIR *proc = opendir("/proc");
    if (proc) {
        struct dirent *ent;
        while ((ent = readdir(proc)) != NULL && a->pid_count < MAX_DISP_PIDS) {
            char *endp;
            long pid = strtol(ent->d_name, &endp, 10);
            if (*endp != '\0' || pid <= 0) continue;
            struct powmon_query_pid qpid = { .pid = (int32_t)pid };
            if (ioctl(a->dev_fd, POWMON_IOC_GET_PID, &qpid) < 0) continue;
            struct powmon_pid_energy *r = &qpid.result;
            if (r->total_power_uw == 0 && r->total_energy_uj == 0) continue;
            PidRow *row = &a->pids[a->pid_count++];
            row->pid        = r->pid;
            memcpy(row->comm, r->comm, sizeof r->comm);
            row->cpu_w      = (double)r->cpu_power_uw / 1e6;
            row->dram_w     = (double)r->dram_power_uw / 1e6;
            row->total_w    = (double)r->total_power_uw / 1e6;
            row->total_j    = (double)r->total_energy_uj / 1e6;
            row->cpu_time_s = (double)r->cpu_time_ns / 1e9;
        }
        closedir(proc);
    }

    /* sort PIDs descending by selected column
     * With DRAM: 0=total_w 1=cpu_w 2=dram_w 3=total_j
     * No DRAM:   0=total_w 1=cpu_w 2=total_j */
    int sort_col = a->proc_sort;
    if (!has_dram(a) && sort_col >= 2) sort_col++; /* skip dram_w slot */
    for (int i = 0; i < a->pid_count - 1; i++) {
        for (int j = i + 1; j < a->pid_count; j++) {
            double a_val, b_val;
            switch (sort_col) {
            case 1:  a_val = a->pids[i].cpu_w;   b_val = a->pids[j].cpu_w;   break;
            case 2:  a_val = a->pids[i].dram_w;  b_val = a->pids[j].dram_w;  break;
            case 3:  a_val = a->pids[i].total_j;  b_val = a->pids[j].total_j; break;
            default: a_val = a->pids[i].total_w;  b_val = a->pids[j].total_w; break;
            }
            if (b_val > a_val) {
                PidRow tmp = a->pids[i]; a->pids[i] = a->pids[j]; a->pids[j] = tmp;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * RENDER HELPERS
 * ═══════════════════════════════════════════════════════════════ */
static const char *watt_color(double w) {
    if (w > 10.0) return RE;
    if (w > 3.0)  return OR;
    if (w > 1.0)  return AM;
    if (w > 0.1)  return GR;
    return DM;
}

static void render_watt(FluxSB *sb, double w) {
    flux_sb_appendf(sb, "%s%s%6.2f W" RS, watt_color(w), BD, w);
}

static void render_energy(FluxSB *sb, double j) {
    if (j > 1000.0) flux_sb_appendf(sb, PU "%7.1f kJ" RS, j / 1000.0);
    else            flux_sb_appendf(sb, PU "%7.2f J" RS, j);
}

/* ═══════════════════════════════════════════════════════════════
 * TAB BAR
 * ═══════════════════════════════════════════════════════════════ */
static void render_tabbar(App *a, FluxSB *sb) {
    flux_sb_append(sb, BG0 DM "  ");
    for (int i = 0; i < SCR_N; i++) {
        if (i == a->screen)
            flux_sb_appendf(sb, RS BG2 CY BD " %s %s " RS BG0 DM, TAB_ICONS[i], TAB_LABELS[i]);
        else
            flux_sb_appendf(sb, " %s %s ", TAB_ICONS[i], TAB_LABELS[i]);
        if (i < SCR_N - 1) flux_sb_append(sb, "│");
    }
    flux_sb_append(sb, RS BG0 "  ");
    if (a->connected)
        flux_sb_appendf(sb, "%s%s⚡ %.1f W" RS, watt_color(a->sys_total_w), BD, a->sys_total_w);
    else
        flux_sb_append(sb, RE BD "✗ disconnected" RS);
    flux_sb_appendf(sb, DM "  [1-%d] Tab q" RS "\n", SCR_N);
    flux_sb_append(sb, DM);
    int W = flux_cols();
    for (int i = 0; i < W; i++) flux_sb_append(sb, "─");
    flux_sb_append(sb, RS "\n");
}

/* ═══════════════════════════════════════════════════════════════
 * SCREEN 0: OVERVIEW
 * ═══════════════════════════════════════════════════════════════ */
static void render_overview(App *a, FluxSB *sb) {
    flux_sb_append(sb, "\n");
    flux_sb_appendf(sb, "  " AM BD "⚡ Power Overview" RS DM "   %s   %u cores  %u pkgs" RS "\n",
        a->sysinfo.vendor == POWMON_VENDOR_INTEL ? "Intel" :
        a->sysinfo.vendor == POWMON_VENDOR_AMD   ? "AMD" : "?",
        a->sysinfo.nr_cores, a->sysinfo.nr_packages);
    flux_sb_appendf(sb, DM "  tracking %u PIDs   sample: %ums   uptime: %.0fs" RS "\n\n",
        a->sysinfo.nr_tracked_pids, a->sysinfo.sample_interval_ms,
        (double)a->sysinfo.uptime_ns / 1e9);

    float scale = 150.0f;

    /* PKG row */
    flux_sb_appendf(sb, "  " CY BD "%-6s" RS "  ", "PKG");
    render_watt(sb, a->sys_pkg_w);
    flux_sb_append(sb, "  ");
    flux_bar(sb, (float)(a->sys_pkg_w / scale), 20, CY, DM);
    flux_sb_append(sb, "  ");
    flux_sparkline(sb, a->spark_pkg.ring, SPARK_LEN, a->spark_pkg.head, CY);
    flux_sb_append(sb, "  "); render_energy(sb, a->sys_pkg_j);
    flux_sb_nl(sb);

    /* DRAM row (only if domain exists) */
    if (has_dram(a)) {
        flux_sb_appendf(sb, "  " PU BD "%-6s" RS "  ", "DRAM");
        render_watt(sb, a->sys_dram_w);
        flux_sb_append(sb, "  ");
        flux_bar(sb, (float)(a->sys_dram_w / scale), 20, PU, DM);
        flux_sb_append(sb, "  ");
        flux_sparkline(sb, a->spark_dram.ring, SPARK_LEN, a->spark_dram.head, PU);
        flux_sb_append(sb, "  "); render_energy(sb, a->sys_dram_j);
        flux_sb_nl(sb);
    }

    /* TOTAL row */
    flux_sb_appendf(sb, "  " AM BD "%-6s" RS "  ", "TOTAL");
    render_watt(sb, a->sys_total_w);
    flux_sb_append(sb, "  ");
    flux_bar(sb, (float)(a->sys_total_w / scale), 20, AM, DM);
    flux_sb_append(sb, "  ");
    flux_sparkline(sb, a->spark_total.ring, SPARK_LEN, a->spark_total.head, AM);
    flux_sb_appendf(sb, "  " RE "peak %.1f W" RS, a->peak_total_w);
    flux_sb_nl(sb);

    flux_sb_append(sb, "\n  "); flux_divider(sb, 80, DM); flux_sb_append(sb, "\n\n");

    /* per-package boxes */
    char box_bufs[POWMON_MAX_PACKAGES][2048];
    const char *panels[POWMON_MAX_PACKAGES];
    int widths[POWMON_MAX_PACKAGES];
    int npkg = (int)a->sysinfo.nr_packages;
    if (npkg > (int)POWMON_MAX_PACKAGES) npkg = POWMON_MAX_PACKAGES;

    for (int i = 0; i < npkg; i++) {
        char c[512]; c[0] = '\0';
        char t[128];
        snprintf(t, sizeof t, CY BD " Package %d" RS "\n\n", i);
        strncat(c, t, sizeof c - strlen(c) - 1);
        snprintf(t, sizeof t, WH "  PKG   %s%6.2f W" RS "\n", watt_color(a->pkg_w[i]), a->pkg_w[i]);
        strncat(c, t, sizeof c - strlen(c) - 1);
        snprintf(t, sizeof t, WH "  CORE  %s%6.2f W" RS "\n", watt_color(a->core_w[i]), a->core_w[i]);
        strncat(c, t, sizeof c - strlen(c) - 1);
        if (has_dram(a)) {
            snprintf(t, sizeof t, WH "  DRAM  %s%6.2f W" RS "\n", watt_color(a->dram_w[i]), a->dram_w[i]);
            strncat(c, t, sizeof c - strlen(c) - 1);
        }
        if (has_dram(a))
            snprintf(t, sizeof t, "\n" DM "  E: %.1fJ / %.1fJ" RS, a->pkg_j[i], a->dram_j[i]);
        else
            snprintf(t, sizeof t, "\n" DM "  E: %.1fJ" RS, a->pkg_j[i]);
        strncat(c, t, sizeof c - strlen(c) - 1);
        flux_box(box_bufs[i], sizeof box_bufs[i], c, &FLUX_BORDER_ROUNDED, 24, CY, NULL);
        panels[i] = box_bufs[i]; widths[i] = 28;
    }
    if (npkg > 0) {
        FluxSB hsb; char hbuf[8192]; flux_sb_init(&hsb, hbuf, sizeof hbuf);
        flux_hbox(&hsb, panels, widths, npkg, "  ");
        char *s = hbuf;
        while (*s) {
            flux_sb_append(sb, "  ");
            char *nl = strchr(s, '\n');
            if (nl) { *nl = '\0'; flux_sb_append(sb, s); flux_sb_nl(sb); s = nl+1; }
            else    { flux_sb_append(sb, s); flux_sb_nl(sb); break; }
        }
    }

    /* top 5 consumers */
    flux_sb_append(sb, "\n  " WH BD "Top consumers:" RS "\n");
    int show = a->pid_count < 5 ? a->pid_count : 5;
    for (int i = 0; i < show; i++) {
        PidRow *p = &a->pids[i];
        flux_sb_appendf(sb, "  %s%s  %-7d %-16s" RS "  ", i==0?RE:i<3?AM:DM, BD, p->pid, p->comm);
        render_watt(sb, p->total_w);
        flux_sb_nl(sb);
    }
    if (a->pid_count == 0) flux_sb_append(sb, DM "  (waiting for data...)\n" RS);
}

/* ═══════════════════════════════════════════════════════════════
 * SCREEN 1: PROCESSES
 * ═══════════════════════════════════════════════════════════════ */
static void render_procs(App *a, FluxSB *sb) {
    static const char *sort_labels_dram[] = {"Total(W)","CPU(W)","DRAM(W)","Energy(J)"};
    static const char *sort_labels_nodram[] = {"Total(W)","CPU(W)","Energy(J)"};
    int dram = has_dram(a);
    int nsorts = dram ? 4 : 3;
    const char **sort_labels = dram ? sort_labels_dram : sort_labels_nodram;
    int sort_idx = a->proc_sort % nsorts;
    flux_sb_append(sb, "\n");
    flux_sb_appendf(sb, "  " GR BD "◉ Process Power" RS DM "   %d tracked   sort: " AM BD "%s" RS DM
        "   [s] sort  ↑↓/jk scroll\n\n" RS, a->pid_count, sort_labels[sort_idx]);

    if (dram)
        flux_sb_appendf(sb, "  " DM "%-7s  %-16s  %9s  %9s  %9s  %10s  %8s" RS "\n",
            "PID","COMMAND","CPU(W)","DRAM(W)","TOTAL(W)","ENERGY(J)","CPU(s)");
    else
        flux_sb_appendf(sb, "  " DM "%-7s  %-16s  %9s  %9s  %10s  %8s" RS "\n",
            "PID","COMMAND","CPU(W)","TOTAL(W)","ENERGY(J)","CPU(s)");
    flux_sb_append(sb, "  " DM);
    int hdrw = dram ? 78 : 68;
    for (int i = 0; i < hdrw; i++) flux_sb_append(sb, "─");
    flux_sb_append(sb, RS "\n");

    int vis = flux_rows() - 10; if (vis < 5) vis = 5;
    if (a->proc_scroll > a->pid_count - vis) a->proc_scroll = a->pid_count - vis;
    if (a->proc_scroll < 0) a->proc_scroll = 0;
    if (a->proc_cursor >= a->pid_count) a->proc_cursor = a->pid_count - 1;
    if (a->proc_cursor < 0) a->proc_cursor = 0;

    for (int i = a->proc_scroll; i < a->pid_count && i < a->proc_scroll + vis; i++) {
        PidRow *p = &a->pids[i];
        int cur = (i == a->proc_cursor);
        if (cur) flux_sb_append(sb, BGX);
        flux_sb_appendf(sb, "  %-7d  " WH "%-16s" RS, p->pid, p->comm);
        if (cur) flux_sb_append(sb, BGX);
        flux_sb_appendf(sb, "  %s%9.3f" RS, watt_color(p->cpu_w), p->cpu_w);
        if (dram) {
            if (cur) flux_sb_append(sb, BGX);
            flux_sb_appendf(sb, "  %s%9.3f" RS, watt_color(p->dram_w), p->dram_w);
        }
        if (cur) flux_sb_append(sb, BGX);
        flux_sb_appendf(sb, "  %s%s%9.3f" RS, watt_color(p->total_w), BD, p->total_w);
        if (cur) flux_sb_append(sb, BGX);
        flux_sb_appendf(sb, "  " PU "%10.3f" RS, p->total_j);
        if (cur) flux_sb_append(sb, BGX);
        flux_sb_appendf(sb, "  " DM "%8.1f" RS, p->cpu_time_s);
        if (cur) flux_sb_append(sb, RS);
        flux_sb_nl(sb);
    }
    flux_sb_append(sb, "  " DM);
    for (int i = 0; i < hdrw; i++) flux_sb_append(sb, "─");
    flux_sb_append(sb, RS "\n");
    int end = a->proc_scroll + vis;
    if (end > a->pid_count) end = a->pid_count;
    flux_sb_appendf(sb, DM "  %d-%d of %d" RS "\n", a->proc_scroll+1, end, a->pid_count);
}

/* ═══════════════════════════════════════════════════════════════
 * SCREEN 2: CORES
 * ═══════════════════════════════════════════════════════════════ */
static void render_cores(App *a, FluxSB *sb) {
    flux_sb_append(sb, "\n");
    flux_sb_appendf(sb, "  " BL BD "▦ Core Power Map" RS DM "   %u cores" RS "\n\n", a->sysinfo.nr_cores);

    static const char *heat[] = {
        "\x1b[38;2;40;60;90m", "\x1b[38;2;50;120;180m",
        "\x1b[38;2;80;200;160m", "\x1b[38;2;180;220;80m",
        "\x1b[38;2;255;180;50m", "\x1b[38;2;255;80;60m",
    };

    /* legend */
    flux_sb_append(sb, "  " DM "0W " RS);
    for (int i = 0; i < 6; i++) flux_sb_appendf(sb, "%s██" RS, heat[i]);
    flux_sb_appendf(sb, " " RE BD "%.1fW" RS "\n\n", a->core_max_w > 0 ? a->core_max_w : 1.0);

    uint32_t nc = a->sysinfo.nr_cores;
    if (nc > POWMON_MAX_CORES) nc = POWMON_MAX_CORES;
    int cols = 16;

    for (uint32_t c = 0; c < nc; c++) {
        if (c % cols == 0) flux_sb_append(sb, "  ");
        double w = a->core_power[c];
        double mx = a->core_max_w > 0.001 ? a->core_max_w : 1.0;
        int ci = (int)((w / mx) * 5.0); if (ci > 5) ci = 5; if (ci < 0) ci = 0;
        flux_sb_appendf(sb, "%s██" RS, heat[ci]);
        if ((c+1) % cols == 0 || c == nc-1) {
            flux_sb_nl(sb);
            flux_sb_append(sb, "  " DM);
            uint32_t start = c - (c % cols);
            for (uint32_t j = start; j <= c; j++) flux_sb_appendf(sb, "%-2u", j);
            flux_sb_append(sb, RS "\n\n");
        }
    }

    flux_sb_append(sb, "  " WH BD "Hottest cores:" RS "\n");
    int top[8] = {0}; int ntop = (int)nc < 8 ? (int)nc : 8;
    for (int t = 0; t < ntop; t++) {
        double best = -1; int bi = 0;
        for (uint32_t c = 0; c < nc; c++) {
            int skip = 0;
            for (int k = 0; k < t; k++) if (top[k] == (int)c) skip = 1;
            if (!skip && a->core_power[c] > best) { best = a->core_power[c]; bi = (int)c; }
        }
        top[t] = bi;
    }
    for (int t = 0; t < ntop; t++) {
        int c = top[t]; double w = a->core_power[c];
        flux_sb_appendf(sb, "  Core %-3d  ", c);
        render_watt(sb, w);
        flux_sb_append(sb, "  ");
        float mx = a->core_max_w > 0.001 ? (float)a->core_max_w : 1.0f;
        flux_bar(sb, (float)(w / mx), 30, watt_color(w), DM);
        flux_sb_nl(sb);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * SCREEN 3: CONFIG
 * ═══════════════════════════════════════════════════════════════ */
static void render_config(App *a, FluxSB *sb) {
    flux_sb_append(sb, "\n");
    flux_sb_append(sb, "  " PU BD "⚙ Configuration" RS "\n\n");

    /* module info box */
    char ic[1024]; ic[0] = '\0'; char t[256];
    static const char *dnames[] = {"PKG","CORE","UNCORE","DRAM","PSYS"};

    snprintf(t, sizeof t, WH BD " Module Info" RS "\n\n"); strncat(ic, t, sizeof ic - strlen(ic) - 1);
    snprintf(t, sizeof t, DM "  Device:   " WH "/dev/powmon" RS "\n"); strncat(ic, t, sizeof ic - strlen(ic) - 1);
    snprintf(t, sizeof t, DM "  Status:   %s" RS "\n", a->connected ? GR BD "connected" RS : RE BD "disconnected" RS);
    strncat(ic, t, sizeof ic - strlen(ic) - 1);
    snprintf(t, sizeof t, DM "  Vendor:   " WH "%s" RS "\n",
        a->sysinfo.vendor == POWMON_VENDOR_INTEL ? "Intel" : a->sysinfo.vendor == POWMON_VENDOR_AMD ? "AMD" : "?");
    strncat(ic, t, sizeof ic - strlen(ic) - 1);
    snprintf(t, sizeof t, DM "  Packages: " WH "%u" RS "\n", a->sysinfo.nr_packages);
    strncat(ic, t, sizeof ic - strlen(ic) - 1);
    snprintf(t, sizeof t, DM "  Cores:    " WH "%u" RS "\n", a->sysinfo.nr_cores);
    strncat(ic, t, sizeof ic - strlen(ic) - 1);
    snprintf(t, sizeof t, DM "  Tracked:  " WH "%u PIDs" RS "\n", a->sysinfo.nr_tracked_pids);
    strncat(ic, t, sizeof ic - strlen(ic) - 1);
    snprintf(t, sizeof t, DM "  RAPL ESU: " WH "1/2^%u J" RS "\n\n", a->sysinfo.rapl_unit_pkg);
    strncat(ic, t, sizeof ic - strlen(ic) - 1);
    snprintf(t, sizeof t, DM "  Domains:  " RS); strncat(ic, t, sizeof ic - strlen(ic) - 1);
    for (int d = 0; d < POWMON_DOMAIN_COUNT; d++) {
        if (a->sysinfo.domain_mask & (1 << d)) {
            snprintf(t, sizeof t, GR "%s " RS, dnames[d]);
            strncat(ic, t, sizeof ic - strlen(ic) - 1);
        }
    }

    char ibox[4096]; flux_box(ibox, sizeof ibox, ic, &FLUX_BORDER_ROUNDED, 40, PU, NULL);
    char *s = ibox;
    while (*s) {
        flux_sb_append(sb, "  ");
        char *nl = strchr(s, '\n');
        if (nl) { *nl = '\0'; flux_sb_append(sb, s); flux_sb_nl(sb); s = nl+1; }
        else    { flux_sb_append(sb, s); flux_sb_nl(sb); break; }
    }
    flux_sb_append(sb, "\n");

    /* settings box */
    char sc[1024]; sc[0] = '\0';
    snprintf(t, sizeof t, WH BD " Settings" RS "\n\n"); strncat(sc, t, sizeof sc - strlen(sc) - 1);

    snprintf(t, sizeof t, "%s  Sample interval: " RS AM BD "%u ms" RS "\n",
        a->config_focus == 0 ? CY BD : DM, a->sysinfo.sample_interval_ms);
    strncat(sc, t, sizeof sc - strlen(sc) - 1);

    snprintf(t, sizeof t, DM "  New value: " RS);
    strncat(sc, t, sizeof sc - strlen(sc) - 1);
    if (a->config_focus == 0) {
        if (a->interval_input.len > 0)
            snprintf(t, sizeof t, CY BD "%.5s" RS "\x1b[7m \x1b[0m\n\n", a->interval_input.buf);
        else
            snprintf(t, sizeof t, DM IT "10-10000" RS "\x1b[7m \x1b[0m\n\n");
    } else {
        if (a->interval_input.len > 0)
            snprintf(t, sizeof t, DM "%.5s" RS "\n\n", a->interval_input.buf);
        else
            snprintf(t, sizeof t, DM IT "10-10000" RS "\n\n");
    }
    strncat(sc, t, sizeof sc - strlen(sc) - 1);

    snprintf(t, sizeof t, "  %s[ Apply ]" RS "\n\n",
        a->config_focus == 1 ? GR BD : DM);
    strncat(sc, t, sizeof sc - strlen(sc) - 1);

    snprintf(t, sizeof t, "  %sTrack all: %s" RS "\n\n",
        a->config_focus == 2 ? CY BD : DM,
        a->track_mode ? GR BD "ON" RS : RE "OFF" RS);
    strncat(sc, t, sizeof sc - strlen(sc) - 1);

    strncat(sc, "  " DM "[ " RS RE "Reset Stats" RS DM " ] press R" RS "\n",
        sizeof sc - strlen(sc) - 1);

    char sbox[4096]; flux_box(sbox, sizeof sbox, sc, &FLUX_BORDER_ROUNDED, 40, AM, NULL);
    s = sbox;
    while (*s) {
        flux_sb_append(sb, "  ");
        char *nl = strchr(s, '\n');
        if (nl) { *nl = '\0'; flux_sb_append(sb, s); flux_sb_nl(sb); s = nl+1; }
        else    { flux_sb_append(sb, s); flux_sb_nl(sb); break; }
    }

    if (a->status_msg[0] && a->status_tick > 0)
        flux_sb_appendf(sb, "\n  " GR "%s" RS "\n", a->status_msg);

    flux_sb_appendf(sb, "\n  " DM "Tab/↑↓: cycle   Enter: confirm   R: reset" RS "\n");
}

/* ═══════════════════════════════════════════════════════════════
 * DISCONNECTED
 * ═══════════════════════════════════════════════════════════════ */
static void render_disconnected(FluxSB *sb) {
    flux_sb_append(sb, "\n\n");
    char c[512];
    snprintf(c, sizeof c,
        RE BD "  ✗ Cannot open /dev/powmon" RS "\n\n"
        WH "  The powmon kernel module is not loaded." RS "\n\n"
        DM "  To load it:" RS "\n"
        CY "    sudo insmod powmon.ko track_all=1" RS "\n\n"
        WH BD "  Press [r] to retry, [q] to quit" RS);
    char box[2048]; flux_box(box, sizeof box, c, &FLUX_BORDER_DOUBLE, 48, RE, NULL);
    char *s = box;
    while (*s) {
        int pad = (flux_cols() - 52) / 2; if (pad < 0) pad = 0;
        for (int i = 0; i < pad; i++) flux_sb_append(sb, " ");
        char *nl = strchr(s, '\n');
        if (nl) { *nl = '\0'; flux_sb_append(sb, s); flux_sb_nl(sb); s = nl+1; }
        else    { flux_sb_append(sb, s); flux_sb_nl(sb); break; }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * VIEW
 * ═══════════════════════════════════════════════════════════════ */
static void app_view(FluxModel *m, char *buf, int sz) {
    App *a = m->state;
    FluxSB sb; flux_sb_init(&sb, buf, sz);
    render_tabbar(a, &sb);
    if (!a->connected) { render_disconnected(&sb); return; }
    switch (a->screen) {
    case SCR_OVERVIEW: render_overview(a, &sb); break;
    case SCR_PROCS:    render_procs(a, &sb);    break;
    case SCR_CORES:    render_cores(a, &sb);    break;
    case SCR_CONFIG:   render_config(a, &sb);   break;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * TICK
 * ═══════════════════════════════════════════════════════════════ */
typedef struct { int ms; int id; } _TickCtx;
static FluxMsg _tick_fn(void *ctx) {
    _TickCtx *t = (_TickCtx*)ctx;
    int ms = t->ms; int id = t->id; free(t);
    struct timespec ts = { ms/1000, (ms%1000)*1000000L }; nanosleep(&ts, NULL);
    FluxMsg m; memset(&m, 0, sizeof m);
    m.type = MSG_CUSTOM; m.u.custom.id = id; return m;
}
static FluxCmd tick_poll(int ms) {
    _TickCtx *c = malloc(sizeof *c); c->ms = ms; c->id = MSG_TICK_POLL;
    return (FluxCmd){ _tick_fn, c };
}

/* ═══════════════════════════════════════════════════════════════
 * UPDATE
 * ═══════════════════════════════════════════════════════════════ */
static FluxCmd app_update(FluxModel *m, FluxMsg msg) {
    App *a = m->state;

    if (msg.type == MSG_KEY) {
        /* config input intercept */
        if (a->screen == SCR_CONFIG && a->config_focus == 0) {
            if (flux_input_update(&a->interval_input, msg)) {
                if (a->interval_input.submitted) a->config_focus = 1;
                return FLUX_CMD_NONE;
            }
        }

        if (flux_key_is(msg,"ctrl+c") || flux_key_is(msg,"q")) return FLUX_CMD_QUIT;
        if (flux_key_is(msg,"tab")) { a->screen = (a->screen+1) % SCR_N; return FLUX_CMD_NONE; }
        if (flux_key_is(msg,"shift+tab")) { a->screen = (a->screen-1+SCR_N) % SCR_N; return FLUX_CMD_NONE; }
        if (msg.u.key.key[0]>='1' && msg.u.key.key[0]<='0'+SCR_N && msg.u.key.key[1]=='\0')
            { a->screen = msg.u.key.key[0]-'1'; return FLUX_CMD_NONE; }

        if (flux_key_is(msg,"r") && !a->connected) { dev_open(a); return FLUX_CMD_NONE; }

        switch (a->screen) {
        case SCR_PROCS:
            if (flux_key_is(msg,"up")||flux_key_is(msg,"k")) {
                if (a->proc_cursor > 0) { a->proc_cursor--;
                    if (a->proc_cursor < a->proc_scroll) a->proc_scroll--; }
            }
            if (flux_key_is(msg,"down")||flux_key_is(msg,"j")) {
                if (a->proc_cursor < a->pid_count-1) { a->proc_cursor++;
                    int vis = flux_rows()-10; if(vis<5)vis=5;
                    if (a->proc_cursor >= a->proc_scroll+vis) a->proc_scroll = a->proc_cursor-vis+1; }
            }
            if (flux_key_is(msg,"s")) {
                int ns = has_dram(a) ? 4 : 3;
                a->proc_sort = (a->proc_sort+1) % ns;
            }
            break;
        case SCR_CONFIG:
            if (flux_key_is(msg,"tab")||flux_key_is(msg,"down")||flux_key_is(msg,"j"))
                a->config_focus = (a->config_focus+1) % 3;
            if (flux_key_is(msg,"shift+tab")||flux_key_is(msg,"up")||flux_key_is(msg,"k"))
                a->config_focus = (a->config_focus+2) % 3;
            if (flux_key_is(msg,"enter")) {
                if (a->config_focus == 1 && a->interval_input.len > 0 && a->connected) {
                    int val = atoi(a->interval_input.buf);
                    struct powmon_config cfg = { .sample_interval_ms = (uint32_t)val, .flags = 0 };
                    if (ioctl(a->dev_fd, POWMON_IOC_SET_CONFIG, &cfg) == 0) {
                        snprintf(a->status_msg, sizeof a->status_msg, "✓ Interval → %d ms", val);
                        a->status_tick = 30; flux_input_clear(&a->interval_input);
                    } else {
                        snprintf(a->status_msg, sizeof a->status_msg, "✗ Invalid (10-10000)");
                        a->status_tick = 30;
                    }
                }
                if (a->config_focus == 2 && a->connected) {
                    a->track_mode ^= 1;
                    if (a->track_mode) ioctl(a->dev_fd, POWMON_IOC_TRACK_ALL);
                }
            }
            if (flux_key_is(msg,"R") || (flux_key_is(msg,"r") && a->connected)) {
                ioctl(a->dev_fd, POWMON_IOC_RESET_STATS);
                a->peak_total_w = 0;
                memset(&a->spark_pkg, 0, sizeof a->spark_pkg);
                memset(&a->spark_dram, 0, sizeof a->spark_dram);
                memset(&a->spark_total, 0, sizeof a->spark_total);
                snprintf(a->status_msg, sizeof a->status_msg, "✓ Stats reset");
                a->status_tick = 30;
            }
            break;
        }
        return FLUX_CMD_NONE;
    }

    if (msg.type == MSG_CUSTOM && msg.u.custom.id == MSG_TICK_POLL) {
        dev_poll(a);
        a->tick++;
        if (a->status_tick > 0) a->status_tick--;
        return tick_poll(a->poll_ms);
    }
    return FLUX_CMD_NONE;
}

/* ═══════════════════════════════════════════════════════════════
 * INIT / FREE
 * ═══════════════════════════════════════════════════════════════ */
static FluxCmd app_init(FluxModel *m) {
    App *a = m->state;
    dev_open(a);
    if (a->connected) dev_poll(a);
    return tick_poll(a->poll_ms);
}
static void app_free(FluxModel *m) {
    App *a = m->state;
    if (a->dev_fd >= 0) close(a->dev_fd);
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════ */
int main(void) {
    static App state;
    memset(&state, 0, sizeof state);
    state.screen  = SCR_OVERVIEW;
    state.poll_ms = 500;
    state.dev_fd  = -1;
    flux_input_init(&state.interval_input, "10-10000");

    FluxModel model = {
        .state = &state, .init = app_init,
        .update = app_update, .view = app_view, .free = app_free,
    };
    FluxProgram prog = { .model = model, .alt_screen = 1, .mouse = 0, .fps = 30 };
    flux_run(&prog);
    return 0;
}
