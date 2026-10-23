/*
 * brightctl_sysmon.c —— 系统状态采集（CPU / 内存 / 温度 / 负载）
 *
 * 数据源全部为系统公开接口，无需 root、无需特殊 entitlement：
 *   CPU 占用  host_processor_info(PROCESSOR_CPU_LOAD_INFO)
 *   内存      host_statistics64(HOST_VM_INFO64)
 *   温度      IOHIDEventSystemClient 枚举 AppleVendor 温度传感器
 *   负载      sysctl vm.loadavg
 *   进程数    sysctl kern.proc.all
 */

#include "brightctl_sysmon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

/* ---------- IOHID 动态符号 ---------- */

typedef void *(*HIDCreate_fn)(CFAllocatorRef);
typedef void (*HIDSetMatching_fn)(void *, CFDictionaryRef);
typedef CFTypeRef (*HIDCopyServices_fn)(void *);
typedef CFTypeRef (*HIDCopyProperty_fn)(void *, CFStringRef);
typedef CFTypeRef (*HIDCopyEvent_fn)(void *, uint32_t, int32_t, int32_t);
typedef double (*HIDGetFloat_fn)(CFTypeRef, uint32_t);

static HIDCreate_fn      g_hid_create;
static HIDSetMatching_fn g_hid_set_matching;
static HIDCopyServices_fn g_hid_copy_services;
static HIDCopyProperty_fn g_hid_copy_property;
static HIDCopyEvent_fn   g_hid_copy_event;
static HIDGetFloat_fn    g_hid_get_float;

static bool g_inited;
static bool g_hid_ready;

/* CPU 采样基线 */
static uint64_t g_prev_user, g_prev_sys, g_prev_idle, g_prev_nice;
static bool g_have_baseline;

#define HID_EVENT_TEMPERATURE 15u
#define HID_FIELD_TEMPERATURE_LEVEL (HID_EVENT_TEMPERATURE << 16)

void bctl_sysmon_init(void) {
    if (g_inited) return;
    g_inited = true;

    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (!iokit) return;
    g_hid_create        = (HIDCreate_fn)dlsym(iokit, "IOHIDEventSystemClientCreate");
    g_hid_set_matching  = (HIDSetMatching_fn)dlsym(iokit, "IOHIDEventSystemClientSetMatching");
    g_hid_copy_services = (HIDCopyServices_fn)dlsym(iokit, "IOHIDEventSystemClientCopyServices");
    g_hid_copy_property = (HIDCopyProperty_fn)dlsym(iokit, "IOHIDServiceClientCopyProperty");
    g_hid_copy_event    = (HIDCopyEvent_fn)dlsym(iokit, "IOHIDServiceClientCopyEvent");
    g_hid_get_float     = (HIDGetFloat_fn)dlsym(iokit, "IOHIDEventGetFloatValue");

    g_hid_ready = g_hid_create && g_hid_set_matching && g_hid_copy_services &&
                  g_hid_copy_property && g_hid_copy_event && g_hid_get_float;
}

/* ---------- CPU ---------- */

static bool read_cpu_ticks(uint64_t *user, uint64_t *sys, uint64_t *idle, uint64_t *nice) {
    natural_t count = 0;
    processor_info_array_t info = NULL;
    mach_msg_type_number_t num = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count, &info, &num) != KERN_SUCCESS)
        return false;

    uint64_t u = 0, s = 0, i = 0, n = 0;
    processor_cpu_load_info_t loads = (processor_cpu_load_info_t)info;
    for (natural_t k = 0; k < count; k++) {
        u += loads[k].cpu_ticks[CPU_STATE_USER];
        s += loads[k].cpu_ticks[CPU_STATE_SYSTEM];
        i += loads[k].cpu_ticks[CPU_STATE_IDLE];
        n += loads[k].cpu_ticks[CPU_STATE_NICE];
    }
    /* 释放 mach 分配的数组 */
    vm_deallocate(mach_task_self(), (vm_address_t)info, num * sizeof(integer_t));

    *user = u; *sys = s; *idle = i; *nice = n;
    return true;
}

static double cpu_percent_now(void) {
    uint64_t u, s, i, n;
    if (!read_cpu_ticks(&u, &s, &i, &n)) return -1;

    if (!g_have_baseline) {
        g_prev_user = u; g_prev_sys = s; g_prev_idle = i; g_prev_nice = n;
        g_have_baseline = true;
        return -1;   /* 首次采样没有可比较的基线 */
    }

    uint64_t du = u - g_prev_user;
    uint64_t ds = s - g_prev_sys;
    uint64_t di = i - g_prev_idle;
    uint64_t dn = n - g_prev_nice;
    g_prev_user = u; g_prev_sys = s; g_prev_idle = i; g_prev_nice = n;

    uint64_t total = du + ds + di + dn;
    if (total == 0) return 0;
    return 100.0 * (double)(du + ds + dn) / (double)total;
}

/* ---------- 内存 ---------- */

