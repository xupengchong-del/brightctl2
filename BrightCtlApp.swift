import Cocoa
import Foundation
import ServiceManagement

// MARK: - 本地化

/// 从 C 核心取文案，与 CLI 共用同一套语言判断
func L(_ key: UInt32) -> String {
    guard let p = bctl_t(Int32(key)) else { return "" }
    return String(cString: p)
}

// MARK: - 偏好设置

enum Prefs {
    /// 语言设置："auto" / "zh" / "en"
    static var language: String {
        get { UserDefaults.standard.string(forKey: "language") ?? "auto" }
        set { UserDefaults.standard.set(newValue, forKey: "language") }
    }

    /// 接上外接屏时自动关闭内屏（默认开启）
    static var autoBlankBuiltin: Bool {
        get {
            if UserDefaults.standard.object(forKey: "autoBlankBuiltin") == nil { return true }
            return UserDefaults.standard.bool(forKey: "autoBlankBuiltin")
        }
        set { UserDefaults.standard.set(newValue, forKey: "autoBlankBuiltin") }
    }
}

// MARK: - 开机自启（登录项）

enum LoginItem {
    static var isEnabled: Bool {
        SMAppService.mainApp.status == .enabled
    }

    static var statusText: String {
        switch SMAppService.mainApp.status {
        case .enabled: return "已启用"
        case .notRegistered: return "未注册"
        case .notFound: return "未找到"
        case .requiresApproval: return "需在系统设置中批准"
        @unknown default: return "未知"
        }
    }

    @discardableResult
    static func set(_ on: Bool) -> Bool {
        do {
            if on {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
            return true
        } catch {
            NSLog("BrightCtl login item 设置失败: \(error.localizedDescription)")
            return false
        }
    }
}

// MARK: - 写入节流（DDC 每次 40~80ms，拖动时不能每个像素都发）

final class BrightWriter {
    private let queue = DispatchQueue.main // 与探测/黑屏配置串行，避免使用被释放的 DDC 句柄
    private var lastWrite: [UInt32: Date] = [:]
    private let lock = NSLock()

    func submit(_ id: UInt32, _ value: Float, force: Bool) {
        lock.lock()
        let now = Date()
        if !force, let t = lastWrite[id], now.timeIntervalSince(t) < 0.09 {
            lock.unlock()
            return
        }
        lastWrite[id] = now
        lock.unlock()

        queue.async {
            _ = bctl_set_percent(id, value)
        }
    }
}

let sharedWriter = BrightWriter()

// MARK: - 单块显示器的行

final class DisplayRow: NSView {
    let displayID: UInt32
    private let nameLabel = NSTextField(labelWithString: "")
    private let valueLabel = NSTextField(labelWithString: "")
    private let slider = NSSlider()

    init(display: bctl_display) {
        self.displayID = display.display_id
        super.init(frame: .zero)

        var title = display.is_builtin ? L(BCTL_T_BUILTIN.rawValue) : L(BCTL_T_EXTERNAL.rawValue)
        if display.is_main {
            title += bctl_language() == nil ? "" : (String(cString: bctl_language()) == "zh" ? "（主屏）" : " (main)")
        }
        title += String(format: "  0x%x", display.display_id)
        nameLabel.stringValue = title
        nameLabel.font = .systemFont(ofSize: 13, weight: .medium)
        nameLabel.textColor = .labelColor

        valueLabel.font = .monospacedDigitSystemFont(ofSize: 13, weight: .regular)
        valueLabel.alignment = .right
        valueLabel.textColor = .secondaryLabelColor

        slider.minValue = 0
        slider.maxValue = 100
        slider.isContinuous = true
        slider.target = self
        slider.action = #selector(sliderChanged(_:))

        let controllable = display.channel != 0
        slider.isEnabled = controllable
        if !controllable {
            valueLabel.stringValue = "不可控"
        }

        addSubview(nameLabel)
        addSubview(valueLabel)
        addSubview(slider)
        update(percent: display.percent)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    var isControllable: Bool { slider.isEnabled }

    func update(percent: Float) {
        guard percent >= 0 else {
            valueLabel.stringValue = "读取失败"
            return
        }
        if !slider.isEnabled { return }
        valueLabel.stringValue = String(format: "%.0f%%", percent)
        if !isDragging {
            slider.doubleValue = Double(percent)
        }
    }

    private var isDragging = false

    @objc private func sliderChanged(_ sender: NSSlider) {
        let v = Float(sender.doubleValue)
        valueLabel.stringValue = String(format: "%.0f%%", v)

        let mouseUp = NSApp.currentEvent?.type == .leftMouseUp
        isDragging = !mouseUp
        sharedWriter.submit(displayID, v, force: mouseUp)
    }

    override func layout() {
        super.layout()
        let w = bounds.width
        nameLabel.frame = NSRect(x: 0, y: bounds.height - 20, width: w - 60, height: 17)
        valueLabel.frame = NSRect(x: w - 56, y: bounds.height - 20, width: 56, height: 17)
        slider.frame = NSRect(x: 0, y: 0, width: w, height: 20)
    }
}

// MARK: - 弹出面板

final class PanelViewController: NSViewController {
    private var rows: [DisplayRow] = []
    private var timer: Timer?
    private let titleLabel = NSTextField(labelWithString: "")
    private let blankCheck = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let loginCheck = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let autoCheck = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let langPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let hintLabel = NSTextField(labelWithString: "")
    private let stack = NSStackView()
    private let quit = NSButton(title: "", target: nil, action: nil)
    private let sysLabel = NSTextField(labelWithString: "")
    private var sysTimer: Timer?

