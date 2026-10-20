# brightctl2 — macOS 亮度控制（CLI + 菜单栏应用）

**中文** | [English](README.en.md)

macOS 亮度控制工具，提供命令行与菜单栏应用两种形态。

在 Apple Silicon 机器上（实测环境：MacBook Pro M3 Pro + 外接 LG UltraFine 作主屏），
传统第三方亮度 API（IODisplay、CGDisplayIOServicePort、CoreDisplay）基本失效。
本工具改用两条**实测可用**的系统通道：

| 通道 | 目标屏 | 底层 | 实现 |
|---|---|---|---|
| CH1 `DisplayServices` | 内建 / Apple 硬件屏 | 私有框架 `DisplayServices.framework` | `dlopen` + `DisplayServicesGet/SetBrightness` |
| CH2 `DDC/CI over IOAVService` | 外接屏（HDMI/DP/USB-C/雷雳） | IOKit `IOAVService` + I2C | `DCPAVServiceProxy` → `IOAVServiceWrite/ReadI2C` 收发 VCP 0x10 |

两条通道都**不需要** root、不需要特殊 entitlement，普通 `clang` 编译的程序即可调用。

---

## 目录结构

| 文件 | 说明 |
|---|---|
| `brightctl_core.h` / `brightctl_core.c` | 亮度控制核心，CLI 与 GUI 共用 |
| `brightctl2.c` | 命令行前端 |
| `brightctl2` | 已编译产物（arm64） |
| `BrightCtlApp.swift` | 菜单栏应用（AppKit） |
| `BrightCtl.app` | 菜单栏应用产物（双击启动） |
| `test.sh` | 自动化验收脚本（真实读写亮度，结束自动还原） |
| `Makefile` | `make` / `make test` / `make clean` |
| `probe.c` / `probe` | 第一步探测：DisplayServices 能否 dlopen、符号是否导出 |
| `probe2.c` / `probe2` | 第二步探测：`CGGetActiveDisplayList` 枚举 + DisplayServices 读写实测 |
| `probe_ddc.c` / `probe_ddc` | 第三步探测：`DCPAVServiceProxy` → `IOAVService` → DDC/CI 读写实测 |

> `probe*` 是开发过程的验证中间产物，保留供追溯；日常使用只需 `brightctl2`。

---

## 原理

本文的接口细节均来自**对系统框架与协议的实测分析**：读取系统框架的导出接口、
按 DDC/CI 规范收发报文、并在真机上验证读写闭环。不依赖、也未使用任何第三方应用的代码或资产。

### 通道一：DisplayServices（内建屏）

Apple 硬件屏的亮度由系统私有框架 `DisplayServices.framework` 管理，核心接口是：

```c
_DisplayServicesSetBrightness(displayID, value);   // value 为 0.0~1.0
```

即 `/System/Library/PrivateFrameworks/DisplayServices.framework` 的
`DisplayServicesGetBrightness / DisplayServicesSetBrightness`，参数是 0.0–1.0 的浮点。
brightctl2 用公共 API `CGGetActiveDisplayList` 枚举显示器 ID，再按 ID 读写。

### 通道二：DDC/CI over IOAVService（外接屏）

外接屏亮度走显示器自身的 DDC/CI 协议。Apple Silicon 上没有 `IOAVService` 节点，
但 IORegistry 里有 `DCPAVServiceProxy`（`Location = External`），
用 `IOAVServiceCreateWithService()` 可以从它创建 `IOAVService`，再收发 I2C：

```c
IOAVServiceRef av = IOAVServiceCreateWithService(kCFAllocatorDefault, proxy);
```

**读亮度（VCP 0x10）**

