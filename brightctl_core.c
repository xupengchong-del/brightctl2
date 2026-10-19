/*
 * brightctl_core.c —— 亮度控制核心（CLI 与菜单栏 app 共用）
 *
 *   CH1 DisplayServices（私有框架）：内建 / Apple 硬件屏
 *   CH2 IOAVService DDC/CI（IOKit）：外接屏，VCP 0x10
 */
#include "brightctl_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <dlfcn.h>
#include <IOKit/IOKitLib.h>
#include <CoreGraphics/CoreGraphics.h>

#define DDC_RETRIES   5
#define DDC_SETTLE_US 40000

/* 跨进程互斥：CLI 与菜单栏应用共用同一条 I2C 总线，
 * 并发访问会让显示器漏掉事务，因此用文件锁把 DDC 操作串行化。 */
static int g_lock_fd = -1;

static void ddc_lock(void) {
    if (g_lock_fd < 0) {
        char path[64];
        snprintf(path, sizeof path, "/tmp/.brightctl-i2c-%d.lock", (int)getuid());
        g_lock_fd = open(path, O_CREAT | O_RDWR, 0600);
    }
    if (g_lock_fd >= 0) flock(g_lock_fd, LOCK_EX);
}

static void ddc_unlock(void) {
    if (g_lock_fd >= 0) flock(g_lock_fd, LOCK_UN);
}

typedef struct __IOAVService *IOAVServiceRef;

extern IOAVServiceRef IOAVServiceCreateWithService(CFAllocatorRef allocator, io_service_t service);
extern IOReturn IOAVServiceWriteI2C(IOAVServiceRef service, uint32_t chipAddress,
                                    uint32_t dataAddress, void *buffer, uint32_t length);
extern IOReturn IOAVServiceReadI2C(IOAVServiceRef service, uint32_t chipAddress,
                                   uint32_t offset, void *buffer, uint32_t length);

typedef int (*DSGetBrightness_fn)(CGDirectDisplayID, float *);
typedef int (*DSSetBrightness_fn)(CGDirectDisplayID, float);

typedef struct {
    CGDirectDisplayID id;
    bool  is_main;
    bool  is_builtin;
    int   channel;
    float ds_value;
    int   ddc_cur;
    int   ddc_max;
    IOAVServiceRef av;
} Entry;

static DSGetBrightness_fn g_ds_get;
static DSSetBrightness_fn g_ds_set;
static bool g_inited;
static Entry g_entries[BCTL_MAX_DISPLAYS];
static int   g_count;

/* ---------- 通道一：DisplayServices ---------- */

static void ds_init(void) {
    void *h = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices",
                     RTLD_NOW);
    if (!h) return;
    g_ds_get = (DSGetBrightness_fn)dlsym(h, "DisplayServicesGetBrightness");
    g_ds_set = (DSSetBrightness_fn)dlsym(h, "DisplayServicesSetBrightness");
    if (!g_ds_get || !g_ds_set) { g_ds_get = NULL; g_ds_set = NULL; }
}

static bool ds_read(CGDirectDisplayID id, float *out) {
    if (!g_ds_get) return false;
    float v = -1.0f;
    if (g_ds_get(id, &v) != 0 || v < 0.0f) return false;
    *out = v;
    return true;
}

static bool ds_write(CGDirectDisplayID id, float v) {
    if (!g_ds_set) return false;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return g_ds_set(id, v) == 0;
}

/* ---------- 通道二：DDC/CI ---------- */

static int collect_external_services(io_service_t *out, int max) {
    int n = 0;
    CFMutableDictionaryRef match = IOServiceMatching("DCPAVServiceProxy");
    if (!match) return 0;
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) return 0;

    io_service_t svc;
    while ((svc = IOIteratorNext(it))) {
        char loc[64] = "";
        CFTypeRef v = IORegistryEntryCreateCFProperty(svc, CFSTR("Location"),
                                                      kCFAllocatorDefault, 0);
        if (v) {
            if (CFGetTypeID(v) == CFStringGetTypeID())
                CFStringGetCString((CFStringRef)v, loc, sizeof loc, kCFStringEncodingUTF8);
            CFRelease(v);
        }
        if (strcmp(loc, "External") == 0 && n < max) out[n++] = svc;
        else IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return n;
}