    /// 切换语言后刷新所有静态文案
    private func applyTexts() {
        titleLabel.stringValue = L(BCTL_T_PANEL_TITLE.rawValue)
        hintLabel.stringValue = L(BCTL_T_HINT.rawValue)
        blankCheck.title = L(BCTL_T_CHECK_BLANK.rawValue)
        loginCheck.title = L(BCTL_T_CHECK_LOGIN.rawValue)
        autoCheck.title = L(BCTL_T_CHECK_AUTO.rawValue)
        quit.title = L(BCTL_T_QUIT.rawValue)
        refreshSystemLine()

        langPopup.removeAllItems()
        langPopup.addItem(withTitle: L(BCTL_T_LANG_AUTO.rawValue))
        langPopup.addItem(withTitle: "中文")
        langPopup.addItem(withTitle: "English")
        switch Prefs.language {
        case "zh": langPopup.selectItem(at: 1)
        case "en": langPopup.selectItem(at: 2)
        default:   langPopup.selectItem(at: 0)
        }
    }

    @objc private func languageChanged(_ sender: NSPopUpButton) {
        let value: String
        switch sender.indexOfSelectedItem {
        case 1: value = "zh"
        case 2: value = "en"
        default: value = "auto"
        }
        Prefs.language = value
        bctl_set_language(value)
        applyTexts()
        rebuild()
    }

    override func loadView() {
        let v = NSView(frame: NSRect(x: 0, y: 0, width: 340, height: 240))
        view = v

        titleLabel.font = .systemFont(ofSize: 15, weight: .semibold)
        titleLabel.textColor = .labelColor

        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false

        hintLabel.font = .systemFont(ofSize: 11)
        hintLabel.textColor = .tertiaryLabelColor

        sysLabel.font = .monospacedDigitSystemFont(ofSize: 11.5, weight: .regular)
        sysLabel.textColor = .secondaryLabelColor
        sysLabel.maximumNumberOfLines = 8
        sysLabel.lineBreakMode = .byWordWrapping

        langPopup.target = self
        langPopup.action = #selector(languageChanged(_:))
        langPopup.font = .systemFont(ofSize: 12)
        langPopup.controlSize = .small

        blankCheck.target = self
        blankCheck.action = #selector(toggleBlank(_:))

        loginCheck.target = self
        loginCheck.action = #selector(toggleLogin(_:))
        loginCheck.state = LoginItem.isEnabled ? .on : .off

        autoCheck.target = self
        autoCheck.action = #selector(toggleAuto(_:))
        autoCheck.state = Prefs.autoBlankBuiltin ? .on : .off

        quit.target = self
        quit.action = #selector(quitApp)
        quit.bezelStyle = .inline
        quit.isBordered = false
        quit.font = .systemFont(ofSize: 12)
        quit.contentTintColor = .secondaryLabelColor

        v.addSubview(titleLabel)
        v.addSubview(stack)
        v.addSubview(hintLabel)
        v.addSubview(blankCheck)
        v.addSubview(loginCheck)
        v.addSubview(autoCheck)
        v.addSubview(langPopup)
        v.addSubview(sysLabel)
        v.addSubview(quit)

        titleLabel.translatesAutoresizingMaskIntoConstraints = false
        hintLabel.translatesAutoresizingMaskIntoConstraints = false
        blankCheck.translatesAutoresizingMaskIntoConstraints = false
        loginCheck.translatesAutoresizingMaskIntoConstraints = false
        autoCheck.translatesAutoresizingMaskIntoConstraints = false
        langPopup.translatesAutoresizingMaskIntoConstraints = false
        sysLabel.translatesAutoresizingMaskIntoConstraints = false
        quit.translatesAutoresizingMaskIntoConstraints = false

        NSLayoutConstraint.activate([
            titleLabel.topAnchor.constraint(equalTo: v.topAnchor, constant: 14),
            titleLabel.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 16),

            stack.topAnchor.constraint(equalTo: titleLabel.bottomAnchor, constant: 10),
            stack.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: v.trailingAnchor, constant: -16),

            hintLabel.topAnchor.constraint(equalTo: stack.bottomAnchor, constant: 8),
            hintLabel.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 16),

