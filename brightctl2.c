/*
 * brightctl2 —— macOS 亮度控制 CLI（核心逻辑见 brightctl_core.c）
 *
 * 输出语言：默认跟随系统（zh/en），可用 --lang zh|en 或环境变量
 * BRIGHTCTL_LANG 强制指定。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <CoreGraphics/CoreGraphics.h>

#include "brightctl_core.h"
#include "brightctl_sysmon.h"

static bctl_display g_list[BCTL_MAX_DISPLAYS];
static int g_n;
static volatile sig_atomic_t blank_stop;
static void stop_blank(int sig) { (void)sig; blank_stop = 1; }

static void load(void) {
    bctl_init();
    g_n = bctl_probe(g_list, BCTL_MAX_DISPLAYS);
    if (g_n <= 0) {
        fprintf(stderr, "%s\n", bctl_t(BCTL_T_READ_FAIL));
        exit(2);
    }
}

static bctl_display *by_id(uint32_t id) {
    for (int i = 0; i < g_n; i++) if (g_list[i].display_id == id) return &g_list[i];
    return NULL;
}

static bool parse_id(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0') return false;
    *out = (uint32_t)v;
    return true;
}

static void cmd_list(void) {
    for (int i = 0; i < g_n; i++) {
        bctl_display *d = &g_list[i];
        printf("display id=0x%x", d->display_id);
        if (d->is_main) printf(" (%s)", bctl_t(BCTL_T_MAIN));
        printf(" (%s)  brightness=",
               d->is_builtin ? bctl_t(BCTL_T_BUILTIN) : bctl_t(BCTL_T_EXTERNAL));
        if (d->percent < 0) printf("%s", bctl_t(BCTL_T_UNREADABLE));
        else                printf("%.1f%%", (double)d->percent);
        const char *ch = (d->channel == BCTL_CHANNEL_DS)  ? "DisplayServices"
                       : (d->channel == BCTL_CHANNEL_DDC) ? "DDC"
                       : bctl_t(BCTL_T_CH_NONE);
        printf("  channel=%s\n", ch);
    }
}

static void fmt_bytes_rate(double bps, char *buf, size_t len) {
    if (bps < 0) { snprintf(buf, len, "-"); return; }
    if (bps < 1024)                snprintf(buf, len, "%.0f B/s", bps);
    else if (bps < 1024 * 1024)    snprintf(buf, len, "%.1f KB/s", bps / 1024);
    else                           snprintf(buf, len, "%.1f MB/s", bps / 1048576);
}

static void fmt_uptime(double sec, char *buf, size_t len) {
    if (sec < 0) { snprintf(buf, len, "-"); return; }
    long s = (long)sec;
    long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
    if (d > 0)      snprintf(buf, len, "%ldd %ldh", d, h);
    else if (h > 0) snprintf(buf, len, "%ldh %ldm", h, m);
    else            snprintf(buf, len, "%ldm", m);
}

static void cmd_sys(void) {
    bctl_sysmon_init();
    bctl_sysinfo a, b;
    /* 两次采样之间留出间隔，CPU 占用率与网络速率才有意义 */
    bctl_sysmon_sample(&a);
    usleep(400000);
    bctl_sysmon_sample(&b);

    printf("%s\n", bctl_t(BCTL_T_SYS_TITLE));

    if (b.cpu_percent >= 0)
        printf("  %-6s %5.1f%%   (%d/%d)\n", bctl_t(BCTL_T_SYS_CPU), b.cpu_percent,
               b.core_count, b.cpu_count);
    else
        printf("  %-6s %s\n", bctl_t(BCTL_T_SYS_CPU), bctl_t(BCTL_T_SYS_UNAVAILABLE));

    if (b.gpu_percent >= 0)
        printf("  %-6s %5.1f%%\n", bctl_t(BCTL_T_SYS_GPU), b.gpu_percent);

    if (b.mem_percent >= 0)
        printf("  %-6s %5.1f%%   (%.0f/%.0f MB)\n", bctl_t(BCTL_T_SYS_MEM), b.mem_percent,
               b.mem_used_mb, b.mem_total_mb);
    else
        printf("  %-6s %s\n", bctl_t(BCTL_T_SYS_MEM), bctl_t(BCTL_T_SYS_UNAVAILABLE));

    if (b.temp_available)
        printf("  %-6s %5.1f°C   (GPU %.1f°C)\n",
               bctl_t(BCTL_T_SYS_TEMP), b.cpu_temp, b.gpu_temp);
    else
        printf("  %-6s %s\n", bctl_t(BCTL_T_SYS_TEMP), bctl_t(BCTL_T_SYS_UNAVAILABLE));

    if (b.disk_percent >= 0)
        printf("  %-6s %5.1f%%   (%.0f/%.0f GB)\n", bctl_t(BCTL_T_SYS_DISK), b.disk_percent,
               b.disk_used_gb, b.disk_total_gb);

    if (b.has_battery && b.battery_percent >= 0) {
        const char *state = b.battery_charging ? bctl_t(BCTL_T_SYS_CHARGING)
                          : b.battery_on_ac   ? bctl_t(BCTL_T_SYS_ON_AC) : "";
        printf("  %-6s %5.1f%%   %s\n", bctl_t(BCTL_T_SYS_BATTERY), b.battery_percent, state);
    }

    if (b.net_available && (b.net_rx_bps >= 0 || b.net_tx_bps >= 0)) {
        char rx[32], tx[32];
        fmt_bytes_rate(b.net_rx_bps, rx, sizeof rx);
        fmt_bytes_rate(b.net_tx_bps, tx, sizeof tx);
        printf("  %-6s ↓%s  ↑%s\n", bctl_t(BCTL_T_SYS_NET), rx, tx);
    }

    if (b.load1 >= 0)
        printf("  %-6s %.2f  %.2f  %.2f\n", bctl_t(BCTL_T_SYS_LOAD),
               b.load1, b.load5, b.load15);

    {
        char up[32];
        fmt_uptime(b.uptime_seconds, up, sizeof up);
        printf("  %-6s %s\n", bctl_t(BCTL_T_SYS_UPTIME), up);
    }

    if (b.process_count > 0)
        printf("  %-6s %d\n", bctl_t(BCTL_T_SYS_PROC), b.process_count);
}