static bool ddc_read(IOAVServiceRef av, int *cur, int *maxv) {
    if (!av) return false;
    ddc_lock();
    for (int attempt = 0; attempt < DDC_RETRIES; attempt++) {
        uint8_t req[4] = {0x82, 0x01, 0x10, 0x00};
        if (IOAVServiceWriteI2C(av, 0x37, 0x51, req, sizeof req) != 0) {
            usleep(DDC_SETTLE_US);
            continue;
        }
        usleep(DDC_SETTLE_US);
        uint8_t rep[12];
        memset(rep, 0, sizeof rep);
        if (IOAVServiceReadI2C(av, 0x37, 0x00, rep, sizeof rep) != 0) {
            usleep(DDC_SETTLE_US);
            continue;
        }
        if (rep[0] != 0x6e || rep[1] != 0x88 || rep[4] != 0x10) {
            usleep(DDC_SETTLE_US);
            continue;
        }
        if (rep[3] != 0x00) {
            usleep(DDC_SETTLE_US);
            continue;
        }
        int mx = (rep[6] << 8) | rep[7];
        int cu = (rep[8] << 8) | rep[9];
        if (mx <= 0) continue;
        *maxv = mx;
        *cur = cu;
        ddc_unlock();
        return true;
    }
    ddc_unlock();
    return false;
}

static bool ddc_write(IOAVServiceRef av, int raw) {
    if (!av) return false;
    if (raw < 0) raw = 0;
    if (raw > 0xffff) raw = 0xffff;
    uint8_t req[6] = {0x84, 0x03, 0x10, (uint8_t)((raw >> 8) & 0xff), (uint8_t)(raw & 0xff), 0x00};
    uint8_t sum = 0;
    for (int i = 0; i < 5; i++) sum ^= req[i];
    req[5] = (uint8_t)(0x6e ^ 0x51 ^ sum);
    ddc_lock();
    bool ok = false;
    for (int attempt = 0; attempt < DDC_RETRIES; attempt++) {
        if (IOAVServiceWriteI2C(av, 0x37, 0x51, req, sizeof req) == 0) { ok = true; break; }
        usleep(DDC_SETTLE_US);
    }
    usleep(DDC_SETTLE_US * 2);
    ddc_unlock();
    return ok;
}

/* ---------- 探测与访问 ---------- */

void bctl_init(void) {
    if (g_inited) return;
    g_inited = true;
    ds_init();
}