            sysLabel.topAnchor.constraint(equalTo: hintLabel.bottomAnchor, constant: 6),
            sysLabel.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 16),
            sysLabel.trailingAnchor.constraint(equalTo: v.trailingAnchor, constant: -16),

            blankCheck.topAnchor.constraint(equalTo: sysLabel.bottomAnchor, constant: 8),
            blankCheck.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 14),

            autoCheck.topAnchor.constraint(equalTo: blankCheck.bottomAnchor, constant: 4),
            autoCheck.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 14),

            loginCheck.topAnchor.constraint(equalTo: autoCheck.bottomAnchor, constant: 4),
            loginCheck.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 14),

            langPopup.topAnchor.constraint(equalTo: loginCheck.bottomAnchor, constant: 8),
            langPopup.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 14),

            quit.topAnchor.constraint(equalTo: langPopup.bottomAnchor, constant: 6),
            quit.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 14),
            quit.bottomAnchor.constraint(equalTo: v.bottomAnchor, constant: -10),
        ])

        rebuild()
        // 面板刷新同样只在可见时进行，避免后台空转
        timer = Timer.scheduledTimer(withTimeInterval: 1.5, repeats: true) { [weak self] _ in
            guard let self, self.viewIfLoaded?.window?.isVisible == true else { return }
            self.refresh()
        }
        RunLoop.main.add(timer!, forMode: .common)
    }

    func reload() {
        rebuild()
    }

    /// 采集并显示系统状态（仅在面板可见时采样，关闭后立刻停止）
    func refreshSystemLine() {
        guard viewIfLoaded?.window?.isVisible == true else { return }
        bctl_sysmon_init()
        var info = bctl_sysinfo()
        bctl_sysmon_sample(&info)

        var lines: [String] = []
        var row1: [String] = []
        if info.cpu_percent >= 0 {
            row1.append(String(format: "%@ %.0f%%", L(BCTL_T_SYS_CPU.rawValue), info.cpu_percent))
        }
        if info.gpu_percent >= 0 {
            row1.append(String(format: "%@ %.0f%%", L(BCTL_T_SYS_GPU.rawValue), info.gpu_percent))
        }
        if info.mem_percent >= 0 {
            row1.append(String(format: "%@ %.0f%%", L(BCTL_T_SYS_MEM.rawValue), info.mem_percent))
        }
        if !row1.isEmpty { lines.append(row1.joined(separator: "   ")) }

        var row2: [String] = []
        if info.temp_available && info.cpu_temp > 0 {
            row2.append(String(format: "%@ %.0f°C", L(BCTL_T_SYS_TEMP.rawValue), info.cpu_temp))
        }
        if info.has_battery && info.battery_percent >= 0 {
            var b = String(format: "%@ %.0f%%", L(BCTL_T_SYS_BATTERY.rawValue), info.battery_percent)
            if info.battery_charging { b += " " + L(BCTL_T_SYS_CHARGING.rawValue) }
            else if info.battery_on_ac { b += " " + L(BCTL_T_SYS_ON_AC.rawValue) }
            row2.append(b)
        }
        if info.disk_percent >= 0 {
            row2.append(String(format: "%@ %.0f%%", L(BCTL_T_SYS_DISK.rawValue), info.disk_percent))
        }
        if !row2.isEmpty { lines.append(row2.joined(separator: "   ")) }

        var row3: [String] = []
        if info.net_available && info.net_rx_bps >= 0 {
            row3.append(String(format: "↓%@ ↑%@",
                               Self.rateText(info.net_rx_bps), Self.rateText(info.net_tx_bps)))
        }
        row3.append(String(format: "%@ %.2f", L(BCTL_T_SYS_LOAD.rawValue), info.load1))
        if info.uptime_seconds > 0 {
            row3.append(String(format: "%@ %@", L(BCTL_T_SYS_UPTIME.rawValue),
                               Self.uptimeText(info.uptime_seconds)))
        }
        lines.append(row3.joined(separator: "   "))

        sysLabel.stringValue = lines.joined(separator: "\n")
    }

    private static func rateText(_ bps: Double) -> String {
        if bps < 0 { return "-" }
        if bps < 1024 { return String(format: "%.0fB/s", bps) }
        if bps < 1048576 { return String(format: "%.0fKB/s", bps / 1024) }
        return String(format: "%.1fMB/s", bps / 1048576)
    }

    private static func uptimeText(_ sec: Double) -> String {
        let s = Int(sec)
        let d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60
        if d > 0 { return "\(d)d \(h)h" }
        if h > 0 { return "\(h)h \(m)m" }
        return "\(m)m"
    }

    func startSystemSampling() {
        sysTimer?.invalidate()
        // 先采一次建立 CPU 基线，再按 2 秒刷新
        _ = bctl_sysmon_cpu_percent()
        sysTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            self?.refreshSystemLine()
        }
        if let t = sysTimer { RunLoop.main.add(t, forMode: .common) }
    }

    func stopSystemSampling() {
        sysTimer?.invalidate()
        sysTimer = nil
    }

    var rowCount: Int { rows.count }
    var enabledRowCount: Int { rows.filter { $0.isControllable }.count }

    private func rebuild() {
        bctl_init()
        var buf = [bctl_display](repeating: bctl_display(), count: Int(BCTL_MAX_DISPLAYS))
        let n = buf.withUnsafeMutableBufferPointer { ptr -> Int in
            Int(bctl_probe(ptr.baseAddress, Int32(BCTL_MAX_DISPLAYS)))
        }
        rows.forEach { $0.removeFromSuperview() }
        rows.removeAll()

        guard n > 0 else { return }
        for i in 0..<n {
            let row = DisplayRow(display: buf[i])
            row.translatesAutoresizingMaskIntoConstraints = false
            stack.addArrangedSubview(row)            // 先入层级，再建跨视图约束
            row.heightAnchor.constraint(equalToConstant: 50).isActive = true
            row.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
            rows.append(row)
        }
        applyTexts()
        let ext = bctl_external_count()
        blankCheck.state = bctl_builtin_is_blank() ? .on : .off
        blankCheck.isEnabled = ext > 0 || bctl_builtin_has_restore_state()
        blankCheck.toolTip = ext > 0
            ? "关闭内建屏显示（已检测到 \(ext) 台外接显示器）"
            : "未检测到外接显示器，关闭内屏后将没有任何画面"
        loginCheck.state = LoginItem.isEnabled ? .on : .off
        autoCheck.state = Prefs.autoBlankBuiltin ? .on : .off

        // 面板高度随屏数变化
        let height = CGFloat(268 + n * 58)
        view.setFrameSize(NSSize(width: 340, height: height))
        preferredContentSize = NSSize(width: 340, height: height)
    }

    private func refresh() {
        guard view.window?.isVisible == true, !bctl_displays_asleep() else { return }
        var buf = [bctl_display](repeating: bctl_display(), count: Int(BCTL_MAX_DISPLAYS))
        let n = buf.withUnsafeMutableBufferPointer { ptr -> Int in
            Int(bctl_refresh(ptr.baseAddress, Int32(BCTL_MAX_DISPLAYS)))
        }
        if n != rows.count {
            rebuild()
            return
        }
        for i in 0..<n {
            rows[i].update(percent: buf[i].percent)
        }
        blankCheck.state = bctl_builtin_is_blank() ? .on : .off
        autoCheck.state = Prefs.autoBlankBuiltin ? .on : .off
    }

    @objc private func toggleAuto(_ sender: NSButton) {
        Prefs.autoBlankBuiltin = (sender.state == .on)
        // 刚打开这个选项时，若当前已有外屏就立刻生效
        if Prefs.autoBlankBuiltin,
           bctl_external_count() > 0,
           !bctl_builtin_is_blank() {
            _ = bctl_builtin_blank(true)
        }
        blankCheck.state = bctl_builtin_is_blank() ? .on : .off
    }

    @objc private func toggleLogin(_ sender: NSButton) {
        let want = sender.state == .on
        if !LoginItem.set(want) {
            sender.state = want ? .off : .on
            let alert = NSAlert()
            alert.messageText = L(BCTL_T_ALERT_LOGIN_TITLE.rawValue)
            alert.informativeText = String(format: L(BCTL_T_ALERT_LOGIN_MSG.rawValue), LoginItem.statusText)
            alert.runModal()
        }
    }

    @objc private func toggleBlank(_ sender: NSButton) {
        let want = sender.state == .on
        // 用户手动点亮内屏，说明此刻想用双屏：同步关掉自动，避免被看门狗黑回去
        if !want && Prefs.autoBlankBuiltin {
            Prefs.autoBlankBuiltin = false
            autoCheck.state = .off
        }
        guard bctl_builtin_blank(want) else {
            sender.state = want ? .off : .on
            let alert = NSAlert()
            alert.messageText = want ? L(BCTL_T_ALERT_BLANK_ON_TITLE.rawValue)
                                     : L(BCTL_T_ALERT_BLANK_OFF_TITLE.rawValue)
            alert.informativeText = want ? L(BCTL_T_ALERT_BLANK_ON_MSG.rawValue)
                                         : L(BCTL_T_ALERT_BLANK_OFF_MSG.rawValue)
            alert.runModal()
            return
        }
    }

    @objc private func quitApp() {
        _ = bctl_builtin_blank(false)
        NSApp.terminate(nil)
    }
}