int main(int argc, char **argv) {
    /* 先剥离 --lang，保证之后所有文案都按目标语言输出 */
    const char *lang = NULL;
    int base = 1;
    while (base < argc) {
        if (!strcmp(argv[base], "--lang")) {
            if (base + 1 >= argc) {
                fprintf(stderr, "--lang needs zh|en\n");
                return 2;
            }
            lang = argv[base + 1];
            base += 2;
        } else if (!strncmp(argv[base], "--lang=", 7)) {
            lang = argv[base] + 7;
            base += 1;
        } else {
            break;
        }
    }
    bctl_set_language(lang);

    if (base < argc && (!strcmp(argv[base], "-h") || !strcmp(argv[base], "--help") ||
                        !strcmp(argv[base], "help"))) {
        printf("%s", bctl_t(BCTL_T_USAGE));
        return 0;
    }

    /* sys 不依赖显示器，先于 load() 处理，避免没有可用屏幕时被拦下 */
    if (base < argc && !strcmp(argv[base], "sys")) { cmd_sys(); return 0; }

    if (base + 1 < argc && !strcmp(argv[base], "blank") && !strcmp(argv[base + 1], "off")) {
        bctl_init();
        bool ok = bctl_builtin_blank(false);
        fprintf(ok ? stdout : stderr, "%s\n", bctl_t(ok ? BCTL_T_BLANK_OFF : BCTL_T_BLANK_FAIL_OFF));
        return ok ? 0 : 1;
    }
    load();

    if (base >= argc || !strcmp(argv[base], "list")) { cmd_list(); return 0; }

    const char *cmd = argv[base];
    const char *a1 = (base + 1 < argc) ? argv[base + 1] : NULL;
    const char *a2 = (base + 2 < argc) ? argv[base + 2] : NULL;
    if (!strcmp(cmd, "get")) {
        if (!a1) { fprintf(stderr, "%s\n", bctl_t(BCTL_T_NEED_ID)); return 2; }
        uint32_t id;
        if (!parse_id(a1, &id)) {
            fprintf(stderr, "%s: %s\n", bctl_t(BCTL_T_BAD_ID), a1);
            return 2;
        }
        if (!by_id(id)) {
            fprintf(stderr, "%s: 0x%x\n", bctl_t(BCTL_T_NO_DISPLAY), id);
            return 1;
        }
        float p;
        if (!bctl_get_percent(id, &p)) {
            fprintf(stderr, "%s\n", bctl_t(BCTL_T_READ_FAIL));
            return 1;
        }
        printf("%.1f\n", (double)p);
        return 0;
    }

    if (!strcmp(cmd, "set")) {
        if (!a2) { fprintf(stderr, "%s\n", bctl_t(BCTL_T_NEED_VALUE)); return 2; }
        uint32_t id;
        if (!parse_id(a1, &id)) {
            fprintf(stderr, "%s: %s\n", bctl_t(BCTL_T_BAD_ID), a1);
            return 2;
        }
        /* 先校验数值语法：参数写错属于用法错误，不应受屏幕当前状态影响 */
        char *end = NULL;
        double v = strtod(a2, &end);
        if (end == a2 || *end != '\0') {
            fprintf(stderr, "%s: %s\n", bctl_t(BCTL_T_BAD_VALUE), a2);
            return 2;
        }
        bool relative = (a2[0] == '+' || a2[0] == '-');
        if (!relative && (v < 0 || v > 100)) {
            fprintf(stderr, "%s: %s\n", bctl_t(BCTL_T_RANGE), a2);
            return 2;
        }

        bctl_display *d = by_id(id);
        if (!d) {
            fprintf(stderr, "%s: 0x%x\n", bctl_t(BCTL_T_NO_DISPLAY), id);
            return 1;
        }
        if (d->channel == BCTL_CHANNEL_NONE) {
            fprintf(stderr, "%s: 0x%x\n", bctl_t(BCTL_T_CH_NONE), id);
            return 1;
        }
        float before;
        if (!bctl_get_percent(id, &before)) {
            fprintf(stderr, "%s\n", bctl_t(BCTL_T_READ_FAIL));
            return 1;
        }
        if (relative) {
            float after = bctl_step_percent(id, (float)v);
            if (after < 0) { fprintf(stderr, "%s\n", bctl_t(BCTL_T_WRITE_FAIL)); return 1; }
            printf("id=0x%x %.1f%% -> %.1f%%\n", id, (double)before, (double)after);
            return 0;
        }
        int target = (int)lround(v);
        if (target == (int)lroundf(before)) {
            printf("id=0x%x %.1f%% (%s)\n", id, (double)before, bctl_t(BCTL_T_UNCHANGED));
            return 0;
        }
        if (!bctl_set_percent(id, (float)target)) {
            fprintf(stderr, "%s\n", bctl_t(BCTL_T_WRITE_FAIL));
            return 1;
        }
        float after;
        bctl_get_percent(id, &after);
        printf("id=0x%x %.1f%% -> %.1f%%\n", id, (double)before, (double)after);
        return 0;
    }

    if (!strcmp(cmd, "up") || !strcmp(cmd, "down")) {
        int step = 5;
        if (a1) {
            char *end = NULL;
            long s = strtol(a1, &end, 10);
            if (end == a1 || *end != '\0') {
                fprintf(stderr, "%s: %s\n", bctl_t(BCTL_T_NEED_STEP), a1);
                return 2;
            }
            step = (int)s;
        }
        uint32_t id = bctl_primary_display_id();
        if (!id) { fprintf(stderr, "%s\n", bctl_t(BCTL_T_NO_CONTROLLABLE)); return 1; }
        if (step < 0) step = -step;
        float before, after;
        if (!bctl_get_percent(id, &before)) {
            fprintf(stderr, "%s\n", bctl_t(BCTL_T_READ_FAIL));
            return 1;
        }
        after = bctl_step_percent(id, cmd[0] == 'u' ? (float)step : (float)-step);
        if (after < 0) { fprintf(stderr, "%s\n", bctl_t(BCTL_T_WRITE_FAIL)); return 1; }
        printf("id=0x%x %.1f%% -> %.1f%%\n", id, (double)before, (double)after);
        return 0;
    }

    if (!strcmp(cmd, "sync")) {
        uint32_t ref = bctl_primary_display_id();
        if (!ref) { fprintf(stderr, "%s\n", bctl_t(BCTL_T_NO_CONTROLLABLE)); return 1; }
        float target;
        if (!bctl_get_percent(ref, &target)) {
            fprintf(stderr, "%s\n", bctl_t(BCTL_T_READ_FAIL));
            return 1;
        }
        int changed = 0;
        for (int i = 0; i < g_n; i++) {
            bctl_display *d = &g_list[i];
            if (d->display_id == ref || d->channel == BCTL_CHANNEL_NONE) continue;
            if (bctl_set_percent(d->display_id, target)) changed++;
        }
        printf(bctl_t(BCTL_T_SYNC_DONE), changed, ref, (double)target);
        printf("\n");
        return 0;
    }

    if (!strcmp(cmd, "all")) {
        if (!a1) { fprintf(stderr, "%s\n", bctl_t(BCTL_T_NEED_VALUE)); return 2; }
        char *end = NULL;
        double v = strtod(a1, &end);
        if (end == a1 || *end != '\0' || v < 0 || v > 100) {
            fprintf(stderr, "%s: %s\n", bctl_t(BCTL_T_BAD_VALUE), a1);
            return 2;
        }
        int changed = 0;
        for (int i = 0; i < g_n; i++) {
            if (g_list[i].channel == BCTL_CHANNEL_NONE) continue;
            if (bctl_set_percent(g_list[i].display_id, (float)v)) changed++;
        }
        if (!changed) { fprintf(stderr, "%s\n", bctl_t(BCTL_T_NO_CONTROLLABLE)); return 1; }
        printf(bctl_t(BCTL_T_ALL_DONE), changed, v);
        printf("\n");
        return 0;
    }

    if (!strcmp(cmd, "blank")) {
        bool on = true;
        if (a1) {
            if (!strcmp(a1, "off")) on = false;
            else if (!strcmp(a1, "on")) on = true;
            else { fprintf(stderr, "%s\n", bctl_t(BCTL_T_BLANK_ARG)); return 2; }
        }
        if (on) {
            signal(SIGINT, stop_blank);
            signal(SIGTERM, stop_blank);
        }
        if (!bctl_builtin_blank(on)) {
            fprintf(stderr, "%s\n", bctl_t(on ? BCTL_T_BLANK_FAIL_ON : BCTL_T_BLANK_FAIL_OFF));
            return 1;
        }
        if (!on) { printf("%s\n", bctl_t(BCTL_T_BLANK_OFF)); return 0; }
        printf("%s\n", bctl_t(BCTL_T_BLANK_ON));
        fflush(stdout);
        while (!blank_stop && bctl_builtin_has_restore_state()) {
            if (bctl_external_count() == 0) break;
            sleep(1);
        }
        bool restored = bctl_builtin_blank(false);
        printf("%s\n", bctl_t(restored ? BCTL_T_BLANK_OFF : BCTL_T_BLANK_FAIL_OFF));
        return restored ? 0 : 1;
    }

    fprintf(stderr, "%s: %s\n", bctl_t(BCTL_T_UNKNOWN_CMD), cmd);
    printf("%s", bctl_t(BCTL_T_USAGE));
    return 2;
}