int bctl_probe(bctl_display *out, int max) {
    bctl_init();

    /* 先释放上一轮的 DDC 句柄 */
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].av) { CFRelease(g_entries[i].av); g_entries[i].av = NULL; }
    g_count = 0;

    CGDirectDisplayID ids[BCTL_MAX_DISPLAYS];
    uint32_t n = 0;
    if (CGGetActiveDisplayList(BCTL_MAX_DISPLAYS, ids, &n) != kCGErrorSuccess || n == 0) return -1;
    if (n > BCTL_MAX_DISPLAYS) n = BCTL_MAX_DISPLAYS;

    for (uint32_t i = 0; i < n; i++) {
        Entry *e = &g_entries[i];
        memset(e, 0, sizeof *e);
        e->id = ids[i];
        e->is_main = (ids[i] == CGMainDisplayID());
        e->is_builtin = CGDisplayIsBuiltin(ids[i]) != 0;
        e->channel = BCTL_CHANNEL_NONE;
    }
    g_count = (int)n;

    /* 内建屏走 DisplayServices */
    for (int i = 0; i < g_count; i++) {
        Entry *e = &g_entries[i];
        if (!e->is_builtin) continue;
        float v;
        if (ds_read(e->id, &v)) { e->channel = BCTL_CHANNEL_DS; e->ds_value = v; }
    }

    /* 外接屏走 DDC */
    io_service_t proxies[BCTL_MAX_DISPLAYS];
    int np = collect_external_services(proxies, BCTL_MAX_DISPLAYS);
    int pi = 0;
    for (int i = 0; i < g_count && pi < np; i++) {
        Entry *e = &g_entries[i];
        if (e->is_builtin) continue;
        IOAVServiceRef av = IOAVServiceCreateWithService(kCFAllocatorDefault, proxies[pi]);
        IOObjectRelease(proxies[pi]);
        pi++;
        if (!av) continue;
        int cur = 0, mx = 0;
        if (ddc_read(av, &cur, &mx)) {
            e->channel = BCTL_CHANNEL_DDC;
            e->av = av;
            e->ddc_cur = cur;
            e->ddc_max = mx;
        } else {
            CFRelease(av);
        }
    }
    while (pi < np) { IOObjectRelease(proxies[pi]); pi++; }

    /* 兜底：DDC 不通的屏再试 DisplayServices */
    for (int i = 0; i < g_count; i++) {
        Entry *e = &g_entries[i];
        if (e->channel != BCTL_CHANNEL_NONE) continue;
        float v;
        if (ds_read(e->id, &v)) { e->channel = BCTL_CHANNEL_DS; e->ds_value = v; }
    }

    if (out) bctl_refresh(out, max);
    return g_count;
}

static int percent_of(Entry *e) {
    if (e->channel == BCTL_CHANNEL_DS)  return (int)lroundf(e->ds_value * 100.0f);
    if (e->channel == BCTL_CHANNEL_DDC) return (int)lroundf((float)e->ddc_cur * 100.0f / (float)e->ddc_max);
    return -1;
}

static bool refresh_entry(Entry *e) {
    if (e->channel == BCTL_CHANNEL_DS)  return ds_read(e->id, &e->ds_value);
    if (e->channel == BCTL_CHANNEL_DDC) return ddc_read(e->av, &e->ddc_cur, &e->ddc_max);
    return false;
}

static void fill(bctl_display *d, Entry *e) {
    d->display_id = e->id;
    d->is_main = e->is_main;
    d->is_builtin = e->is_builtin;
    d->channel = e->channel;
    d->percent = (float)percent_of(e);
}

int bctl_refresh(bctl_display *out, int max) {
    for (int i = 0; i < g_count; i++) refresh_entry(&g_entries[i]);
    if (!out) return g_count;
    int cnt = g_count < max ? g_count : max;
    for (int i = 0; i < cnt; i++) fill(&out[i], &g_entries[i]);
    return g_count;
}

static Entry *find_entry(uint32_t id) {
    for (int i = 0; i < g_count; i++) if (g_entries[i].id == id) return &g_entries[i];
    return NULL;
}

bool bctl_get_percent(uint32_t display_id, float *out) {
    Entry *e = find_entry(display_id);
    if (!e || !refresh_entry(e)) return false;
    int p = percent_of(e);
    if (p < 0) return false;
    if (out) *out = (float)p;
    return true;
}

static bool set_percent_entry(Entry *e, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (e->channel == BCTL_CHANNEL_DS) {
        if (!ds_write(e->id, (float)pct / 100.0f)) return false;
        usleep(30000);
        refresh_entry(e);
        return true;
    }
    if (e->channel == BCTL_CHANNEL_DDC) {
        int raw = (int)lroundf((float)pct * (float)e->ddc_max / 100.0f);
        if (!ddc_write(e->av, raw)) return false;
        refresh_entry(e);
        return true;
    }
    return false;
}

bool bctl_set_percent(uint32_t display_id, float percent) {
    Entry *e = find_entry(display_id);
    if (!e) return false;
    return set_percent_entry(e, (int)lroundf(percent));
}