```c
uint8_t req[4] = {0x82, 0x01, 0x10, 0x00};              // Get VCP 0x10
IOAVServiceWriteI2C(av, 0x37, 0x51, req, 4);            // chip=0x37, offset=0x51
usleep(40000);
uint8_t rep[12];
IOAVServiceReadI2C(av, 0x37, 0x00, rep, 12);            // offset=0x00
// 回包 6e 88 02 <result> 10 <type> <max_hi> <max_lo> <cur_hi> <cur_lo> <chk>
```

**写亮度（VCP 0x10）**

```c
uint8_t req[6] = {0x84, 0x03, 0x10, hi, lo, 0};
req[5] = 0x6e ^ 0x51 ^ req[0] ^ req[1] ^ req[2] ^ req[3] ^ req[4];   // XOR 校验
IOAVServiceWriteI2C(av, 0x37, 0x51, req, 6);
```

> **踩坑记录**：最初按「`0x03 0x10 hi lo` 就是 Set VCP」的直觉写，`IOAVServiceWriteI2C`
> 返回 0（成功），但读回亮度不变——外接屏直接忽略了这个报文。正确报文必须带
> 功能码 `0x84` 与数据长度 `0x03`：`84 03 10 hi lo chk`。改对后读回立即生效（89 → 81 → 89）。

---

## 构建

依赖：`clang`（Xcode 命令行工具）+ 系统自带 `CoreGraphics` / `IOKit` / 私有 `DisplayServices`。

```bash
cd brightctl2
make            # 构建 brightctl2 与三个 probe
make app        # 构建菜单栏应用 BrightCtl.app
make test       # 构建 + 跑验收脚本
make install    # 安装到 /Applications 并启动
```

手动编译等价命令：

```bash
clang -O2 -Wall -Wextra -o brightctl2 brightctl2.c -ldl \
      -framework CoreGraphics -framework IOKit -framework CoreFoundation
```

---

## 菜单栏应用（BrightCtl.app）

除了命令行，项目还提供一个菜单栏应用：启动后常驻状态栏，点图标弹出面板。

```bash
make app          # 构建
open BrightCtl.app   # 启动
make install      # 装进 /Applications 并启动
```

面板内容：

- **每块显示器一行**：名称（内建/外接/主屏）、当前百分比、亮度滑块，拖动即时生效
- **关闭内建屏**：勾选后内建屏从系统里断开（等同拔掉），取消勾选或退出应用即恢复。

> 早期版本用 `CGDisplayCapture` 独占内屏，副作用很重：被捕获的屏幕无法正常参与
> WindowServer 合成，**系统截图快捷键会静默失效**，显示器也无法正常休眠/锁屏。
> 现在改用显示配置事务（`CGBeginDisplayConfiguration` + `SLSConfigureDisplayEnabled`）
> 真正把内屏断开，截图、锁屏、休眠全部保持系统原生行为。
> 断开时会把内屏 id 落盘，进程崩溃或退出后再启动也能接回来。
- **开机自动启动**：勾选注册为登录项（SMAppService），取消即注销
- **退出 BrightCtl**

### 内屏安全保护

内屏黑屏后如果外接屏被拔掉，你会没有任何可操作的画面。所以做了三层保护：

1. **没有外屏时直接拒绝黑屏**：`bctl_builtin_blank(true)` 会先数一遍活动外接屏，为 0 就返回失败，面板上的勾选框也会置灰并给出提示。
2. **外屏断开时自动恢复**：应用注册了 `CGDisplayRegisterReconfigurationCallback`，任何显示器增删/启停都会触发检查；一旦外接屏数量归零且内屏仍是黑屏状态，立即释放捕获。
3. **进程退出自动释放**：`CGDisplayCapture` 与进程绑定，应用崩溃或退出后系统会自己恢复内屏，不会留下黑屏。

实测（`--selftest`）：`blank on=true isBlank=true` → `blank off=true isBlank=false`；
模拟无外屏时 `guard blank=false`；模拟「已黑屏 + 外屏消失」时 `autoRecover preBlank=true postBlank=false`。

实现要点：

