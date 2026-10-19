/*
 * brightctl2 —— macOS 亮度控制 CLI（核心逻辑见 brightctl_core.c）
 *
 * 用法:
 *   brightctl2                    列出显示器与当前亮度
 *   brightctl2 get <id>           读指定屏亮度（0-100）
 *   brightctl2 set <id> <v>       设亮度；v 可以是 0-100 或 +N / -N
 *   brightctl2 up [step]          主目标屏 +step（默认 5）
 *   brightctl2 down [step]        主目标屏 -step（默认 5）
 *   brightctl2 sync               其余可调屏亮度对齐主目标屏
 *   brightctl2 all <v>            所有可调屏设为 v
 *   brightctl2 blank [on|off]     内建屏黑屏 / 恢复（依赖本进程存活）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <CoreGraphics/CoreGraphics.h>

#include "brightctl_core.h"

static void usage(FILE *fp) {
    fprintf(fp,
        "用法:\n"
        "  brightctl2                列出显示器与当前亮度\n"
        "  brightctl2 get <id>       读亮度(0-100)\n"
        "  brightctl2 set <id> <v>   设亮度; v 支持 0-100 或 +N / -N\n"
        "  brightctl2 up [step]      主目标屏 +step (默认 5)\n"
        "  brightctl2 down [step]    主目标屏 -step (默认 5)\n"
        "  brightctl2 sync           其余可调屏对齐主目标屏\n"
        "  brightctl2 all <v>        所有可调屏设为 v\n"
        "  brightctl2 blank [on|off] 内建屏黑屏/恢复（进程需存活）\n");
}

static const char *channel_name(int ch) {
    switch (ch) {
        case BCTL_CHANNEL_DS:  return "DisplayServices";
        case BCTL_CHANNEL_DDC: return "DDC";
        default:               return "不可控";
    }
}

static bctl_display g_list[BCTL_MAX_DISPLAYS];
static int g_n;

static void load(void) {
    bctl_init();
    g_n = bctl_probe(g_list, BCTL_MAX_DISPLAYS);
    if (g_n <= 0) { fprintf(stderr, "枚举显示器失败\n"); exit(2); }
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
        printf("display id=0x%x%s%s  brightness=", d->display_id,
               d->is_main ? " (主屏)" : "", d->is_builtin ? " (内建)" : " (外接)");
        if (d->percent < 0) printf("读取失败");
        else                printf("%.1f%%", (double)d->percent);
        printf("  channel=%s\n", channel_name(d->channel));
    }
}

static int cmd_get(const char *idarg) {
    uint32_t id;
    if (!parse_id(idarg, &id)) { fprintf(stderr, "显示器 id 无效: %s\n", idarg); return 2; }
    if (!by_id(id)) { fprintf(stderr, "找不到显示器 0x%x\n", id); return 1; }
    float p;
    if (!bctl_get_percent(id, &p)) { fprintf(stderr, "读取亮度失败\n"); return 1; }
    printf("%.1f\n", (double)p);
    return 0;
}

static int cmd_set(const char *idarg, const char *valarg) {
    uint32_t id;
    if (!parse_id(idarg, &id)) { fprintf(stderr, "显示器 id 无效: %s\n", idarg); return 2; }
    bctl_display *d = by_id(id);
    if (!d) { fprintf(stderr, "找不到显示器 0x%x\n", id); return 1; }
    if (d->channel == BCTL_CHANNEL_NONE) { fprintf(stderr, "显示器 0x%x 亮度不可控\n", id); return 1; }

    char *end = NULL;
    double v = strtod(valarg, &end);
    if (end == valarg || *end != '\0') { fprintf(stderr, "亮度值无效: %s\n", valarg); return 2; }

    bool relative = (valarg[0] == '+' || valarg[0] == '-');
    float before;
    if (!bctl_get_percent(id, &before)) { fprintf(stderr, "无法读取当前亮度\n"); return 1; }

    if (relative) {
        float after = bctl_step_percent(id, (float)v);
        if (after < 0) { fprintf(stderr, "写入亮度失败\n"); return 1; }
        printf("id=0x%x %.1f%% -> %.1f%%\n", id, (double)before, (double)after);
        return 0;
    }

    if (v < 0 || v > 100) { fprintf(stderr, "亮度值需在 0-100: %s\n", valarg); return 2; }
    int target = (int)lround(v);
    if (target == (int)lroundf(before)) {
        printf("id=0x%x %.1f%% (未变化)\n", id, (double)before);
        return 0;
    }
    if (!bctl_set_percent(id, (float)target)) { fprintf(stderr, "写入亮度失败\n"); return 1; }
    float after;
    bctl_get_percent(id, &after);
    printf("id=0x%x %.1f%% -> %.1f%%\n", id, (double)before, (double)after);
    return 0;
}

static int cmd_step(bool up, int step) {
    uint32_t id = bctl_primary_display_id();
    if (!id) { fprintf(stderr, "没有可控显示器\n"); return 1; }
    if (step < 0) step = -step;
    float before, after;
    if (!bctl_get_percent(id, &before)) { fprintf(stderr, "无法读取当前亮度\n"); return 1; }
    after = bctl_step_percent(id, up ? (float)step : (float)-step);
    if (after < 0) { fprintf(stderr, "写入亮度失败\n"); return 1; }
    printf("id=0x%x %.1f%% -> %.1f%%\n", id, (double)before, (double)after);
    return 0;
}

static int cmd_sync(void) {
    uint32_t ref = bctl_primary_display_id();
    if (!ref) { fprintf(stderr, "没有可控显示器\n"); return 1; }
    float target;
    if (!bctl_get_percent(ref, &target)) { fprintf(stderr, "无法读取参考屏亮度\n"); return 1; }
    int changed = 0;
    for (int i = 0; i < g_n; i++) {
        bctl_display *d = &g_list[i];
        if (d->display_id == ref || d->channel == BCTL_CHANNEL_NONE) continue;
        if (bctl_set_percent(d->display_id, target)) changed++;
    }
    printf("已把 %d 台屏亮度对齐到 id=0x%x 的 %.1f%%\n", changed, ref, (double)target);
    return 0;
}

static int cmd_all(const char *valarg) {
    char *end = NULL;
    double v = strtod(valarg, &end);
    if (end == valarg || *end != '\0' || v < 0 || v > 100) {
        fprintf(stderr, "亮度值无效: %s\n", valarg);
        return 2;
    }
    int changed = 0;
    for (int i = 0; i < g_n; i++) {
        if (g_list[i].channel == BCTL_CHANNEL_NONE) continue;
        if (bctl_set_percent(g_list[i].display_id, (float)v)) changed++;
    }
    if (!changed) { fprintf(stderr, "没有可控显示器\n"); return 1; }
    printf("已把 %d 台屏亮度设为 %.0f%%\n", changed, v);
    return 0;
}

static int cmd_blank(int argc, char **argv) {
    bool on = true;
    if (argc >= 3 && !strcmp(argv[2], "off")) on = false;
    else if (argc >= 3 && !strcmp(argv[2], "on")) on = true;
    else if (argc >= 3) { fprintf(stderr, "blank 参数只能是 on|off\n"); return 2; }

    if (!bctl_builtin_blank(on)) { fprintf(stderr, "内建屏%s失败\n", on ? "黑屏" : "恢复"); return 1; }
    if (!on) { printf("内建屏已恢复\n"); return 0; }
    printf("内建屏已黑屏，按 Ctrl-C 恢复\n");
    fflush(stdout);
    for (;;) sleep(1);   /* 持有 capture，直到被中断 */
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") ||
                      !strcmp(argv[1], "help"))) {
        usage(stdout);
        return 0;
    }

    load();

    if (argc < 2 || !strcmp(argv[1], "list")) { cmd_list(); return 0; }

    const char *cmd = argv[1];
    if (!strcmp(cmd, "get")) {
        if (argc < 3) { fprintf(stderr, "get 需要显示器 id\n"); usage(stderr); return 2; }
        return cmd_get(argv[2]);
    }
    if (!strcmp(cmd, "set")) {
        if (argc < 4) { fprintf(stderr, "set 需要 <id> <v>\n"); usage(stderr); return 2; }
        return cmd_set(argv[2], argv[3]);
    }
    if (!strcmp(cmd, "up") || !strcmp(cmd, "down")) {
        int step = 5;
        if (argc >= 3) {
            char *end = NULL;
            long s = strtol(argv[2], &end, 10);
            if (end == argv[2] || *end != '\0') { fprintf(stderr, "步长无效: %s\n", argv[2]); return 2; }
            step = (int)s;
        }
        return cmd_step(cmd[0] == 'u', step);
    }
    if (!strcmp(cmd, "sync")) return cmd_sync();
    if (!strcmp(cmd, "all")) {
        if (argc < 3) { fprintf(stderr, "all 需要一个亮度值\n"); return 2; }
        return cmd_all(argv[2]);
    }
    if (!strcmp(cmd, "blank")) return cmd_blank(argc, argv);

    fprintf(stderr, "未知命令 %s\n", cmd);
    usage(stderr);
    return 2;
}
