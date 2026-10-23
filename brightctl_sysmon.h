#ifndef BRIGHTCTL_SYSMON_H
#define BRIGHTCTL_SYSMON_H

#include <stdint.h>
#include <stdbool.h>

/* ---------- 系统状态采集 ---------- */

typedef struct {
    /* CPU */
    double cpu_percent;      /* 0-100，两次采样之间的占用率 */
    int    cpu_count;        /* 逻辑核心数 */
    int    core_count;       /* 物理核心数 */

    /* 内存 */
    double mem_total_mb;
    double mem_used_mb;      /* active + wired + compressed */
    double mem_percent;

    /* 温度（摄氏） */
    double cpu_temp;         /* CPU 裸片温度，取 PMU tdie 最大值 */
    double gpu_temp;         /* 取 PMU tdev 最大值；无则 -1 */
    bool   temp_available;

    /* 负载 */
    double load1, load5, load15;

    /* 进程 */
    int    process_count;
} bctl_sysinfo;

/* 初始化（可重复调用） */
void bctl_sysmon_init(void);

/* 采样一次；首次调用只建立基线，cpu_percent 为 -1 */
void bctl_sysmon_sample(bctl_sysinfo *out);

/* 单项快速读取 */
double bctl_sysmon_cpu_percent(void);
double bctl_sysmon_mem_percent(void);
bool   bctl_sysmon_temp(double *cpu, double *gpu);

#endif
