# brightctl2 — macOS brightness control (CLI + menu bar app)

[中文](README.md) | **English**

Control both the built-in and external display brightness on macOS: a menu bar app for daily use,
a CLI for scripting. The built-in panel turns off automatically when an external display is
connected, and comes back when you unplug it.

## Install

Download `BrightCtl-1.0.0-macos-arm64.zip` from the
[latest release](https://github.com/xupengchong-del/brightctl2/releases/latest), unzip it, and drag
`BrightCtl.app` into /Applications.

**Right-click → Open** the first time (the app is not notarized by Apple). If macOS says the app is
damaged:

```bash
xattr -dr com.apple.quarantine /Applications/BrightCtl.app
```

Requires macOS 13+ on Apple Silicon. A ☀️ icon appears in the menu bar.

Or build from source:

```bash
make          # build the CLI
make app      # build the menu bar app
make install  # install into /Applications
make test     # regression tests that really change brightness
```

## Why not the usual approach

macOS has no public API to truly turn off a single display. The common trick is `CGDisplayCapture`,
but a captured display drops out of normal WindowServer compositing — while it is captured, **the
system screenshot shortcut silently stops working**, and sleep/lock misbehave.

This tool instead **mirrors** the built-in panel to the external display and sets its backlight to 0
via DisplayServices. The built-in stays in the system display list, so screenshots, lock and sleep
keep working. Original brightness, auto-brightness and mirror state are saved before the change;
failures roll back, and unplugging or quitting restores everything.

## Two brightness channels

---

## Layout

| File | Purpose |
|---|---|
| `brightctl_core.h` / `brightctl_core.c` | Brightness core, shared by CLI and GUI |
| `brightctl2.c` | Command-line front end |
| `BrightCtlApp.swift` | Menu bar app (AppKit) |
| `test.sh` | Acceptance test (really changes brightness, restores on exit) |
| `Makefile` | `make` / `make app` / `make test` / `make install` |

---

## How it works

Everything below comes from **measuring the system frameworks and protocols directly**:
reading the exported interfaces of the system frameworks, sending DDC/CI packets per the
specification, and verifying the read/write round trip on real hardware. No third-party
application's code or assets are used, copied, or distributed.

### Channel 1: DisplayServices (built-in display)

Brightness of Apple displays is managed by the private framework `DisplayServices.framework`:

```c
_DisplayServicesSetBrightness(displayID, value);   // value in 0.0 ~ 1.0
```

i.e. `DisplayServicesGetBrightness` / `DisplayServicesSetBrightness`, taking a float in 0.0–1.0.
brightctl2 enumerates displays with the public `CGGetActiveDisplayList` API and reads/writes by ID.

### Channel 2: DDC/CI over IOAVService (external display)

External displays expose brightness through their own DDC/CI implementation. On Apple Silicon
there are no `IOAVService` nodes in the IORegistry, but there are `DCPAVServiceProxy` entries
with `Location = External`. An `IOAVService` can be created from one of them:

```c
IOAVServiceRef av = IOAVServiceCreateWithService(kCFAllocatorDefault, proxy);
```

**Read brightness (VCP 0x10)**

```c
uint8_t req[4] = {0x82, 0x01, 0x10, 0x00};              // Get VCP 0x10
IOAVServiceWriteI2C(av, 0x37, 0x51, req, 4);            // chip=0x37, offset=0x51
usleep(40000);
uint8_t rep[12];
IOAVServiceReadI2C(av, 0x37, 0x00, rep, 12);            // offset=0x00
// reply: 6e 88 02 <result> 10 <type> <max_hi> <max_lo> <cur_hi> <cur_lo> <chk>
```

**Write brightness (VCP 0x10)**

```c
uint8_t req[6] = {0x84, 0x03, 0x10, hi, lo, 0};
req[5] = 0x6e ^ 0x51 ^ req[0] ^ req[1] ^ req[2] ^ req[3] ^ req[4];   // XOR checksum
IOAVServiceWriteI2C(av, 0x37, 0x51, req, 6);
```

> **Gotcha**: sending `0x03 0x10 hi lo` (the intuitive "Set VCP") makes
> `IOAVServiceWriteI2C` return 0 (success), but the display silently ignores it and the value
> never changes. The packet must carry function code `0x84` and length `0x03`:
> `84 03 10 hi lo chk`. With that, the change is visible immediately (89 → 81 → 89).

---

## Build

Requires `clang` (Xcode command line tools) plus the system `CoreGraphics` / `IOKit`
and the private `DisplayServices` framework.

```bash
cd brightctl2
make            # build the CLI
make app        # build the menu bar app BrightCtl.app
make test       # build + run the acceptance test
make install    # install to /Applications and launch
```

Manual equivalent:

```bash
clang -O2 -Wall -Wextra -o brightctl2 brightctl2.c brightctl_core.c -ldl \
      -framework CoreGraphics -framework IOKit -framework CoreFoundation
```

---

## Menu bar app (BrightCtl.app)

Besides the CLI, the project ships a menu bar app: it lives in the status bar and opens a panel
when you click the sun icon.

```bash
make app
open BrightCtl.app
make install      # install to /Applications and launch
```

The panel contains:

- **One row per display**: name (built-in / external / main), current percentage, and a brightness
  slider that applies while dragging.
- **Blank built-in display**: mirror the built-in panel to the external display and set its backlight to 0; uncheck or unplug the external display to restore its saved settings.
- **Blank built-in automatically when an external display is connected**: on by default, can be
  turned off; the choice is remembered.
- **Launch at login**: registers an `SMAppService` login item.
- **Quit BrightCtl.**

Implementation notes:

- `NSStatusItem`, with `LSUIElement=true` so it never shows in the Dock.
- Slider input is throttled to 90 ms (a DDC transaction takes 40–80 ms); the final value on mouse-up
  is always written.
- The panel re-reads values every 1.5 s, so changes made with the keyboard brightness keys show up.
- The built-in panel uses public mirroring configuration and DisplayServices backlight control; the app does not call `CGDisplayCapture` or private display-disable APIs.

### Built-in display recovery

The built-in display stays in the system display list and is never captured or disabled. BrightCtl saves its original brightness, auto-brightness, and mirror state; a failed mirror or brightness operation is rolled back. Unplugging the external display restores the saved state. While the screen is locked or asleep, display configuration changes are deferred until unlock/wake.

Brightness 0 turns off the backlight, not the panel's hardware power. If auto-brightness was enabled, the ambient sensor may choose a different brightness after restoration, so the final percentage can differ from the value before blanking.

---

## Usage (CLI)

```
brightctl2                list all displays, current brightness, and channel used
brightctl2 get <id>       read brightness (0-100)
brightctl2 set <id> <v>   set brightness; v may be 0-100 or +N / -N
brightctl2 up [step]      brightness of the primary target display +step (default 5)
brightctl2 down [step]    brightness of the primary target display -step (default 5)
brightctl2 sync           align other controllable displays to the primary target
brightctl2 all <v>        set every controllable display to v
brightctl2 blank [on|off] mirror and darken/restore built-in (restores on exit)
brightctl2 sys            show CPU / memory / temperature / load / processes
brightctl2 --lang zh|en   force the output language (default: follow the system)
brightctl2 --help         usage
```

All UI and CLI output is **bilingual (Chinese / English)** and follows the system language by
default (`zh*` → Chinese, otherwise English):

- CLI: `--lang zh|en`, or the `BRIGHTCTL_LANG` environment variable
- Menu bar app: a "Language" popup at the bottom of the panel (Automatic / 中文 / English);
  the choice is remembered

```text
$ ./brightctl2
display id=0x2 (main) (external)  brightness=89.0%  channel=DDC
display id=0x1 (built-in)  brightness=56.0%  channel=DisplayServices
```

**Primary target** selection: the main display if it is controllable, otherwise the first
controllable display. On the tested machine the main display is the external LG UltraFine, so
`up` / `down` / `sync` act on the display you are actually looking at.

Exit codes: `0` success, `1` runtime failure (cannot read/write/find the display), `2` usage error.

---

## System status

`brightctl2 sys` and the menu bar panel both show live system stats:

```text
$ brightctl2 sys
System
  CPU     46.6%   (11/11)
  Memory  80.9%   (14913/18432 MB)
  Temp    57.8°C   (GPU 50.0°C)
  Load   8.86  7.48  6.54
  Procs  758
```

Everything comes from public system interfaces — **no root, no extra dependencies**:

| Metric | Source |
|---|---|
| CPU usage | `host_processor_info(PROCESSOR_CPU_LOAD_INFO)`, delta between two samples |
| Memory | `host_statistics64(HOST_VM_INFO64)`: app + wired + compressed |
| CPU/GPU temperature | `IOHIDEventSystemClient`, reading `PMU tdie*` / `PMU tdev*` sensors |
| Load | `sysctl vm.loadavg` |
| Process count | `sysctl kern.proc.all` |

Temperature comes from Apple Silicon PMU sensors. On machines without them the row shows
"unavailable" rather than inventing a number.

## Tests

```bash
make test
```

`test.sh` really changes brightness and covers 32 CLI assertions, restoring every display to its
starting value when it exits. A separate hardware-free suite covers 18 blank/restore failure scenarios:

1. enumeration (display count, controllable count)
2. `get` returns a value within 0–100
3. `set` absolute round trip, `+N` / `-N` relative steps
4. `set 100` / `set 0` clamping
5. `up` / `down` effect and restore
6. `sync` consistency (±2 tolerance)
7. `all` batch set
8. exit codes for bad input (missing args, non-numeric, out of range, unknown id, unknown command, `--help`)

A real-machine check verified that the built-in stays online and uncaptured, native Cmd-Shift-3 creates a screenshot, and SIGTERM restores brightness, mirroring, and position. When auto-brightness is enabled, the ambient sensor may choose a different final brightness.

---

## Measured on real hardware (2026-10-19, macOS 26.6.2 / Apple M3 Pro)

1. **DisplayServices**: `dlopen` succeeds, `GetBrightness` / `SetBrightness` are exported; the
   built-in display (id=0x1) reads and writes normally, no entitlement required.
2. **DDC**: two `DCPAVServiceProxy` entries exist (External / Embedded); creating an `IOAVService`
   from the External one yields a 256-byte EDID, VCP 0x10 is readable, and `84 03 10 ...` writes
   take effect and read back (89 → 81 → 89).
3. `PowerMode` is not available on the built-in display (`HasPowerMode=0`); the panel is darkened with DisplayServices backlight control.

---

## Known limitations

- **Multi-external-display matching**: `CGDirectDisplayID` ↔ `DCPAVServiceProxy` is matched by
  IORegistry order. Verified with a single external display; with two or more, an incorrect order
  could send a brightness change to the wrong display (EDID-based matching is not implemented yet).
- **Different scales**: `DisplayServices` uses a 0–1 UI brightness, DDC uses the display's own
  0–max scale. `sync` can only align percentages; the perceived brightness may not match exactly.
- **Auto-brightness on the built-in display**: ambient light changes make readings drift
  (observed 65% → 72%), and a `set` may be adjusted by the system afterwards.
- **DDC is slow**: each transaction waits 40–80 ms; repeated `set` calls are noticeably slow.
- **Private APIs**: `DisplayServices` and `IOAVService` are not public interfaces and may break in a
  future macOS release.
- **Sleep / hot-plug**: an `IOAVService` handle may become invalid after sleep or unplugging; just
  run the command again.

---

## Contributing

- **Issues are disabled**: this project does not accept bug reports or feature requests. If you need
  a change, fork the repository or download the code and modify it yourself.
- **Pull requests are welcome**: improvements may be submitted and will be reviewed as time permits,
  but merging is not guaranteed. Changes touching private interfaces need verification on real
  hardware first.
- The project has only been verified on one Apple Silicon machine with an LG UltraFine external
  display. Behaviour on other display models is unknown and is expected to differ.

---

## License and disclaimer

- Released under the [MIT](LICENSE) license.
- The tool calls undocumented macOS interfaces (`DisplayServices`, `IOAVService`) and the DDC/CI
  protocol. These are not supported by Apple, **may break at any system update**, and may cause
  unexpected display behaviour. Use at your own risk. Such usage is not eligible for the Mac App
  Store; the project is intended for developers who build it themselves.
- DDC/CI writes VCP 0x10 to the display. Some panels are sensitive to non-standard timing.
- This project is not affiliated with, or endorsed by, any third-party commercial software.

## Roadmap

1. **EDID-based matching for multiple external displays**, replacing the current order heuristic.
2. **Launch at login / scheduled restore** via launchd.
3. **More UI work**: menu bar panel refinement (larger scope, needs its own evaluation).