static void read_memory(double *total_mb, double *used_mb, double *percent) {
    uint64_t memsize = 0;
    size_t len = sizeof memsize;
    if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) != 0 || memsize == 0) {
        *total_mb = *used_mb = *percent = -1;
        return;
    }

    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS) {
        *total_mb = *used_mb = *percent = -1;
        return;
    }

    /* 页大小按实际查询，避免在非 16K 页机器上算错 */
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);

    /* 与「活动监视器」口径接近：app + wired + compressed */
    double used = ((double)vm.active_count + (double)vm.wire_count +
                   (double)vm.compressor_page_count) * (double)page;
    *total_mb = (double)memsize / 1048576.0;
    *used_mb = used / 1048576.0;
    *percent = *used_mb / *total_mb * 100.0;
}

/* ---------- 温度 ---------- */

bool bctl_sysmon_temp(double *cpu, double *gpu) {
    bctl_sysmon_init();
    if (cpu) *cpu = -1;
    if (gpu) *gpu = -1;
    if (!g_hid_ready) return false;

    void *client = g_hid_create(kCFAllocatorDefault);
    if (!client) return false;

    int page = 0xff00, usage = 5;
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberIntType, &page);
    CFNumberRef nu = CFNumberCreate(NULL, kCFNumberIntType, &usage);
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (np && nu && match) {
        CFDictionarySetValue(match, CFSTR("PrimaryUsagePage"), np);
        CFDictionarySetValue(match, CFSTR("PrimaryUsage"), nu);
        g_hid_set_matching(client, match);
    }

    CFArrayRef services = (CFArrayRef)g_hid_copy_services(client);
    double cpu_max = -1, gpu_max = -1;
    if (services && CFGetTypeID(services) == CFArrayGetTypeID()) {
        long n = (long)CFArrayGetCount(services);
        for (long i = 0; i < n; i++) {
            void *svc = (void *)CFArrayGetValueAtIndex(services, i);
            char name[128] = "";
            CFTypeRef prod = g_hid_copy_property(svc, CFSTR("Product"));
            if (prod && CFGetTypeID(prod) == CFStringGetTypeID())
                CFStringGetCString((CFStringRef)prod, name, sizeof name, kCFStringEncodingUTF8);
            if (prod) CFRelease(prod);

            /* 只关心 CPU/GPU 相关传感器，过滤电池、NAND、环境光等 */
            bool is_cpu = strncmp(name, "PMU tdie", 8) == 0 || strncmp(name, "SOC MCPU", 8) == 0;
            bool is_gpu = strncmp(name, "PMU tdev", 8) == 0 || strncmp(name, "PMU tdieg", 9) == 0;
            if (!is_cpu && !is_gpu) continue;

            CFTypeRef ev = g_hid_copy_event(svc, HID_EVENT_TEMPERATURE, 0, 0);
            if (!ev) continue;
            double v = g_hid_get_float(ev, HID_FIELD_TEMPERATURE_LEVEL);
            CFRelease(ev);
            if (!isfinite(v) || v <= 0 || v > 150) continue;

            if (is_cpu && v > cpu_max) cpu_max = v;
            if (is_gpu && v > gpu_max) gpu_max = v;
        }
    }
    if (services) CFRelease(services);
    if (match) CFRelease(match);
    if (np) CFRelease(np);
    if (nu) CFRelease(nu);

    if (cpu) *cpu = cpu_max;
    if (gpu) *gpu = gpu_max;
    return cpu_max > 0 || gpu_max > 0;
}

/* ---------- 负载 / 进程 ---------- */

static void read_load(double *l1, double *l5, double *l15) {
    struct loadavg la;
    size_t len = sizeof la;
    if (sysctl((int[]){CTL_VM, VM_LOADAVG}, 2, &la, &len, NULL, 0) != 0) {
        *l1 = *l5 = *l15 = -1;
        return;
    }
    double scale = (double)la.fscale;
    *l1  = (double)la.ldavg[0] / scale;
    *l5  = (double)la.ldavg[1] / scale;
    *l15 = (double)la.ldavg[2] / scale;
}

static int process_count(void) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0) return -1;
    /* 结构体大小随版本变化，用字节数除以单个记录长度即可，无需解析内容 */
    struct kinfo_proc kp;
    if (size < sizeof kp) return -1;
    return (int)(size / sizeof kp);
}

/* ---------- 汇总 ---------- */

void bctl_sysmon_sample(bctl_sysinfo *out) {
    if (!out) return;
    bctl_sysmon_init();
    memset(out, 0, sizeof *out);

    out->cpu_percent = cpu_percent_now();

    int64_t ncpu = 0, ncore = 0;
    size_t len = sizeof ncpu;
    if (sysctlbyname("hw.logicalcpu", &ncpu, &len, NULL, 0) != 0) ncpu = 0;
    len = sizeof ncore;
    if (sysctlbyname("hw.physicalcpu", &ncore, &len, NULL, 0) != 0) ncore = 0;
    out->cpu_count = (int)ncpu;
    out->core_count = (int)ncore;

    read_memory(&out->mem_total_mb, &out->mem_used_mb, &out->mem_percent);

    double cpu = -1, gpu = -1;
    out->temp_available = bctl_sysmon_temp(&cpu, &gpu);
    out->cpu_temp = cpu;
    out->gpu_temp = gpu;

    read_load(&out->load1, &out->load5, &out->load15);
    out->process_count = process_count();
}

double bctl_sysmon_cpu_percent(void) {
    bctl_sysmon_init();
    return cpu_percent_now();
}

double bctl_sysmon_mem_percent(void) {
    bctl_sysmon_init();
    double t, u, p;
    read_memory(&t, &u, &p);
    return p;
}
