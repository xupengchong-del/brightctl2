#ifndef BRIGHTCTL_CORE_H
#define BRIGHTCTL_CORE_H

#include <stdint.h>
#include <stdbool.h>

#define BCTL_MAX_DISPLAYS 16

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

/* 当前活动的外接显示器数量（不依赖 DDC 是否可控） */
int  bctl_external_count(void);

/* 是否为内建屏 */
bool bctl_is_builtin(uint32_t display_id);

#endif
