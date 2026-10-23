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
#include <sys/mount.h>
#include <sys/time.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_var.h>
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


/* ---------- GPU 占用（直接查 IOAccelerator，避免 ioreg 全量遍历） ---------- */

static double gpu_utilization(void) {
    io_iterator_t iter = 0;
    CFMutableDictionaryRef match = IOServiceMatching("IOAccelerator");
    if (!match) return -1;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS) return -1;

    double best = -1;
    io_service_t svc;
    while ((svc = IOIteratorNext(iter))) {
        CFTypeRef stats = IORegistryEntryCreateCFProperty(svc, CFSTR("PerformanceStatistics"),
                                                          kCFAllocatorDefault, 0);
        if (stats && CFGetTypeID(stats) == CFDictionaryGetTypeID()) {
            CFTypeRef v = CFDictionaryGetValue((CFDictionaryRef)stats, CFSTR("Device Utilization %"));
            if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
                int pct = 0;
                if (CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &pct) && pct > best)
                    best = (double)pct;
            }
            v = CFDictionaryGetValue((CFDictionaryRef)stats, CFSTR("Renderer Utilization %"));
            if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
                int pct = 0;
                if (CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &pct) && pct > best)
                    best = (double)pct;
            }
        }
        if (stats) CFRelease(stats);
        IOObjectRelease(svc);
    }
    IOObjectRelease(iter);
    return best;
}

/* ---------- 电池 ---------- */

static void read_battery(bool *has, double *percent, bool *charging, bool *on_ac) {
    *has = false; *percent = -1; *charging = false; *on_ac = false;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSmartBattery"));
    if (!svc) return;

    int cur = 0, max = 0;
    bool chg = false, ac = false;
    CFTypeRef v;
    if ((v = IORegistryEntryCreateCFProperty(svc, CFSTR("CurrentCapacity"), kCFAllocatorDefault, 0))) {
        if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &cur);
        CFRelease(v);
    }
    if ((v = IORegistryEntryCreateCFProperty(svc, CFSTR("MaxCapacity"), kCFAllocatorDefault, 0))) {
        if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &max);
        CFRelease(v);
    }
    if ((v = IORegistryEntryCreateCFProperty(svc, CFSTR("IsCharging"), kCFAllocatorDefault, 0))) {
        chg = (v == kCFBooleanTrue);
        CFRelease(v);
    }
    if ((v = IORegistryEntryCreateCFProperty(svc, CFSTR("ExternalConnected"), kCFAllocatorDefault, 0))) {
        ac = (v == kCFBooleanTrue);
        CFRelease(v);
    }
    IOObjectRelease(svc);

    *has = true;
    *charging = chg;
    *on_ac = ac;
    if (max > 0) *percent = 100.0 * (double)cur / (double)max;
}

/* ---------- 磁盘（根卷） ---------- */

static void read_disk(double *used_gb, double *total_gb, double *percent) {
    struct statfs sf;
    if (statfs("/System/Volumes/Data", &sf) != 0 && statfs("/", &sf) != 0) {
        *used_gb = *total_gb = *percent = -1;
        return;
    }
    double bsize = (double)sf.f_bsize;
    double total = (double)sf.f_blocks * bsize;
    double avail = (double)sf.f_bavail * bsize;
    *total_gb = total / 1073741824.0;
    *used_gb = (total - avail) / 1073741824.0;
    *percent = total > 0 ? (total - avail) / total * 100.0 : -1;
}

/* ---------- 网络（默认路由接口，累计字节求差） ---------- */

static uint64_t g_prev_rx, g_prev_tx;
static bool g_have_net_baseline;

static void read_network(double *rx_bps, double *tx_bps, double elapsed_sec, bool *ok) {
    *rx_bps = *tx_bps = -1;
    *ok = false;

    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return;

    uint64_t rx = 0, tx = 0;
    bool found = false;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK || !p->ifa_data) continue;
        /* 跳过回环与非常用接口 */
        if (strncmp(p->ifa_name, "lo", 2) == 0) continue;
        if (strncmp(p->ifa_name, "en", 2) != 0 && strncmp(p->ifa_name, "utun", 4) != 0) continue;
        struct if_data *d = (struct if_data *)p->ifa_data;
        rx += (uint64_t)d->ifi_ibytes;
        tx += (uint64_t)d->ifi_obytes;
        found = true;
    }
    freeifaddrs(ifa);
    if (!found) return;
    *ok = true;

    if (!g_have_net_baseline || elapsed_sec <= 0) {
        g_prev_rx = rx; g_prev_tx = tx;
        g_have_net_baseline = true;
        return;   /* 首次没有速率 */
    }
    /* 计数回绕或接口重连时可能变小，按 0 处理而不是显示负数 */
    double drx = rx >= g_prev_rx ? (double)(rx - g_prev_rx) : 0;
    double dtx = tx >= g_prev_tx ? (double)(tx - g_prev_tx) : 0;
    g_prev_rx = rx; g_prev_tx = tx;

    *rx_bps = drx / elapsed_sec;
    *tx_bps = dtx / elapsed_sec;
}