float bctl_step_percent(uint32_t display_id, float delta) {
    Entry *e = find_entry(display_id);
    if (!e) return -1.0f;
    if (!refresh_entry(e)) return -1.0f;
    int cur = percent_of(e);
    if (cur < 0) return -1.0f;
    int target = cur + (int)lroundf(delta);
    if (target < 0) target = 0;
    if (target > 100) target = 100;
    if (target == cur) return (float)cur;
    if (!set_percent_entry(e, target)) return -1.0f;
    return (float)percent_of(e);
}

uint32_t bctl_primary_display_id(void) {
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].is_main && g_entries[i].channel != BCTL_CHANNEL_NONE)
            return g_entries[i].id;
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].channel != BCTL_CHANNEL_NONE)
            return g_entries[i].id;
    return 0;
}

bool bctl_is_builtin(uint32_t display_id) {
    Entry *e = find_entry(display_id);
    return e && e->is_builtin;
}

/* ---------- 内屏黑屏（CGDisplayCapture） ---------- */

static CGDirectDisplayID g_blank_id;
static bool g_blanked;

int bctl_external_count(void) {
    /* 仅供自动化测试模拟「没有外接显示器」的场景 */
    if (getenv("BRIGHTCTL_TEST_NO_EXTERNAL")) return 0;
    /* 用 online 而不是 active：显示器休眠时会从 active 列表消失，
     * 但它仍然连着、画面也还在，不能当成「外屏没了」。 */
    CGDirectDisplayID ids[BCTL_MAX_DISPLAYS];
    uint32_t n = 0;
    if (CGGetOnlineDisplayList(BCTL_MAX_DISPLAYS, ids, &n) != kCGErrorSuccess) return 0;
    int ext = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!CGDisplayIsBuiltin(ids[i])) ext++;
    return ext;
}

bool bctl_displays_asleep(void) {
    CGDirectDisplayID ids[BCTL_MAX_DISPLAYS];
    uint32_t n = 0;
    if (CGGetOnlineDisplayList(BCTL_MAX_DISPLAYS, ids, &n) != kCGErrorSuccess) return false;
    for (uint32_t i = 0; i < n; i++)
        if (CGDisplayIsAsleep(ids[i])) return true;
    return false;
}

bool bctl_builtin_blank(bool blank) {
    if (blank) {
        if (bctl_builtin_is_blank()) return true;
        /* 安全阀：没有外接显示器时绝不能黑掉内屏，否则用户没有任何画面 */
        if (bctl_external_count() == 0) return false;
        CGDirectDisplayID target = 0;
        for (int i = 0; i < g_count; i++)
            if (g_entries[i].is_builtin) { target = g_entries[i].id; break; }
        if (!target) {
            /* 缓存未初始化时直接按 CG 枚举找内建屏 */
            CGDirectDisplayID ids[BCTL_MAX_DISPLAYS];
            uint32_t n = 0;
            if (CGGetActiveDisplayList(BCTL_MAX_DISPLAYS, ids, &n) == kCGErrorSuccess) {
                for (uint32_t i = 0; i < n; i++)
                    if (CGDisplayIsBuiltin(ids[i])) { target = ids[i]; break; }
            }
        }
        if (!target) return false;
        if (CGDisplayCapture(target) != kCGErrorSuccess) return false;
        g_blank_id = target;
        g_blanked = true;
        return true;
    }
    if (!g_blanked) return true;
    CGDisplayRelease(g_blank_id);
    g_blanked = false;
    g_blank_id = 0;
    return true;
}

