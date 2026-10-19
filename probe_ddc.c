/*
 * probe_ddc.c —— 外接屏 DDC/CI 通道探测（保留供追溯）
 *
 * 验证链路：DCPAVServiceProxy(Location=External) → IOAVService → I2C 0x37
 *   Get VCP 0x10 : 写 [0x82,0x01,0x10,0x00] 到 (0x37,0x51)，再从 (0x37,0x00) 读 12 字节
 *   Set VCP 0x10 : 写 [0x84,0x03,0x10,hi,lo,chk] 到 (0x37,0x51)，chk = 0x6e^0x51^...
 *
 * 用法:
 *   ./probe_ddc            只读：打印每个 External 代理 + EDID + 当前亮度
 *   ./probe_ddc set <pct>  写入指定百分比（会改变屏幕亮度）
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>

typedef struct __IOAVService *IOAVServiceRef;
extern IOAVServiceRef IOAVServiceCreateWithService(CFAllocatorRef, io_service_t);
extern IOReturn IOAVServiceWriteI2C(IOAVServiceRef, uint32_t, uint32_t, void*, uint32_t);
extern IOReturn IOAVServiceReadI2C(IOAVServiceRef, uint32_t, uint32_t, void*, uint32_t);
extern IOReturn IOAVServiceCopyEDID(IOAVServiceRef, CFDataRef*);

static int ddc_get(IOAVServiceRef av, int *cur, int *maxv) {
    uint8_t req[4] = {0x82, 0x01, 0x10, 0x00};
    if (IOAVServiceWriteI2C(av, 0x37, 0x51, req, 4) != 0) return -1;
    usleep(40000);
    uint8_t rep[12];
    memset(rep, 0, sizeof rep);
    if (IOAVServiceReadI2C(av, 0x37, 0x00, rep, 12) != 0) return -2;
    if (rep[0] != 0x6e || rep[1] != 0x88 || rep[4] != 0x10) return -3;
    if (rep[3] != 0) return -4;
    *maxv = (rep[6] << 8) | rep[7];
    *cur  = (rep[8] << 8) | rep[9];
    return 0;
}

static int ddc_set(IOAVServiceRef av, int raw) {
    uint8_t req[6] = {0x84, 0x03, 0x10, (uint8_t)(raw >> 8), (uint8_t)raw, 0x00};
    uint8_t s = 0;
    for (int i = 0; i < 5; i++) s ^= req[i];
    req[5] = (uint8_t)(0x6e ^ 0x51 ^ s);
    if (IOAVServiceWriteI2C(av, 0x37, 0x51, req, 6) != 0) return -1;
    usleep(80000);
    return 0;
}

int main(int argc, char **argv) {
    int want_set = -1;
    if (argc >= 3 && !strcmp(argv[1], "set")) want_set = atoi(argv[2]);

    CFMutableDictionaryRef match = IOServiceMatching("DCPAVServiceProxy");
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) {
        printf("枚举 DCPAVServiceProxy 失败\n");
        return 1;
    }
    io_service_t svc;
    int idx = 0, found = 0;
    while ((svc = IOIteratorNext(it))) {
        char loc[64] = "?";
        CFTypeRef v = IORegistryEntryCreateCFProperty(svc, CFSTR("Location"),
                                                      kCFAllocatorDefault, 0);
        if (v) {
            if (CFGetTypeID(v) == CFStringGetTypeID())
                CFStringGetCString((CFStringRef)v, loc, sizeof loc, kCFStringEncodingUTF8);
            CFRelease(v);
        }
        printf("proxy[%d] Location=%s\n", idx++, loc);
        if (strcmp(loc, "External") != 0) { IOObjectRelease(svc); continue; }

        IOAVServiceRef av = IOAVServiceCreateWithService(kCFAllocatorDefault, svc);
        IOObjectRelease(svc);
        if (!av) { printf("  IOAVService 创建失败\n"); continue; }
        found++;

        CFDataRef edid = NULL;
        if (IOAVServiceCopyEDID(av, &edid) == 0 && edid) {
            const uint8_t *p = CFDataGetBytePtr(edid);
            printf("  EDID len=%ld vendor=0x%02x%02x\n", (long)CFDataGetLength(edid), p[8], p[9]);
            CFRelease(edid);
        }
        int cur = -1, mx = -1;
        int r = ddc_get(av, &cur, &mx);
        if (r == 0) printf("  VCP 0x10 cur=%d max=%d (%.1f%%)\n", cur, mx, cur * 100.0 / mx);
        else        printf("  VCP 0x10 读取失败 r=%d（该屏可能不支持 DDC/CI）\n", r);

        if (want_set >= 0 && r == 0) {
            int raw = (int)(want_set * (double)mx / 100.0 + 0.5);
            printf("  set %d%% (raw=%d) -> ret=%d\n", want_set, raw, ddc_set(av, raw));
            int c2 = -1, m2 = -1;
            if (ddc_get(av, &c2, &m2) == 0) printf("  读回 %d/%d (%.1f%%)\n", c2, m2, c2 * 100.0 / m2);
        }
        CFRelease(av);
    }
    IOObjectRelease(it);
    printf("External 代理数: %d\n", found);
    return found > 0 ? 0 : 1;
}
