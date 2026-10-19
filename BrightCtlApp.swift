import Cocoa
import Foundation
import ServiceManagement

// MARK: - 偏好设置

enum Prefs {
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
    private let queue = DispatchQueue(label: "brightctl.write", qos: .userInitiated)
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

        var title = display.is_builtin ? "内建屏" : "外接屏"
        if display.is_main { title += "（主屏）" }
        title += String(format: "  0x%x", display.display_id)
        nameLabel.stringValue = title
        nameLabel.font = .systemFont(ofSize: 12, weight: .medium)
        nameLabel.textColor = .labelColor

        valueLabel.font = .monospacedDigitSystemFont(ofSize: 12, weight: .regular)
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
    private let titleLabel = NSTextField(labelWithString: "显示器亮度")
    private let blankCheck = NSButton(checkboxWithTitle: "关闭内建屏（黑屏）", target: nil, action: nil)
    private let loginCheck = NSButton(checkboxWithTitle: "开机自动启动", target: nil, action: nil)
    private let autoCheck = NSButton(checkboxWithTitle: "接上外屏时自动关闭内屏", target: nil, action: nil)
    private let hintLabel = NSTextField(labelWithString: "")
    private let stack = NSStackView()

    override func loadView() {
        let v = NSView(frame: NSRect(x: 0, y: 0, width: 320, height: 220))
        view = v

        titleLabel.font = .systemFont(ofSize: 13, weight: .semibold)
        titleLabel.textColor = .labelColor

        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false

        hintLabel.font = .systemFont(ofSize: 10)
        hintLabel.textColor = .tertiaryLabelColor
        hintLabel.stringValue = "拖动即生效"

        blankCheck.target = self
        blankCheck.action = #selector(toggleBlank(_:))

        loginCheck.target = self
        loginCheck.action = #selector(toggleLogin(_:))
        loginCheck.state = LoginItem.isEnabled ? .on : .off

        autoCheck.target = self
        autoCheck.action = #selector(toggleAuto(_:))
        autoCheck.state = Prefs.autoBlankBuiltin ? .on : .off

        let quit = NSButton(title: "退出 BrightCtl", target: self, action: #selector(quitApp))
        quit.bezelStyle = .inline
        quit.isBordered = false
        quit.font = .systemFont(ofSize: 11)
        quit.contentTintColor = .secondaryLabelColor

        v.addSubview(titleLabel)
        v.addSubview(stack)
        v.addSubview(hintLabel)
        v.addSubview(blankCheck)
        v.addSubview(loginCheck)
        v.addSubview(autoCheck)
        v.addSubview(quit)

        titleLabel.translatesAutoresizingMaskIntoConstraints = false
        hintLabel.translatesAutoresizingMaskIntoConstraints = false
        blankCheck.translatesAutoresizingMaskIntoConstraints = false
        loginCheck.translatesAutoresizingMaskIntoConstraints = false
        autoCheck.translatesAutoresizingMaskIntoConstraints = false
        quit.translatesAutoresizingMaskIntoConstraints = false

        NSLayoutConstraint.activate([
            titleLabel.topAnchor.constraint(equalTo: v.topAnchor, constant: 14),
            titleLabel.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 16),

            stack.topAnchor.constraint(equalTo: titleLabel.bottomAnchor, constant: 10),
            stack.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: v.trailingAnchor, constant: -16),

            hintLabel.topAnchor.constraint(equalTo: stack.bottomAnchor, constant: 8),
            hintLabel.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 16),

            blankCheck.topAnchor.constraint(equalTo: hintLabel.bottomAnchor, constant: 8),
            blankCheck.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 14),