/// 追加一行诊断日志（/tmp/brightctl_app.log）
func logLine(_ text: String) {
    let path = "/tmp/brightctl_app.log"
    let line = text + "\n"
    if let fh = FileHandle(forWritingAtPath: path) {
        fh.seekToEndOfFile()
        fh.write(line.data(using: .utf8)!)
        fh.closeFile()
    } else {
        try? line.write(toFile: path, atomically: true, encoding: .utf8)
    }
}

// MARK: - 显示器配置变更回调

private func brightctlDisplayChanged(_ display: CGDirectDisplayID,
                                     _ flags: CGDisplayChangeSummaryFlags,
                                     _ userInfo: UnsafeMutableRawPointer?) {
    guard let userInfo else { return }
    logLine("reconfig display=0x\(String(display, radix: 16)) flags=\(flags.rawValue)")
    let delegate = Unmanaged<AppDelegate>.fromOpaque(userInfo).takeUnretainedValue()
    DispatchQueue.main.async {
        delegate.handleDisplaysChanged()
    }
}

// MARK: - 菜单栏

final class AppDelegate: NSObject, NSApplicationDelegate, NSPopoverDelegate {
    private var statusItem: NSStatusItem!
    private var popover: NSPopover!
    private let panel = PanelViewController()
    private var screenLocked = false
    private var screensSleeping = false
    private var terminating = false
    private var lastExternalIDs: Set<UInt32> = []
    private var topologyGeneration = 0
    private var observers: [NSObjectProtocol] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        bctl_set_language((Prefs.language as NSString).utf8String)
        // 命令行开关登录项，便于安装脚本调用
        let args = ProcessInfo.processInfo.arguments
        if args.contains("--enable-login") || args.contains("--disable-login") {
            let on = args.contains("--enable-login")
            let ok = LoginItem.set(on)
            let msg = "login on=\(on) ok=\(ok) status=\(LoginItem.statusText) bundle=\(Bundle.main.bundlePath)\n"
            try? msg.write(toFile: "/tmp/brightctl_login.log", atomically: true, encoding: .utf8)
            NSApp.terminate(nil)
            return
        }

        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem.button {
            let img = NSImage(systemSymbolName: "sun.max.fill", accessibilityDescription: "显示器亮度")
            img?.isTemplate = true
            button.image = img
            button.target = self
            button.action = #selector(togglePopover)
        }