/* ---------- 开机时长 ---------- */

static double uptime_seconds(void) {
    struct timeval boot;
    size_t len = sizeof boot;
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boot, &len, NULL, 0) != 0) return -1;
    struct timeval now;
    gettimeofday(&now, NULL);
    return (double)(now.tv_sec - boot.tv_sec);
}

/* ---------- 汇总 ---------- */

/* 分级缓存：便宜指标每次采，昂贵指标按各自周期降频刷新。
 * 面板每 2 秒刷新一次，温度/GPU 每 4 秒、电池每 8 秒、磁盘每 10 秒，
 * 避免为「显示一行字」反复唤起床、跑 IORegistry 查询。 */
static struct timespec g_last_temp, g_last_gpu, g_last_batt, g_last_disk;
static bool g_have_cache;
static double g_c_cpu_temp = -1, g_c_gpu_temp = -1, g_c_gpu_pct = -1;
static bool   g_c_temp_ok;
static bool   g_c_has_batt; static double g_c_batt_pct = -1;
static bool   g_c_batt_chg, g_c_batt_ac;
static double g_c_disk_used = -1, g_c_disk_total = -1, g_c_disk_pct = -1;

static double elapsed_since(struct timespec *t) {
    if (__builtin_expect(!g_have_cache, 1)) return 1e9;   /* 首次立即刷新 */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - t->tv_sec) + (double)(now.tv_nsec - t->tv_nsec) / 1e9;
}

static void mark_now(struct timespec *t) { clock_gettime(CLOCK_MONOTONIC, t); }

void bctl_sysmon_sample(bctl_sysinfo *out) {
    if (!out) return;
    bctl_sysmon_init();
    memset(out, 0, sizeof *out);

    struct timespec t_begin;
    clock_gettime(CLOCK_MONOTONIC, &t_begin);

    out->cpu_percent = cpu_percent_now();

    int64_t ncpu = 0, ncore = 0;
    size_t len = sizeof ncpu;
    if (sysctlbyname("hw.logicalcpu", &ncpu, &len, NULL, 0) != 0) ncpu = 0;
    len = sizeof ncore;
    if (sysctlbyname("hw.physicalcpu", &ncore, &len, NULL, 0) != 0) ncore = 0;
    out->cpu_count = (int)ncpu;
    out->core_count = (int)ncore;

    read_memory(&out->mem_total_mb, &out->mem_used_mb, &out->mem_percent);

    /* 温度：每 4 秒 */
    if (elapsed_since(&g_last_temp) >= 4.0) {
        double c = -1, g = -1;
        g_c_temp_ok = bctl_sysmon_temp(&c, &g);
        g_c_cpu_temp = c; g_c_gpu_temp = g;
        mark_now(&g_last_temp);
    }
    out->temp_available = g_c_temp_ok;
    out->cpu_temp = g_c_cpu_temp;
    out->gpu_temp = g_c_gpu_temp;

    /* GPU：每 4 秒 */
    if (elapsed_since(&g_last_gpu) >= 4.0) {
        g_c_gpu_pct = gpu_utilization();
        mark_now(&g_last_gpu);
    }
    out->gpu_percent = g_c_gpu_pct;

    /* 电池：每 8 秒 */
    if (elapsed_since(&g_last_batt) >= 8.0) {
        read_battery(&g_c_has_batt, &g_c_batt_pct, &g_c_batt_chg, &g_c_batt_ac);
        mark_now(&g_last_batt);
    }
    out->has_battery = g_c_has_batt;
    out->battery_percent = g_c_batt_pct;
    out->battery_charging = g_c_batt_chg;
    out->battery_on_ac = g_c_batt_ac;

    /* 磁盘：每 10 秒 */
    if (elapsed_since(&g_last_disk) >= 10.0) {
        read_disk(&g_c_disk_used, &g_c_disk_total, &g_c_disk_pct);
        mark_now(&g_last_disk);
    }
    out->disk_used_gb = g_c_disk_used;
    out->disk_total_gb = g_c_disk_total;
    out->disk_percent = g_c_disk_pct;

    /* 网络：取决于与上次采样的真实间隔 */
    static struct timespec t_prev_sample;
    double interval = g_have_cache
        ? (double)(t_begin.tv_sec - t_prev_sample.tv_sec) +
          (double)(t_begin.tv_nsec - t_prev_sample.tv_nsec) / 1e9
        : 0;
    t_prev_sample = t_begin;
    read_network(&out->net_rx_bps, &out->net_tx_bps, interval, &out->net_available);

    out->uptime_seconds = uptime_seconds();
    read_load(&out->load1, &out->load5, &out->load15);
    out->process_count = process_count();

    g_have_cache = true;
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