            autoCheck.topAnchor.constraint(equalTo: blankCheck.bottomAnchor, constant: 4),
            autoCheck.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 14),

            loginCheck.topAnchor.constraint(equalTo: autoCheck.bottomAnchor, constant: 4),
            loginCheck.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 14),

            quit.topAnchor.constraint(equalTo: loginCheck.bottomAnchor, constant: 6),
            quit.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 14),
            quit.bottomAnchor.constraint(equalTo: v.bottomAnchor, constant: -10),
        ])

        rebuild()
        timer = Timer.scheduledTimer(withTimeInterval: 1.5, repeats: true) { [weak self] _ in
            self?.refresh()
        }
        RunLoop.main.add(timer!, forMode: .common)
    }

    func reload() {
        rebuild()
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
            row.heightAnchor.constraint(equalToConstant: 46).isActive = true
            row.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
            rows.append(row)
        }
        let ext = bctl_external_count()
        blankCheck.state = bctl_builtin_is_blank() ? .on : .off
        blankCheck.isEnabled = ext > 0
        blankCheck.toolTip = ext > 0
            ? "关闭内建屏显示（已检测到 \(ext) 台外接显示器）"
            : "未检测到外接显示器，关闭内屏后将没有任何画面"
        loginCheck.state = LoginItem.isEnabled ? .on : .off
        autoCheck.state = Prefs.autoBlankBuiltin ? .on : .off

        // 面板高度随屏数变化
        let height = CGFloat(150 + n * 56)
        view.setFrameSize(NSSize(width: 320, height: height))
        preferredContentSize = NSSize(width: 320, height: height)
    }

    private func refresh() {
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
            alert.messageText = "无法设置开机自启"
            alert.informativeText = "请先把 BrightCtl.app 安装到 /Applications（make install），再重试。当前状态：\(LoginItem.statusText)"
            alert.runModal()
        }
    }

    @objc private func toggleBlank(_ sender: NSButton) {
        let want = sender.state == .on
        guard bctl_builtin_blank(want) else {
            sender.state = want ? .off : .on
            let alert = NSAlert()
            alert.messageText = want ? "无法关闭内建屏" : "无法恢复内建屏"
            alert.informativeText = want
                ? "未检测到外接显示器。为免失去所有画面，已拒绝关闭内屏。"
                : "释放捕获失败，请重新打开面板重试。"
            alert.runModal()
            return
        }
    }

    @objc private func quitApp() {
        _ = bctl_builtin_blank(false)
        NSApp.terminate(nil)
    }
}

// MARK: - 显示器配置变更回调

private func brightctlDisplayChanged(_ display: CGDirectDisplayID,
                                     _ flags: CGDisplayChangeSummaryFlags,
                                     _ userInfo: UnsafeMutableRawPointer?) {
    guard let userInfo else { return }
    let delegate = Unmanaged<AppDelegate>.fromOpaque(userInfo).takeUnretainedValue()
    DispatchQueue.main.async {
        delegate.handleDisplaysChanged()
    }
}

// MARK: - 菜单栏

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem!
    private var popover: NSPopover!
    private let panel = PanelViewController()

    func applicationDidFinishLaunching(_ notification: Notification) {
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
        // 启动即按当前外屏情况执行一次（自检模式除外，避免干扰）
        if !ProcessInfo.processInfo.arguments.contains("--selftest") {
            applyAutoBlankIfNeeded()
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
                    self.handleDisplaysChanged()
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

    /// 统一处理内屏开关：有外屏且开启自动 → 关内屏；无外屏 → 必开内屏
    func applyAutoBlankIfNeeded() {
        let ext = bctl_external_count()
        if ext == 0 {
            if bctl_builtin_is_blank() { _ = bctl_builtin_blank(false) }
        } else if Prefs.autoBlankBuiltin && !bctl_builtin_is_blank() {
            _ = bctl_builtin_blank(true)
        }
    }

    /// 显示器增删/启停后调用：外屏没了就强制恢复内屏
    func handleDisplaysChanged() {
        _ = bctl_probe(nil, 0)
        applyAutoBlankIfNeeded()
        panel.reload()
        let ext = bctl_external_count()
        let msg = "displaysChanged external=\(ext) blanked=\(bctl_builtin_is_blank())\n"
        if let fh = FileHandle(forWritingAtPath: "/tmp/brightctl_app.log") {
            fh.seekToEndOfFile()
            fh.write(msg.data(using: .utf8)!)
            fh.closeFile()
        }
    }

    @objc private func togglePopover() {
        guard let button = statusItem.button else { return }
        if popover.isShown {
            popover.performClose(nil)
        } else {
            panel.reload()
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
            popover.contentViewController?.view.window?.makeKey()
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
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