        popover = NSPopover()
        popover.contentViewController = panel
        popover.behavior = .transient
        popover.delegate = self
        popover.animates = true

        CGDisplayRegisterReconfigurationCallback(
            brightctlDisplayChanged,
            Unmanaged.passUnretained(self).toOpaque()
        )

        var log = "launched pid=\(ProcessInfo.processInfo.processIdentifier) statusItem=\(statusItem != nil) image=\(statusItem.button?.image != nil)\n"
        bctl_init()
        var buf = [bctl_display](repeating: bctl_display(), count: Int(BCTL_MAX_DISPLAYS))
        let n = buf.withUnsafeMutableBufferPointer { ptr -> Int in
            Int(bctl_probe(ptr.baseAddress, Int32(BCTL_MAX_DISPLAYS)))
        }
        log += "probe count=\(n)\n"
        for i in 0..<max(0, n) {
            let d = buf[i]
            log += String(format: "  id=0x%x main=%d builtin=%d channel=%d percent=%.1f\n",
                          d.display_id, d.is_main ? 1 : 0, d.is_builtin ? 1 : 0, d.channel, d.percent)
        }
        if !args.contains("--selftest") {
            observePowerAndSession()
            lastExternalIDs = externalIDs()
            // 重启时先恢复未完成快照，再按当前开关决定是否重新黑屏。
            let recovered = !bctl_builtin_has_restore_state() || bctl_builtin_blank(false)
            if recovered { applyAutoBlankIfNeeded() }
            Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
                // 兜底只恢复，不反复强制关闭内屏。
                self?.applyAutoBlankIfNeeded(allowBlank: false)
            }
        }

        // 自检模式：程序化打开面板，确认控件真的能构造与显示，然后退出
        if ProcessInfo.processInfo.arguments.contains("--selftest") {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { [weak self] in
                guard let self, let button = self.statusItem.button else { return }
                self.panel.reload()
                self.popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) {
                    var s = log
                    s += "selftest popoverShown=\(self.popover.isShown)"
                    s += " rows=\(self.panel.rowCount)"
                    s += " rowsEnabled=\(self.panel.enabledRowCount)\n"
                    s += "lang setting=\(Prefs.language) effective=\(String(cString: bctl_language()))\n"
                    s += "texts title=[\(L(BCTL_T_PANEL_TITLE.rawValue))] blank=[\(L(BCTL_T_CHECK_BLANK.rawValue))] quit=[\(L(BCTL_T_QUIT.rawValue))]\n"
                    s += "blank external=\(bctl_external_count())\n"
                    let b1 = bctl_builtin_blank(true)
                    s += "blank on=\(b1) isBlank=\(bctl_builtin_is_blank())\n"
                    let b2 = bctl_builtin_blank(false)
                    s += "blank off=\(b2) isBlank=\(bctl_builtin_is_blank())\n"
                    setenv("BRIGHTCTL_TEST_NO_EXTERNAL", "1", 1)
                    let b3 = bctl_builtin_blank(true)
                    s += "guard blank=\(b3) expect=false external=\(bctl_external_count())\n"
                    // 模拟「内屏已黑屏 + 外屏拔掉」：触发变更处理，应自动恢复内屏
                    unsetenv("BRIGHTCTL_TEST_NO_EXTERNAL")
                    _ = bctl_builtin_blank(true)
                    let pre = bctl_builtin_is_blank()
                    setenv("BRIGHTCTL_TEST_NO_EXTERNAL", "1", 1)
                    self.applyAutoBlankIfNeeded()
                    let post = bctl_builtin_is_blank()
                    unsetenv("BRIGHTCTL_TEST_NO_EXTERNAL")
                    s += "autoRecover preBlank=\(pre) postBlank=\(post) expect post=false\n"
                    s += "autoBlank enabled=\(Prefs.autoBlankBuiltin) external=\(bctl_external_count())\n"
                    self.applyAutoBlankIfNeeded()
                    s += "  afterAuto blanked=\(bctl_builtin_is_blank()) expect=true\n"
                    _ = bctl_builtin_blank(false)
                    s += "login status=\(LoginItem.statusText)\n"
                    try? s.write(toFile: "/tmp/brightctl_app.log", atomically: true, encoding: .utf8)
                    NSApp.terminate(nil)
                }
            }
        }
        try? log.write(toFile: "/tmp/brightctl_app.log", atomically: true, encoding: .utf8)
    }

    private var lastDecision = ""
    private var isApplying = false

    private func externalIDs() -> Set<UInt32> {
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(BCTL_MAX_DISPLAYS))
        var count: UInt32 = 0
        guard CGGetOnlineDisplayList(UInt32(ids.count), &ids, &count) == .success else { return lastExternalIDs }
        return Set(ids.prefix(Int(count)).filter { CGDisplayIsBuiltin($0) == 0 })
    }

    private func observePowerAndSession() {
        let nc = NSWorkspace.shared.notificationCenter
        for name in [NSWorkspace.screensDidSleepNotification, NSWorkspace.willSleepNotification] {
            observers.append(nc.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                self?.screensSleeping = true
            })
        }
        for name in [NSWorkspace.screensDidWakeNotification, NSWorkspace.didWakeNotification] {
            observers.append(nc.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                self?.screensSleeping = false
                self?.scheduleTopologyCheck(afterWake: true)
            })
        }
        let dn = DistributedNotificationCenter.default()
        observers.append(dn.addObserver(forName: NSNotification.Name("com.apple.screenIsLocked"), object: nil, queue: .main) { [weak self] _ in
            self?.screenLocked = true
            self?.popover.performClose(nil)
        })
        observers.append(dn.addObserver(forName: NSNotification.Name("com.apple.screenIsUnlocked"), object: nil, queue: .main) { [weak self] _ in
            self?.screenLocked = false
            self?.scheduleTopologyCheck(afterWake: true)
        })
    }

    /// 恢复优先；关闭动作仅在启动/插拔/唤醒后进行，不由定时器反复强制执行。
    func applyAutoBlankIfNeeded(allowBlank: Bool = true) {
        guard !terminating, !isApplying else { return }
        // 锁屏/睡眠时不提交任何显示配置，即使拔线也保留快照待解锁/唤醒恢复。
        if screenLocked || screensSleeping || bctl_displays_asleep() { return }
        let ext = bctl_external_count()
        guard ext >= 0 else { return } // 查询失败不是拔线
        let pending = bctl_builtin_has_restore_state()
        var action = "none"
        isApplying = true
        defer { isApplying = false }
        if ext == 0 && pending {
            let ok = bctl_builtin_blank(false)
            action = ok ? "restore(no-external)" : "restore-failed(no-external)"
        } else if ext > 0 && allowBlank && !screenLocked && !screensSleeping && !bctl_displays_asleep() {
            if Prefs.autoBlankBuiltin && !bctl_builtin_is_blank() {
                let ok = bctl_builtin_blank(true)
                action = ok ? "blank(auto)" : "blank-failed(auto)"
            }
        }
        let decision = "ext=\(ext) pending=\(pending) auto=\(Prefs.autoBlankBuiltin) action=\(action)"
        if decision != lastDecision {
            lastDecision = decision
            logLine("autoBlank " + decision)
        }
    }

    private func scheduleTopologyCheck(afterWake: Bool = false) {
        guard !terminating else { return }
        topologyGeneration += 1
        let generation = topologyGeneration
        // 合并重配置事件，再有限次确认；镜像自身产生的回调不会再次强制黑屏。
        for delay in [0.35, 1.0, 3.0] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                guard let self, !self.terminating, self.topologyGeneration == generation else { return }
                let now = self.externalIDs()
                let changed = now != self.lastExternalIDs
                self.lastExternalIDs = now
                self.applyAutoBlankIfNeeded(allowBlank: changed || afterWake)
                if self.popover.isShown && !self.screenLocked && !self.screensSleeping {
                    self.panel.reload()
                }
            }
        }
    }

    func handleDisplaysChanged() { scheduleTopologyCheck() }

    /// 所有关闭路径（点外部、Esc、performClose）都会到这里，统一停掉采样
    func popoverDidClose(_ notification: Notification) {
        panel.stopSystemSampling()
    }

    /// 打开时才启动采样，保证后台零轮询
    func popoverWillShow(_ notification: Notification) {
        panel.startSystemSampling()
    }

    @objc private func togglePopover() {
        guard let button = statusItem.button else { return }
        if popover.isShown {
            popover.performClose(nil)      // 关闭事件由 popoverDidClose 统一收尾
        } else {
            panel.reload()
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
            popover.contentViewController?.view.window?.makeKey()
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        terminating = true
        topologyGeneration += 1
        CGDisplayRemoveReconfigurationCallback(
            brightctlDisplayChanged,
            Unmanaged.passUnretained(self).toOpaque()
        )
        _ = bctl_builtin_blank(false)
    }
}

// MARK: - 入口

@main
struct BrightCtlMain {
    static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.accessory)
        withExtendedLifetime(delegate) {
            app.run()
        }
    }
}