- 用 `NSStatusItem` 挂在系统状态栏，`LSUIElement=true` 所以不占 Dock、不抢前台。
- 滑块拖动做 90ms 节流（DDC 每次事务 40–80ms），松手时强制写入最终值。
- 面板每 1.5 秒回读一次，外部改了亮度（比如按键盘亮度键）也能同步显示。
- 内屏黑屏用公共 API `CGDisplayCapture`；它和进程绑定，进程退出系统会自动释放，不会留下黑屏。

> 界面验证说明：本机未授予终端「屏幕录制」与「辅助功能」权限，因此无法由我截图核对外观；
> 已通过应用内日志确认 `bctl_probe` 在 GUI 进程里正确返回 2 块屏（0x2→DDC、0x1→DisplayServices）
> 以及菜单栏项与图标创建成功。实际观感请以你打开后看到的为准。

---

## 用法（命令行）

```
brightctl2                列出所有显示器、当前亮度与所用通道
brightctl2 get <id>       读指定显示器亮度（0-100）
brightctl2 set <id> <v>   设指定显示器亮度（0-100）；v 支持 +N / -N 相对增减
brightctl2 up [step]      主目标屏亮度 +step（默认 5）
brightctl2 down [step]    主目标屏亮度 -step（默认 5）
brightctl2 sync           其余可调屏亮度对齐主目标屏
brightctl2 all <v>        所有可调屏亮度设为 v
brightctl2 --lang zh|en   手动指定输出语言（默认跟随系统）
brightctl2 --help         用法
```

界面与输出支持**中英双语**，默认跟随系统语言（`zh*` → 中文，其他 → English）：

- 命令行：`--lang zh|en`，或环境变量 `BRIGHTCTL_LANG`
- 菜单栏应用：面板底部「语言」下拉，可选「跟随系统 / 中文 / English」，选择会被记住

```text
$ ./brightctl2
display id=0x2 (主屏) (外接)  brightness=89.0%  channel=DDC
display id=0x1 (内建)  brightness=56.0%  channel=DisplayServices
```

**主目标屏**的选取：主屏可控就用主屏，否则用第一台可控屏。
因此在本机（主屏是外接 LG UltraFine）上，`up` / `down` / `sync` 现在作用于**你正在看的那块外接屏**，
而不是旧版的「第一台可读屏（内建屏）」。

退出码：`0` 成功，`1` 运行期失败（读不到/写不进/找不到 id），`2` 用法错误。

---

## 验收

```bash
make test
```

`test.sh` 会真实读写亮度，覆盖 23 项断言，结束前把每台屏还原到起始值：

1. 枚举（屏数、可控屏数）
2. `get` 返回值落在 0–100
3. `set` 绝对值的读写闭环、`+N` / `-N` 相对增减
4. `set 100` / `set 0` 的边界 clamp
5. `up` / `down` 生效与还原
6. `sync` 后各屏与参考屏一致（容差 ±2）
7. `all` 批量设置
8. 错误路径退出码（缺参、非数字、超范围、不存在 id、未知命令、`--help`）

实测输出（2026-10-19，本机）：

```text
== 1. 枚举 ==
display id=0x2 (主屏) (外接)  brightness=89.0%  channel=DDC
display id=0x1 (内建)  brightness=56.0%  channel=DisplayServices
  ✓ 枚举到 2 台屏
  ✓ 2 台可控
...
通过 23 项，失败 0 项
```

---

## 实测记录（2026-10-19，macOS 26.6.2 / Apple M3 Pro）

1. **DisplayServices 通道**：`dlopen` 成功，`GetBrightness` / `SetBrightness` 符号导出；
   内建屏（id=0x1）读写正常，写值无需 entitlement。
2. **DDC 通道**：IORegistry 中存在 2 个 `DCPAVServiceProxy`（External / Embedded）；
   对 External 建 `IOAVService` 后，`IOAVServiceCopyEDID` 返回 256 字节 EDID（vendor `0x1e6d` = LG），
   VCP 0x10 可读（cur/max = 89/100），`84 03 10 ...` 可写并读回生效（89 → 81 → 89）。