bool bctl_builtin_is_blank(void) {
    if (!g_blanked) return false;
    /* 显示器休眠/唤醒时系统可能自行释放捕获，这里以真实状态为准，
     * 否则会一直以为还黑着而不去补。 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (g_blank_id && !CGDisplayIsCaptured(g_blank_id)) {
        g_blanked = false;
        g_blank_id = 0;
    }
#pragma clang diagnostic pop
    return g_blanked;
}

/* ---------- 本地化 ---------- */

typedef struct { const char *zh; const char *en; } bctl_msg;

static const bctl_msg g_msgs[BCTL_T_COUNT] = {
    [BCTL_T_MAIN]       = {"主屏", "main"},
    [BCTL_T_BUILTIN]    = {"内建", "built-in"},
    [BCTL_T_EXTERNAL]   = {"外接", "external"},
    [BCTL_T_UNREADABLE] = {"读取失败", "unreadable"},
    [BCTL_T_CH_NONE]    = {"不可控", "unavailable"},
    [BCTL_T_CH_DS]      = {"DisplayServices", "DisplayServices"},
    [BCTL_T_CH_DDC]     = {"DDC", "DDC"},

    [BCTL_T_USAGE] = {
        "用法:\n"
        "  brightctl2                列出显示器与当前亮度\n"
        "  brightctl2 get <id>       读亮度(0-100)\n"
        "  brightctl2 set <id> <v>   设亮度; v 支持 0-100 或 +N / -N\n"
        "  brightctl2 up [step]      主目标屏 +step (默认 5)\n"
        "  brightctl2 down [step]    主目标屏 -step (默认 5)\n"
        "  brightctl2 sync           其余可调屏对齐主目标屏\n"
        "  brightctl2 all <v>        所有可调屏设为 v\n"
        "  brightctl2 blank [on|off] 内建屏黑屏/恢复（进程需存活）\n"
        "  brightctl2 --lang zh|en   手动指定输出语言\n",
        "Usage:\n"
        "  brightctl2                list displays and current brightness\n"
        "  brightctl2 get <id>       read brightness (0-100)\n"
        "  brightctl2 set <id> <v>   set brightness; v may be 0-100 or +N / -N\n"
        "  brightctl2 up [step]      primary target +step (default 5)\n"
        "  brightctl2 down [step]    primary target -step (default 5)\n"
        "  brightctl2 sync           align other controllable displays to primary\n"
        "  brightctl2 all <v>        set every controllable display to v\n"
        "  brightctl2 blank [on|off] blank/restore built-in display (process must live)\n"
        "  brightctl2 --lang zh|en   force the output language\n"
    },
    [BCTL_T_NEED_ID]          = {"需要显示器 id", "display id required"},
    [BCTL_T_BAD_ID]           = {"显示器 id 无效", "invalid display id"},
    [BCTL_T_NO_DISPLAY]       = {"找不到显示器", "display not found"},
    [BCTL_T_BAD_VALUE]        = {"亮度值无效", "invalid brightness value"},
    [BCTL_T_RANGE]            = {"亮度值需在 0-100", "brightness must be within 0-100"},
    [BCTL_T_READ_FAIL]        = {"读取亮度失败", "failed to read brightness"},
    [BCTL_T_WRITE_FAIL]       = {"写入亮度失败", "failed to write brightness"},
    [BCTL_T_NO_CONTROLLABLE]  = {"没有可控显示器", "no controllable display"},
    [BCTL_T_UNKNOWN_CMD]      = {"未知命令", "unknown command"},
    [BCTL_T_UNCHANGED]        = {"未变化", "unchanged"},
    [BCTL_T_SYNC_DONE]        = {"已把 %d 台屏亮度对齐到 id=0x%x 的 %.1f%%",
                                 "aligned %d display(s) to id=0x%x at %.1f%%"},
    [BCTL_T_ALL_DONE]         = {"已把 %d 台屏亮度设为 %.0f%%",
                                 "set %d display(s) to %.0f%%"},
    [BCTL_T_BLANK_ON]         = {"内建屏已黑屏，按 Ctrl-C 恢复",
                                 "built-in display blanked; press Ctrl-C to restore"},
    [BCTL_T_BLANK_OFF]        = {"内建屏已恢复", "built-in display restored"},
    [BCTL_T_BLANK_FAIL_ON]    = {"内建屏黑屏失败", "failed to blank the built-in display"},
    [BCTL_T_BLANK_FAIL_OFF]   = {"内建屏恢复失败", "failed to restore the built-in display"},
    [BCTL_T_BLANK_ARG]        = {"blank 参数只能是 on|off", "blank accepts only on|off"},
    [BCTL_T_NEED_VALUE]       = {"需要一个亮度值", "a brightness value is required"},
    [BCTL_T_NEED_STEP]        = {"步长无效", "invalid step"},

    [BCTL_T_PANEL_TITLE]      = {"显示器亮度", "Display brightness"},
    [BCTL_T_HINT]             = {"拖动即生效", "Changes apply while dragging"},
    [BCTL_T_CHECK_BLANK]      = {"关闭内建屏（黑屏）", "Blank built-in display"},
    [BCTL_T_CHECK_AUTO]       = {"接上外屏时自动关闭内屏",
                                 "Blank built-in when an external display is connected"},
    [BCTL_T_CHECK_LOGIN]      = {"开机自动启动", "Launch at login"},
    [BCTL_T_QUIT]             = {"退出 BrightCtl", "Quit BrightCtl"},
    [BCTL_T_LANG_LABEL]       = {"语言", "Language"},
    [BCTL_T_LANG_AUTO]        = {"跟随系统", "Automatic"},
    [BCTL_T_ALERT_LOGIN_TITLE] = {"无法设置开机自启", "Cannot change login item"},
    [BCTL_T_ALERT_LOGIN_MSG]   = {"请先把 BrightCtl.app 安装到 /Applications（make install），再重试。当前状态：%s",
                                  "Install BrightCtl.app into /Applications first (make install), then retry. Current status: %s"},
    [BCTL_T_ALERT_BLANK_ON_TITLE] = {"无法关闭内建屏", "Cannot blank the built-in display"},
    [BCTL_T_ALERT_BLANK_ON_MSG]   = {"未检测到外接显示器。为免失去所有画面，已拒绝关闭内屏。",
                                     "No external display detected. Refusing to blank the built-in display so you do not lose all video output."},
    [BCTL_T_ALERT_BLANK_OFF_TITLE] = {"无法恢复内建屏", "Cannot restore the built-in display"},
    [BCTL_T_ALERT_BLANK_OFF_MSG]   = {"释放捕获失败，请重新打开面板重试。",
                                      "Failed to release the capture. Reopen the panel and try again."},
};

static char g_lang_setting[8] = "auto";
static char g_lang_effective[8] = "en";

static const char *system_language(void) {
    CFArrayRef langs = CFLocaleCopyPreferredLanguages();
    const char *out = "en";
    if (langs && CFArrayGetCount(langs) > 0) {
        CFStringRef first = (CFStringRef)CFArrayGetValueAtIndex(langs, 0);
        char buf[32] = "";
        if (CFStringGetCString(first, buf, sizeof buf, kCFStringEncodingUTF8)) {
            if (strncmp(buf, "zh", 2) == 0) out = "zh";
        }
    }
    if (langs) CFRelease(langs);
    return out;
}

static void resolve_language(void) {
    /* 优先级：显式设置（--lang / 面板选择）> 环境变量 > 系统语言 */
    if (strcmp(g_lang_setting, "zh") == 0) { strcpy(g_lang_effective, "zh"); return; }
    if (strcmp(g_lang_setting, "en") == 0) { strcpy(g_lang_effective, "en"); return; }

    const char *env = getenv("BRIGHTCTL_LANG");
    if (env && *env) {
        if (strncmp(env, "zh", 2) == 0) { strcpy(g_lang_effective, "zh"); return; }
        if (strncmp(env, "en", 2) == 0) { strcpy(g_lang_effective, "en"); return; }
    }
    strcpy(g_lang_effective, system_language());
}

void bctl_set_language(const char *lang) {
    if (!lang || !*lang) lang = "auto";
    snprintf(g_lang_setting, sizeof g_lang_setting, "%s", lang);
    resolve_language();
}

const char *bctl_language(void) { return g_lang_effective; }
const char *bctl_language_setting(void) { return g_lang_setting; }

const char *bctl_t(int key) {
    if (key < 0 || key >= BCTL_T_COUNT) return "";
    const bctl_msg *m = &g_msgs[key];
    return (strcmp(g_lang_effective, "zh") == 0) ? m->zh : m->en;
}

