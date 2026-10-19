#ifndef BRIGHTCTL_CORE_H
#define BRIGHTCTL_CORE_H

#include <stdint.h>
#include <stdbool.h>

#define BCTL_MAX_DISPLAYS 16

/* ---------- 本地化 ---------- */

typedef enum {
    BCTL_T_MAIN = 0, BCTL_T_BUILTIN, BCTL_T_EXTERNAL, BCTL_T_UNREADABLE,
    BCTL_T_CH_NONE, BCTL_T_CH_DS, BCTL_T_CH_DDC,

    BCTL_T_USAGE,
    BCTL_T_NEED_ID, BCTL_T_BAD_ID, BCTL_T_NO_DISPLAY, BCTL_T_BAD_VALUE, BCTL_T_RANGE,
    BCTL_T_READ_FAIL, BCTL_T_WRITE_FAIL, BCTL_T_NO_CONTROLLABLE, BCTL_T_UNKNOWN_CMD,
    BCTL_T_UNCHANGED, BCTL_T_SYNC_DONE, BCTL_T_ALL_DONE,
    BCTL_T_BLANK_ON, BCTL_T_BLANK_OFF, BCTL_T_BLANK_FAIL_ON, BCTL_T_BLANK_FAIL_OFF, BCTL_T_BLANK_ARG,
    BCTL_T_NEED_VALUE, BCTL_T_NEED_STEP,

    BCTL_T_PANEL_TITLE, BCTL_T_HINT, BCTL_T_CHECK_BLANK, BCTL_T_CHECK_AUTO,
    BCTL_T_CHECK_LOGIN, BCTL_T_QUIT, BCTL_T_LANG_LABEL, BCTL_T_LANG_AUTO,
    BCTL_T_ALERT_LOGIN_TITLE, BCTL_T_ALERT_LOGIN_MSG,
    BCTL_T_ALERT_BLANK_ON_TITLE, BCTL_T_ALERT_BLANK_ON_MSG,
    BCTL_T_ALERT_BLANK_OFF_TITLE, BCTL_T_ALERT_BLANK_OFF_MSG,

    BCTL_T_COUNT
} bctl_str;

/* 语言：NULL 或 "auto" 跟随系统；"zh" / "en" 强制 */
void        bctl_set_language(const char *lang);
const char *bctl_language(void);      /* 当前生效的 "zh" 或 "en" */
const char *bctl_language_setting(void); /* 当前设置：auto / zh / en */
const char *bctl_t(int key);

enum {
    BCTL_CHANNEL_NONE = 0,
    BCTL_CHANNEL_DS   = 1,   /* DisplayServices */
    BCTL_CHANNEL_DDC  = 2,   /* DDC/CI over IOAVService */
};

typedef struct {
    uint32_t display_id;
    bool     is_main;
    bool     is_builtin;
    int      channel;
    float    percent;        /* 0-100；<0 表示不可读 */
} bctl_display;

/* 初始化私有框架与 IOAVService（可重复调用，内部只做一次） */
void bctl_init(void);

/* 枚举并探测所有活动显示器，返回数量；负数表示失败 */
int  bctl_probe(bctl_display *out, int max);

/* 重新读取亮度并刷新 out 数组（保持顺序），返回数量 */
int  bctl_refresh(bctl_display *out, int max);

/* 按显示器 id 读写百分比（0-100） */
bool bctl_get_percent(uint32_t display_id, float *out);
bool bctl_set_percent(uint32_t display_id, float percent);

/* 相对调整，返回调整后的百分比；失败返回 -1 */
float bctl_step_percent(uint32_t display_id, float delta);

/* 主目标屏 id（主屏可控则取主屏，否则第一台可控屏）；无则返回 0 */
uint32_t bctl_primary_display_id(void);

/* 内建屏黑屏开关：blank=true 捕获内屏（黑屏），false 释放。
 * 捕获与进程绑定，进程退出会自动恢复。
 * 没有外接显示器时拒绝黑屏，避免用户失去所有画面。 */
bool bctl_builtin_blank(bool blank);
bool bctl_builtin_is_blank(void);

/* 当前在线（含休眠）的外接显示器数量，不依赖 DDC 是否可控 */
int  bctl_external_count(void);

/* 是否有显示器正处于休眠状态（休眠期间不宜改动捕获状态） */
bool bctl_displays_asleep(void);

/* 是否为内建屏 */
bool bctl_is_builtin(uint32_t display_id);

#endif