3. **旧版遗留问题已解决**：旧版记录「主屏 id=0x2 读亮度返回错误码」——那是外接屏，
   `DisplayServices` 对它返回 `1000` 且 `CanChangeBrightness=0`；现在该屏走 DDC 通道，读写均正常。
4. **与其他亮度软件共存**：同一块外接屏可能被多个程序同时接管，
   若另一个程序也在做亮度同步或自动调节，两边会互相覆盖，见「已知限制」。

---

## 已知限制

- **多外接屏的 ID 配对**：`CGDirectDisplayID` 与 `DCPAVServiceProxy` 之间是按 IORegistry 顺序
  一一对应的启发式匹配，单外接屏场景已实测；接两块以上外接屏时若顺序不符，可能把亮度写到另一块屏上
  （尚未做 EDID 精确配对）。
- **两通道刻度不同**：`DisplayServices` 是 0–1 的 UI 亮度，DDC 是显示器自己的 0–max 刻度。
  `sync` 只能按百分比对齐，两块屏的**观感**未必完全一致。
- **内建屏有自动亮度**：环境光变化会让读数漂移（实测 65% → 72% 自动变化），`set` 后也可能被系统微调。
- **DDC 事务较慢**：每次读写有 40–80 ms 等待，连续 `set` 会明显变慢；个别屏不支持 DDC/CI 时会
  退化为「读取失败」，工具会回退尝试 DisplayServices。
- **依赖私有 API**：`DisplayServices` 与 `IOAVService` 均非公开接口，macOS 系统更新后可能失效；
  当前版本实测可用。
- **显示器休眠/热插拔**：休眠或拔线后 `IOAVService` 句柄可能失效，重新运行命令即可。

---

## 安全与使用说明

- 本工具会**直接修改显示器亮度**，属可见副作用操作；`test.sh` 只做小幅调整并在结束时还原。
- 依赖私有框架，行为随系统版本可能变化；仅供自用/学习。
- 本项目通过调用系统自带的私有框架与 IOKit 接口实现，**未使用、未复制、未分发任何第三方应用的代码或资产**；
  文中的接口名、报文格式与常量均为系统框架与 DDC/CI 规范本身的内容。

---

## 关于协作

- **Issue 面板已关闭**：本项目不接收问题反馈或功能请求。使用中遇到问题，请自行 fork 或下载代码修改。
- **可以提 Pull Request**：欢迎提交改进，维护者会按需审阅；但不承诺合并，涉及私有接口的改动需要真机验证后才会考虑。
- 项目主要在一台 Apple Silicon 机器 + LG UltraFine 外接屏上验证；其他显示器型号的适配情况未知，这属于预期内的差异。

## 许可与免责

- 代码以 [MIT](LICENSE) 许可发布。
- 本工具调用 macOS 未公开的私有接口（`DisplayServices`、`IOAVService`）与 DDC/CI 协议。
  这些接口不受 Apple 官方支持，**可能随系统更新失效**，也可能导致显示器行为异常；
  使用风险自负。此类用法不符合 Mac App Store 的上架要求，本项目仅面向开发者自编译使用。
- DDC/CI 会向显示器写入 VCP 0x10。个别显示器对非标准时序敏感，请自行评估风险。
- 本项目与任何第三方商业软件无隶属或授权关系。

## 后续路线

1. **多外接屏 EDID 精确配对**：用 `IOAVServiceCopyEDID` 与 `CGDisplay` 的 EDID 对齐，替换顺序匹配。
2. **开机自启动 / 定时恢复亮度**：用 launchd 在登录后按配置设一次亮度。
3. **GUI 形态**：菜单栏 / 窗口界面（工程量大，需另行评估）。
