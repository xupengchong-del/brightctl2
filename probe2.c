// probe2.c —— 完整探测：CGGetActiveDisplayList 枚举 + DisplayServices 读写亮度
#include <stdio.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdbool.h>
#include <CoreGraphics/CoreGraphics.h>

typedef int  (*DSGetBrightness_fn)(CGDirectDisplayID, float*);
typedef int  (*DSSetBrightness_fn)(CGDirectDisplayID, float);
typedef int  (*DSCanChange_fn)(CGDirectDisplayID, bool*);

int main(void) {
    printf("=== 枚举活动显示器 (CGGetActiveDisplayList) ===\n");
    CGDirectDisplayID ids[8];
    uint32_t count = 0;
    CGError err = CGGetActiveDisplayList(8, ids, &count);
    printf("CGGetActiveDisplayList err=%d count=%u\n", err, count);
    if (err != 0 || count == 0) { printf("无活动显示器\n"); return 1; }

    void *h = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", RTLD_NOW);
    if (!h) { printf("dlopen 失败: %s\n", dlerror()); return 1; }
    DSGetBrightness_fn getb = (DSGetBrightness_fn)dlsym(h, "DisplayServicesGetBrightness");
    DSSetBrightness_fn setb = (DSSetBrightness_fn)dlsym(h, "DisplayServicesSetBrightness");
    DSCanChange_fn     chg  = (DSCanChange_fn)dlsym(h, "DisplayServicesCanChangeBrightness");

    for (uint32_t i = 0; i < count; i++) {
        CGDirectDisplayID id = ids[i];
        bool isMain = (CGMainDisplayID() == id);
        printf("--- display[%u] id=0x%x%s ---\n", i, id, isMain ? " (主屏)" : "");
        float b = -1; int g = getb ? getb(id, &b) : -99;
        printf("  GetBrightness r=%d val=%.3f (0~1)\n", g, b);
        bool can = false; int c = chg ? chg(id, &can) : -99;
        printf("  CanChangeBrightness r=%d can=%d\n", c, can);
        // 强制直接写（绕过 can 检查），验证 SetBrightness 本身是否生效
        if (setb && b >= 0) {
            float target = b - 0.05f; if (target < 0.02f) target = 0.02f;
            int s = setb(id, target);
            printf("  [强制] SetBrightness(id, %.3f) r=%d\n", target, s);
            float b2 = -1; if (getb) getb(id, &b2);
            printf("  写后 GetBrightness=%.3f  %s\n", b2,
                   (b2 > 0 && (b2 < b - 0.005f || b2 > b + 0.005f)) ? "【亮度已变化】" : "（未变化）");
        } else if (setb) {
            printf("  [强制] SetBrightness 未测（getb 失败或无值）\n");
        }
    }
    printf("=== 结束 ===\n");
    return 0;
}
