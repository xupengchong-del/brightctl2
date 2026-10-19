// probe.c —— 实测 BetterDisplay 两条私有亮度路线的可调用性（内建屏 DisplayServices + 外接 DDC IOAVService）
#include <stdio.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdbool.h>
#include <mach/mach.h>

typedef uint32_t CGDirectDisplayID;

// DisplayServices 私有框架函数签名（依据框架导出符号表确认）
typedef int  (*DSGetBrightness_fn)(CGDirectDisplayID, float*);
typedef int  (*DSSetBrightness_fn)(CGDirectDisplayID, float);
typedef int  (*DSCanChange_fn)(CGDirectDisplayID, bool*);
typedef int  (*DSEnumerate_fn)(int, CGDirectDisplayID*, int*, void*);

int main(void) {
    printf("=== 探测 DisplayServices 私有框架 ===\n");
    void *h = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", RTLD_NOW);
    if (!h) { printf("dlopen DisplayServices 失败: %s\n", dlerror()); return 1; }
    printf("dlopen 成功\n");

    DSGetBrightness_fn  getb  = (DSGetBrightness_fn)dlsym(h, "DisplayServicesGetBrightness");
    DSSetBrightness_fn  setb  = (DSSetBrightness_fn)dlsym(h, "DisplayServicesSetBrightness");
    DSCanChange_fn      chg   = (DSCanChange_fn)dlsym(h, "DisplayServicesCanChangeBrightness");
    DSEnumerate_fn      en    = (DSEnumerate_fn)dlsym(h, "DisplayServicesGetDisplayList");
    printf("DisplayServicesGetBrightness        = %p\n", (void*)getb);
    printf("DisplayServicesSetBrightness        = %p\n", (void*)setb);
    printf("DisplayServicesCanChangeBrightness  = %p\n", (void*)chg);
    printf("DisplayServicesGetDisplayList       = %p\n", (void*)en);

    if (en) {
        CGDirectDisplayID ids[8];
        int count = 0;
        int r = en(0, ids, &count, NULL);
        printf("GetDisplayList r=%d count=%d\n", r, count);
        for (int i=0;i<count && i<8;i++) printf("  display[%d]=0x%x\n", i, ids[i]);
        if (count>0) {
            if (getb) { float v=0; int g=getb(ids[0], &v); printf("GetBrightness(id=0x%x)=%d val=%.3f\n", ids[0], g, v); }
            if (chg) { bool c=false; int cc=chg(ids[0], &c); printf("CanChange(id)=%d can=%d\n", cc, c); }
        }
    } else {
        printf("(GetDisplayList 未导出)\n");
    }
    printf("=== 结束 ===\n");
    return 0;
}
