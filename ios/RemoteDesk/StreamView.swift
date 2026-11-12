import AVFoundation
import CoreMotion
import SwiftUI
import UIKit

/// Một view UIKit tự xử lý mọi ngón chạm (như Moonlight) để đa chạm ổn định. Mỗi ngón được gán lúc chạm xuống:
/// trúng nút thì nút đó giữ ngón đến khi nhấc, trúng vùng cần di chuyển thì điều khiển cần, còn lại là "nền"
/// (trackpad, chạm trực tiếp, hoặc vuốt xoay camera tuỳ hồ sơ).
final class StreamUIView: UIView, UIKeyInput {
    private weak var session: Session?
    private var keys: [KeyButton] = []
    private var caps: [KeyCap] = []
    private var editing = false, keysHidden = false
    private var keyAlpha: CGFloat = 0.85

    private var keyTouches: [UITouch: Int] = [:]           // ngón đang giữ nút/cần
    private var dragStart: [UITouch: CGPoint] = [:], dragMoved: Set<UITouch> = []
    private var stickOrigin: [UITouch: CGPoint] = [:]      // tâm cần lúc ngón đặt xuống
    private var stickHeld: [Int: Set<String>] = [:]        // phím cần đang giữ
    private var latched: Set<Int> = []                     // nút bật/tắt đang bật
    private var turbo: [UITouch: Timer] = [:]              // nút liên thanh đang giữ
    private var pad: Set<UITouch> = []
    private var directDown: String?                        // nút chuột đang giữ do chạm nền kiểu trực tiếp
    private var padStart = 0.0, padMax = 0, padMoved: CGFloat = 0, padLast = CGPoint.zero
    private var mouseAcc = CGPoint.zero, wheelAcc = CGPoint.zero
    private let motion = CMMotionManager()
    private var gyroMode = GyroMode.off, lastGyro: TimeInterval = 0
    private let haptic = UIImpactFeedbackGenerator(style: .light)

    init(session: Session) {
        self.session = session
        super.init(frame: .zero)
        backgroundColor = .black
        isMultipleTouchEnabled = true
        layer.addSublayer(session.renderer.layer)
    }
    required init?(coder: NSCoder) { fatalError() }

    override func layoutSubviews() {
        super.layoutSubviews()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        session?.renderer.layer.frame = bounds
        CATransaction.commit()
        if stickOrigin.isEmpty { for i in caps.indices { caps[i].center = home(i) } }
    }

    override func willMove(toWindow newWindow: UIWindow?) {
        if newWindow == nil {
            motion.stopDeviceMotionUpdates()
            gyroMode = .off
            releaseAll()
        }
    }

    func apply(keymap: [KeyButton], editing: Bool, hidden: Bool, fade: Double, gyro: GyroMode) {
        let dragging = self.editing && !keyTouches.isEmpty  // đang kéo nút trong chế độ sửa: chưa dựng lại
        if (keymap != keys && !dragging) || editing != self.editing || (hidden && !keysHidden) { releaseAll() }
        if keymap != keys && !dragging {
            keys = keymap
            caps.forEach { $0.removeFromSuperview() }
            caps = keys.map { k in
                let c = KeyCap(k)
                addSubview(c)
                return c
            }
            for i in caps.indices { caps[i].center = home(i) }
        }
        self.editing = editing
        keysHidden = hidden
        keyAlpha = CGFloat(1 - fade)
        caps.forEach { $0.isHidden = hidden; $0.editing = editing; $0.alpha = $0.pressed || editing ? 1 : keyAlpha }
        setGyro(gyro)
    }

    private func home(_ i: Int) -> CGPoint { CGPoint(x: CGFloat(keys[i].x) * bounds.width, y: CGFloat(keys[i].y) * bounds.height) }

    /// Nút thường xét trước (kể cả nằm trong vùng cần), rồi tới vùng cần. Cần nổi nhận ngón trong bán kính gấp 2,2 lần đế.
    private func control(at p: CGPoint) -> Int? {
        guard !keysHidden else { return nil }
        if let i = caps.indices.reversed().first(where: { !keys[$0].isStick && caps[$0].frame.insetBy(dx: -8, dy: -8).contains(p) }) {
            return i
        }
        return caps.indices.reversed().first { i in
            guard keys[i].isStick else { return false }
            let h = home(i), zone = KeyCap.stickRadius(keys[i]) * (editing || keys[i].floating == false ? 1.1 : 2.2)
            return hypot(p.x - h.x, p.y - h.y) <= zone
        }
    }

    private func send(_ m: [String: Any]) { session?.send(m) }
    private func keyDown(_ k: String) { if !k.isEmpty { send(["t": "k", "keys": k, "d": 1]) } }
    private func keyUp(_ k: String) { if !k.isEmpty { send(["t": "k", "keys": k, "d": 0]) } }
    private func setPressed(_ i: Int, _ on: Bool) {
        caps[i].pressed = on
        caps[i].alpha = on || editing ? 1 : keyAlpha  // nút mờ vẫn hiện rõ lúc đang bấm
    }

    /// Nhả mọi thứ đang giữ (đổi hồ sơ, vào chế độ sửa, ẩn nút, rời màn hình) để PC không bị kẹt phím.
    private func releaseAll() {
        for (t, i) in keyTouches where i < keys.count && !keys[i].isStick && (keys[i].mode ?? .hold) == .hold && turbo[t] == nil {
            keyUp(keys[i].keys)
        }
        turbo.values.forEach { $0.invalidate() }
        for held in stickHeld.values { held.forEach(keyUp) }
        for i in latched where i < keys.count { keyUp(keys[i].keys) }
        if let b = directDown { keyUp(b) }
        directDown = nil
        turbo.removeAll()
        stickHeld.removeAll()
        latched.removeAll()
        keyTouches.removeAll()
        stickOrigin.removeAll()
        dragStart.removeAll()
        dragMoved.removeAll()
        for i in caps.indices {
            setPressed(i, false)
            caps[i].knob(nil)
            caps[i].center = home(i)
        }
    }

    // MARK: chạm

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches {
            let p = t.location(in: self)
            guard let i = control(at: p) else { padBegan(t); continue }
            keyTouches[t] = i
            dragStart[t] = p
            if editing { continue }
            haptic.impactOccurred()
            if keys[i].isStick {
                let origin = keys[i].floating == false ? home(i) : p  // cần nổi: đế dời tới chỗ ngón đặt
                stickOrigin[t] = origin
                caps[i].center = origin
                setPressed(i, true)
                stickMoved(t, i)
            } else {
                pressBegan(t, i)
            }
        }
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        var padChanged = false
        var look = CGPoint.zero
        for t in touches {
            let p = t.location(in: self), q = t.previousLocation(in: self)
            if let i = keyTouches[t], i < keys.count {
                if editing {
                    let s = dragStart[t] ?? p
                    if hypot(p.x - s.x, p.y - s.y) > 8 { dragMoved.insert(t) }
                    guard dragMoved.contains(t) else { continue }
                    keys[i].x = Double(min(max((p.x / bounds.width * 100).rounded() / 100, 0), 1))  // bám lưới 1% cho dễ thẳng hàng
                    keys[i].y = Double(min(max((p.y / bounds.height * 100).rounded() / 100, 0), 1))
                    caps[i].center = home(i)
                } else if keys[i].isStick {
                    stickMoved(t, i)
                } else if keys[i].aim == true {  // nút bắn/ngắm: vừa giữ vừa kéo để xoay camera
                    look.x += p.x - q.x
                    look.y += p.y - q.y
                }
            } else if pad.contains(t) {
                if session?.mode == .look && !isDirect {
                    look.x += p.x - q.x
                    look.y += p.y - q.y
                } else {
                    padChanged = true
                }
            }
        }
        if look != .zero {
            let s = CGFloat(session?.lookSens ?? 1.5)
            moveMouse(look.x * s, look.y * s)
        }
        if padChanged { padMovedNow() }
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches {
            if let i = keyTouches.removeValue(forKey: t), i < keys.count {
                dragStart[t] = nil
                if editing {
                    if dragMoved.remove(t) != nil { session?.keymap = keys } else { session?.editIndex = i }
                } else if keys[i].isStick {
                    stickEnded(t, i)
                } else {
                    pressEnded(t, i)
                }
            } else if pad.contains(t) {
                padEnded(t)
            }
        }
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { touchesEnded(touches, with: event) }

    // MARK: nút: giữ, bật/tắt, liên thanh

    private func pressBegan(_ t: UITouch, _ i: Int) {
        let k = keys[i].keys
        switch keys[i].mode ?? .hold {
        case .hold:
            keyDown(k)
            setPressed(i, true)
        case .toggle:
            if latched.remove(i) != nil {
                keyUp(k)
                setPressed(i, false)
            } else {
                latched.insert(i)
                keyDown(k)
                setPressed(i, true)
            }
        case .turbo:  // ~15 lần/giây như "repeated tap" của BlueStacks
            setPressed(i, true)
            session?.tapKey(k)
            turbo[t] = Timer.scheduledTimer(withTimeInterval: 1.0 / 15, repeats: true) { [weak self] _ in self?.session?.tapKey(k) }
        }
    }

    private func pressEnded(_ t: UITouch, _ i: Int) {
        switch keys[i].mode ?? .hold {
        case .hold:
            keyUp(keys[i].keys)
            setPressed(i, false)
        case .toggle:
            break
        case .turbo:
            turbo.removeValue(forKey: t)?.invalidate()
            setPressed(i, false)
        }
    }

    // MARK: cần di chuyển

    private func stickMoved(_ t: UITouch, _ i: Int) {
        guard let o = stickOrigin[t] else { return }
        let p = t.location(in: self), r = KeyCap.stickRadius(keys[i])
        let dx = p.x - o.x, dy = p.y - o.y, len = hypot(dx, dy), lim = r * 0.58
        caps[i].knob(len > lim ? CGPoint(x: dx / len * lim, y: dy / len * lim) : CGPoint(x: dx, y: dy))
        let want = Stick.keys(dx: Double(dx), dy: Double(dy), radius: Double(r), map: keys[i].keys, sprint: keys[i].sprint)
        let have = stickHeld[i] ?? []
        have.subtracting(want).forEach(keyUp)
        want.subtracting(have).forEach(keyDown)
        stickHeld[i] = want
    }

    private func stickEnded(_ t: UITouch, _ i: Int) {
        stickHeld.removeValue(forKey: i)?.forEach(keyUp)
        stickOrigin[t] = nil
        caps[i].knob(nil)
        caps[i].center = home(i)
        setPressed(i, false)
    }

    // MARK: nền màn hình

    private var padCenter: CGPoint {
        let ps = pad.map { $0.location(in: self) }
        guard !ps.isEmpty else { return .zero }
        return CGPoint(x: ps.map(\.x).reduce(0, +) / CGFloat(ps.count), y: ps.map(\.y).reduce(0, +) / CGFloat(ps.count))
    }
    private var directButton: String { session?.mode == .directRight ? "RMB" : "LMB" }
    /// Nút "Chuột" đang bật hoặc đang giữ: game hiện con trỏ (túi đồ, bản đồ, giữ ALT) nên chạm nền phải bấm đúng chỗ.
    private var cursorOn: Bool {
        latched.union(keyTouches.values).contains { $0 < keys.count && keys[$0].cursor == true }
    }
    private var isDirect: Bool { session?.mode == .direct || session?.mode == .directRight || cursorOn }

    private func padBegan(_ t: UITouch) {
        if pad.isEmpty { padStart = CACurrentMediaTime(); padMax = 0; padMoved = 0 }
        pad.insert(t)
        padMax = max(padMax, pad.count)
        padLast = padCenter
        if isDirect && pad.count == 1 && directDown == nil {
            sendAbsolute(t.location(in: self))
            directDown = directButton
            keyDown(directButton)
        }
    }

    private func padMovedNow() {
        let c = padCenter
        let dx = c.x - padLast.x, dy = c.y - padLast.y
        padLast = c
        padMoved += abs(dx) + abs(dy)
        if isDirect {
            if let t = pad.first { sendAbsolute(t.location(in: self)) }
        } else if pad.count == 1 {
            let s = CGFloat(session?.sensitivity ?? 2.2)
            moveMouse(dx * s, dy * s)
        } else {  // hai ngón: cuộn tự nhiên
            wheelAcc.x -= dx * 4
            wheelAcc.y += dy * 4
            let ix = wheelAcc.x.rounded(.towardZero), iy = wheelAcc.y.rounded(.towardZero)
            wheelAcc.x -= ix
            wheelAcc.y -= iy
            if ix != 0 || iy != 0 { send(["t": "w", "dx": Int(ix), "dy": Int(iy)]) }
        }
    }

    private func padEnded(_ t: UITouch) {
        pad.remove(t)
        padLast = padCenter
        guard pad.isEmpty else { return }
        if let b = directDown {
            keyUp(b)
            directDown = nil
        } else if session?.mode == .trackpad && padMoved < 10 && CACurrentMediaTime() - padStart < 0.3 {  // chạm nhanh: 1 ngón trái, 2 ngón phải
            session?.tapKey(padMax >= 2 ? "RMB" : "LMB")
        }
    }

    /// Chuột tương đối; dồn phần lẻ để kéo chậm vẫn chính xác.
    private func moveMouse(_ dx: CGFloat, _ dy: CGFloat) {
        mouseAcc.x += dx
        mouseAcc.y += dy
        let ix = mouseAcc.x.rounded(.towardZero), iy = mouseAcc.y.rounded(.towardZero)
        mouseAcc.x -= ix
        mouseAcc.y -= iy
        if ix != 0 || iy != 0 { send(["t": "r", "dx": Int(ix), "dy": Int(iy)]) }
    }

    private func sendAbsolute(_ p: CGPoint) {
        guard let size = session?.renderer.videoSize else { return }
        let r = AVMakeRect(aspectRatio: size, insideRect: bounds)
        send(["t": "m", "x": Double(min(max((p.x - r.minX) / r.width, 0), 1)), "y": Double(min(max((p.y - r.minY) / r.height, 0), 1))])
    }

    // MARK: con quay (gyro): cách ngắm Xbox Cloud Gaming khuyên dùng cho game bắn súng

    private func setGyro(_ m: GyroMode) {
        guard m != gyroMode, window != nil else { return }
        gyroMode = m
        motion.stopDeviceMotionUpdates()
        guard m != .off, motion.isDeviceMotionAvailable else { return }
        motion.deviceMotionUpdateInterval = 1.0 / 120
        lastGyro = 0
        motion.startDeviceMotionUpdates(to: .main) { [weak self] dm, _ in
            if let dm { self?.gyroTick(dm) }
        }
    }

    private var aiming: Bool {
        keyTouches.values.contains { $0 < keys.count && keys[$0].aim == true } || latched.contains { $0 < keys.count && keys[$0].aim == true }
    }

    private func gyroTick(_ m: CMDeviceMotion) {
        defer { lastGyro = m.timestamp }
        guard lastGyro > 0, !editing, gyroMode == .always || (gyroMode == .aim && aiming) else { return }
        let dt = min(m.timestamp - lastGyro, 0.05)
        let r = m.rotationRate  // rad/s, hệ máy đã bù sai lệch
        // Đổi trục của máy ra quay ngang/ngẩng lên theo cách đang cầm
        var yaw = -r.y, pitch = -r.x  // dọc
        switch window?.windowScene?.interfaceOrientation ?? .landscapeRight {
        case .landscapeRight: yaw = -r.x; pitch = r.y
        case .landscapeLeft: yaw = r.x; pitch = -r.y
        case .portraitUpsideDown: yaw = r.y; pitch = r.x
        default: break
        }
        let k = 600 * (session?.gyroSens ?? 1) * dt  // 1 radian ≈ 600 điểm ảnh ở độ nhạy 1×
        moveMouse(CGFloat(yaw * k), CGFloat(pitch * k * ((session?.gyroInvertY ?? false) ? -1 : 1)))
    }

    // MARK: bàn phím iOS

    func toggleKeyboard() { _ = isFirstResponder ? resignFirstResponder() : becomeFirstResponder() }
    override var canBecomeFirstResponder: Bool { true }
    var hasText: Bool { true }  // để phím xoá luôn gửi được
    var autocorrectionType: UITextAutocorrectionType = .no
    var autocapitalizationType: UITextAutocapitalizationType = .none
    var spellCheckingType: UITextSpellCheckingType = .no
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartDashesType: UITextSmartDashesType = .no
    func insertText(_ text: String) {
        if text == "\n" { session?.tapKey("ENTER") } else { send(["t": "text", "s": text]) }
    }
    func deleteBackward() { session?.tapKey("BACK") }
}

/// Nút keymap hoặc cần di chuyển: viên mờ như rãnh điều khiển của Apple, không bóng; đang bấm thì xanh #0071e3.
/// Không nhận chạm, StreamUIView tự xét vị trí.
final class KeyCap: UIView {
    static func stickRadius(_ k: KeyButton) -> CGFloat { 58 * CGFloat(k.scale) }

    private let blur = UIVisualEffectView(effect: UIBlurEffect(style: .systemUltraThinMaterial))
    private let label = UILabel()
    private let knobView = UIView()
    private let isStick: Bool

    var pressed = false { didSet { restyle() } }
    var editing = false { didSet { restyle() } }

    init(_ k: KeyButton) {
        isStick = k.isStick
        super.init(frame: .zero)
        isUserInteractionEnabled = false
        let s = CGFloat(k.scale)
        label.text = k.label
        label.textAlignment = .center
        label.adjustsFontSizeToFitWidth = true
        label.minimumScaleFactor = 0.6
        if isStick {
            let r = Self.stickRadius(k), kr = r * 0.42
            frame = CGRect(x: 0, y: 0, width: r * 2, height: r * 2)
            layer.cornerRadius = r
            knobView.frame = CGRect(x: r - kr, y: r - kr, width: kr * 2, height: kr * 2)
            knobView.layer.cornerRadius = kr
            label.font = .systemFont(ofSize: 11 * s, weight: .semibold)
            label.frame = knobView.bounds.insetBy(dx: 4, dy: 0)
        } else {
            label.font = .systemFont(ofSize: 15 * s, weight: .semibold)
            let h = 52 * s
            frame = CGRect(x: 0, y: 0, width: max(h, label.intrinsicContentSize.width + 24 * s), height: h)
            layer.cornerRadius = h / 2
            label.frame = bounds.insetBy(dx: 6, dy: 0)
        }
        layer.cornerCurve = .continuous
        clipsToBounds = true
        blur.frame = bounds
        addSubview(blur)
        if isStick {
            addSubview(knobView)
            knobView.addSubview(label)
        } else {
            addSubview(label)
        }
        restyle()  // trong init, gán thuộc tính không gọi didSet
    }
    required init?(coder: NSCoder) { fatalError() }

    /// Núm cần lệch khỏi tâm đế (nil = về giữa).
    func knob(_ offset: CGPoint?) {
        knobView.center = CGPoint(x: bounds.midX + (offset?.x ?? 0), y: bounds.midY + (offset?.y ?? 0))
    }

    private func restyle() {
        let blue = UIColor(hex: 0x0071E3)
        if isStick {
            blur.contentView.backgroundColor = Theme.uiOverlay
            knobView.backgroundColor = pressed ? blue : UIColor.dyn(0xFFFFFF, 0x636366, alpha: 0.9)
        } else {
            blur.contentView.backgroundColor = pressed ? blue : Theme.uiOverlay
        }
        label.textColor = pressed ? .white : Theme.uiOverlayInk
        layer.borderWidth = editing ? 2 : 0
        layer.borderColor = UIColor.dyn(0x0066CC, 0x2997FF).resolvedColor(with: traitCollection).cgColor
    }
}

struct StreamCanvas: UIViewRepresentable {
    @ObservedObject var session: Session

    func makeUIView(context: Context) -> StreamUIView {
        let v = StreamUIView(session: session)
        session.view = v
        return v
    }

    func updateUIView(_ v: StreamUIView, context: Context) {
        v.apply(keymap: session.keymap, editing: session.editing, hidden: session.keysHidden, fade: session.keyFade,
                gyro: session.gyro)
    }
}

struct EditTarget: Identifiable { let id: Int }

struct StreamView: View {
    @ObservedObject var session: Session
    var demo = false  // ảnh chụp giao diện trên CI: không có PC, giả đang phát, mở sẵn bảng
    var close: () -> Void
    @State private var user = ""
    @State private var pass = ""
    @State private var panel = false
    @State private var pickPhotos = false
    @State private var pickFiles = false

    var body: some View {
        ZStack(alignment: .top) {
            Color.black.ignoresSafeArea()
                .sheet(isPresented: Binding(get: { session.state == .badLogin }, set: { if !$0 && session.state == .badLogin { close() } })) {
                    LoginSheet(title: "Đăng nhập",
                               message: "Sai tên hoặc mật khẩu, hoặc PC đã đổi tài khoản. Dùng tài khoản tạo trên cửa sổ Remote Desk ở PC.",
                               user: $user, pass: $pass) {
                        session.retry(user: user, password: pass)
                        pass = ""
                    } cancel: { close() }
                }
            StreamCanvas(session: session).ignoresSafeArea()
                .sheet(isPresented: $pickPhotos) { PhotoPicker { session.send(files: $0) }.ignoresSafeArea() }
            // Giữa mép trên để trống (các mẫu keymap chừa chỗ): một nút nhỏ mở bảng, hoặc thanh sửa keymap
            VStack(spacing: 8) {
                if session.editing { editBar } else if !panel { handle }  // bảng đang mở: nút mở bảng không ló ra cạnh bảng
                if let tr = session.transfer {
                    TransferBadge(transfer: tr) { session.transfer = nil }
                }
                Spacer()
                if session.stats != .off && session.state == .streaming && !panel {  // thông số đã có ở đầu bảng
                    HStack {
                        LinkBadge(link: session.link, compact: session.stats == .compact)
                        Spacer()
                    }
                    .opacity(1 - session.barFade)
                    .allowsHitTesting(false)  // chạm xuyên xuống nút/nền bên dưới
                }
            }
            .padding(.horizontal)
            if panel {
                Color.black.opacity(0.3).ignoresSafeArea()
                    .onTapGesture { show(false) }
                    .transition(.opacity)
                HStack(spacing: 0) {
                    ControlPanel(session: session, close: close, hide: { show(false) },
                                 keyboard: { show(false); session.view?.toggleKeyboard() },
                                 photos: { show(false); pickPhotos = true },
                                 files: { show(false); pickFiles = true },
                                 edit: { show(false); session.editing = true })
                    Spacer(minLength: 0)
                }
                .ignoresSafeArea(edges: .vertical)
                .transition(.move(edge: .leading))
            }
            statusCard.frame(maxHeight: .infinity)
        }
        .fileImporter(isPresented: $pickFiles, allowedContentTypes: [.item], allowsMultipleSelection: true) { r in
            if case .success(let urls) = r { session.send(files: urls.compactMap { try? Files.stage($0) }) }
        }
        .statusBarHidden()
        .gameScreen()
        .sheet(item: Binding(get: { session.editIndex.map(EditTarget.init) }, set: { session.editIndex = $0?.id })) { t in
            KeyEditor(session: session, index: t.id)
        }
        .alert("Chứng chỉ của PC đã đổi", isPresented: .constant(session.state == .certChanged)) {
            Button("Tin chứng chỉ mới") { session.trustNewCert() }
            Button("Đóng", role: .cancel, action: close)
        } message: {
            Text("Thường do PC đổi địa chỉ IP. Chỉ tin nếu bạn chắc đây đúng là PC của mình.")
        }
        .onAppear {
            user = session.host.user ?? ""
            UIApplication.shared.isIdleTimerDisabled = true
            if demo {
                session.state = .streaming
                session.link = LinkStats(fps: 60, ping: 12, mbps: 18.4, size: "1920×1080", enc: "NVIDIA HEVC", network: "Wi‑Fi")
                panel = true
            } else {
                session.connect()
            }
        }
        .onDisappear {
            UIApplication.shared.isIdleTimerDisabled = false
            session.disconnect()
        }
    }

    private func show(_ on: Bool) { withAnimation(.easeOut(duration: 0.22)) { panel = on } }

    /// Nút nổi nhỏ mở bảng điều khiển (như nút Panel của StarDesk), mờ theo cài đặt để không che hình.
    private var handle: some View {
        Button { show(true) } label: {
            Image(systemName: "slider.horizontal.3").font(.system(size: 14, weight: .semibold)).frame(width: 46, height: 28)
        }
        .foregroundStyle(.primary)
        .overlayTrack(14)
        .opacity(1 - session.barFade)
        .accessibilityLabel("Mở bảng điều khiển")
    }

    /// Lúc sửa keymap: kéo nút để dời, chạm nút để đổi; thanh này chỉ còn thêm nút và xong.
    private var editBar: some View {
        HStack(spacing: 4) {
            Button {
                session.keymap.append(KeyButton(label: "A", keys: "A", x: 0.5, y: 0.5))
                session.editIndex = session.keymap.count - 1
            } label: { Label("Thêm nút", systemImage: "plus") }
            Divider().frame(height: 18)
            Button {
                session.editing = false
                session.saveKeymap()
            } label: { Label("Xong", systemImage: "checkmark") }
        }
        .font(.text(13, .semibold))
        .foregroundStyle(.primary)
        .padding(.horizontal, 14)
        .frame(height: 34)
        .overlayTrack(17)
    }

    @ViewBuilder private var statusCard: some View {
        switch session.state {
        case .connecting:
            HStack(spacing: 12) {
                ProgressView()
                Text("Đang kết nối…").font(.text(17)).tracking(-0.37)
            }
            .foregroundStyle(Theme.ink)
            .padding(.horizontal, 28)
            .padding(.vertical, 20)
            .glassCard()
        case .failed(let msg), .ended(let msg):
            VStack(spacing: 20) {
                Text(msg).font(.text(17)).tracking(-0.37).foregroundStyle(Theme.ink).multilineTextAlignment(.center)
                HStack(spacing: 12) {
                    Button("Thử lại") { session.retry() }.primaryStyle()
                    Button("Đóng", action: close).secondaryStyle()
                }
            }
            .padding(28)
            .frame(maxWidth: 380)
            .glassCard()
        default:
            EmptyView()
        }
    }
}

/// Dải gửi/nhận file: tên, tiến độ, lỗi; file nhận xong có nút Chia sẻ (lưu vào Ảnh, mở bằng app khác).
struct TransferBadge: View {
    let transfer: FileTransfer
    var close: () -> Void

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: transfer.sending ? "arrow.up.circle" : "arrow.down.circle")
            Text(transfer.name).lineLimit(1).truncationMode(.middle).frame(maxWidth: 200)
            if let f = transfer.failed {
                Text(f).foregroundStyle(.red).lineLimit(1)
            } else if !transfer.finished {
                ProgressView(value: transfer.progress).frame(width: 80)
            } else if let url = transfer.saved {
                Button("Chia sẻ") { Files.share(url) }
            } else {
                Text("Đã gửi")
            }
            Button(action: close) { Image(systemName: "xmark") }.accessibilityLabel("Đóng")
        }
        .font(.text(12, .medium))
        .foregroundStyle(.primary)
        .padding(.horizontal, 14)
        .padding(.vertical, 7)
        .overlayTrack(16)
    }
}

/// Dải thông số: chấm màu theo ping, khung/s, ping, băng thông, độ phân giải, loại mạng.
struct LinkBadge: View {
    let link: LinkStats
    var compact = false

    var body: some View {
        HStack(spacing: 10) {
            Circle().fill([Color.green, .yellow, .red][link.quality]).frame(width: 8, height: 8)
            Text("\(link.fps) khung/s")
            Text("ping \(link.ping) ms")
            if !compact {
                Text(String(format: "%.1f Mbps", link.mbps))
                if !link.size.isEmpty { Text(link.size) }
                Text(link.network)
            }
        }
        .font(.text(12, .medium).monospacedDigit())
        .foregroundStyle(.primary)
        .padding(.horizontal, 14)
        .padding(.vertical, 7)
        .overlayTrack(16)
        .padding(.bottom, 4)
    }
}

/// Bảng điều khiển trượt từ cạnh trái (kiểu Panel của StarDesk): nút nhanh, chất lượng hình, điều khiển, hiển thị.
/// Tấm kính nổi cách mép, chiếm khoảng 1/3 màn ngang, cuộn được; chạm ra ngoài là đóng.
struct ControlPanel: View {
    @ObservedObject var session: Session
    var close: () -> Void
    var hide: () -> Void
    var keyboard: () -> Void
    var photos: () -> Void
    var files: () -> Void
    var edit: () -> Void
    @State private var confirmDelete = false

    private static let resolutions = [(1280, "720p"), (1920, "1080p"), (2560, "1440p"), (0, "Gốc")]
    private static let rates = [30, 60, 90, 120], bitrates = [5, 10, 20, 30, 50]

    var body: some View {
        GeometryReader { g in
            let shape = RoundedRectangle(cornerRadius: 28, style: .continuous)
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    header
                    actions
                    quality
                    controls
                    display
                    gyro
                    connection
                }
                .padding(14)
            }
            .frame(width: min(360, max(300, g.size.width * 0.42)))
            .clipShape(shape)
            .glass(shape)
            .padding(.leading, 10)  // nổi cách mép; HStack cha đã chừa phần tai thỏ
            .padding(.vertical, 10)
        }
    }

    private var header: some View {
        HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 2) {
                Text(session.host.name).font(.display(17)).foregroundStyle(Theme.ink).lineLimit(1)
                HStack(spacing: 6) {
                    Circle().fill([Color.green, .yellow, .red][session.link.quality]).frame(width: 7, height: 7)
                    Text("\(session.link.fps) khung/s · \(session.link.ping) ms · \(session.link.network)").lineLimit(1)
                }
                .font(.text(11).monospacedDigit()).foregroundStyle(Theme.slate)
            }
            Spacer(minLength: 0)
            Button(action: hide) { Image(systemName: "xmark").font(.system(size: 13, weight: .bold)).frame(width: 30, height: 30) }
                .foregroundStyle(Theme.slate)
                .background(Theme.fill, in: Circle())
                .accessibilityLabel("Đóng bảng")
        }
    }

    private var actions: some View {
        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 6), count: 4), spacing: 6) {
            tile("Bàn phím", "keyboard", action: keyboard)
            tile("Gửi ảnh", "photo.on.rectangle", action: photos)
            tile("Gửi tệp", "folder", action: files)
            tile(session.keysHidden ? "Hiện phím" : "Ẩn phím", session.keysHidden ? "eye" : "eye.slash") { session.keysHidden.toggle() }
            tile("Sửa keymap", "square.grid.3x3", action: edit)
            tile("Thông số: \(session.stats.title)", "speedometer") {
                let all = StatsStyle.allCases
                session.stats = all[(all.firstIndex(of: session.stats)! + 1) % all.count]
            }
            tile("Ngắt", "power", tint: .red, action: close)
        }
    }

    private func tile(_ title: String, _ icon: String, tint: Color = Theme.ink, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 5) {
                Image(systemName: icon).font(.system(size: 17, weight: .medium))
                Text(title).font(.text(11, .medium)).lineLimit(1).minimumScaleFactor(0.75)
            }
            .foregroundStyle(tint)
            .frame(maxWidth: .infinity, minHeight: 58)
            .background(Theme.fill, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        }
        .accessibilityLabel(title)
    }

    private var quality: some View {
        PanelSection("Chất lượng hình") {
            segment("Độ phân giải", Self.resolutions.map(\.1), Self.resolutions.firstIndex { $0.0 == session.quality.width }) {
                session.setQuality(width: Self.resolutions[$0].0)
            }
            segment("Khung hình", Self.rates.map(String.init), Self.rates.firstIndex(of: session.quality.fps)) {
                session.setQuality(fps: Self.rates[$0])
            }
            segment("Bitrate (Mbps)", Self.bitrates.map(String.init), Self.bitrates.firstIndex(of: session.quality.mbps)) {
                session.setQuality(mbps: Self.bitrates[$0])
            }
            segment("Mã hoá", ["H.264", "HEVC"], session.quality.hevc ? 1 : 0) { session.setQuality(hevc: $0 == 1) }
            Note("Đổi là PC phát lại ngay. Hình giật, trễ thì hạ bitrate hoặc độ phân giải.")
        }
    }

    /// Hàng chọn kiểu phân đoạn; nil = PC đang dùng mức không có trong danh sách.
    private func segment(_ title: String, _ items: [String], _ selected: Int?, set: @escaping (Int) -> Void) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(title).font(.text(12)).foregroundStyle(Theme.slate)
            Picker(title, selection: Binding(get: { selected ?? -1 }, set: set)) {
                ForEach(items.indices, id: \.self) { Text(items[$0]).tag($0) }
            }
            .pickerStyle(.segmented)
        }
    }

    private var controls: some View {
        PanelSection("Điều khiển") {
            Menu {
                Picker("Hồ sơ", selection: $session.activeID) {
                    ForEach(session.profiles) { Text($0.name).tag($0.id) }
                }
                Menu("Thêm hồ sơ từ mẫu") {
                    ForEach(KeyProfile.presets) { p in Button(p.name) { session.addProfile(from: p) } }
                }
                Button("Nhân bản hồ sơ này") { session.duplicateProfile() }
                if let preset = session.profilePreset {
                    Button("Khôi phục theo mẫu \"\(preset)\"") { session.resetProfile() }
                }
                if session.profiles.count > 1 {
                    Button("Xoá hồ sơ này", role: .destructive) { confirmDelete = true }
                }
            } label: {
                MenuRow(title: "Hồ sơ keymap", value: session.profileName)
            }
            TextField("Tên hồ sơ", text: $session.profileName).font(.text(13)).textFieldStyle(.roundedBorder)
            Menu {
                Picker("Chạm vào nền", selection: $session.mode) {
                    ForEach(TouchMode.allCases, id: \.self) { Text($0.title).tag($0) }
                }
            } label: {
                MenuRow(title: "Chạm vào nền", value: session.mode.title)
            }
            SliderRow(title: "Độ nhạy trackpad", value: String(format: "%.1f×", session.sensitivity), v: $session.sensitivity, range: 0.5...6)
            SliderRow(title: "Độ nhạy camera", value: String(format: "%.1f×", session.lookSens), v: $session.lookSens, range: 0.3...6)
        }
        .confirmationDialog("Xoá hồ sơ \"\(session.profileName)\"?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Xoá hồ sơ", role: .destructive) { session.deleteProfile() }
        }
    }

    private var display: some View {
        PanelSection("Hiển thị") {
            Toggle("Nút keymap", isOn: Binding(get: { !session.keysHidden }, set: { session.keysHidden = !$0 })).font(.text(13))
            SliderRow(title: "Độ mờ nút", value: "\(Int(session.keyFade * 100))%", v: $session.keyFade, range: 0...0.9)
            segment("Thông số trên màn hình", StatsStyle.allCases.map(\.title), StatsStyle.allCases.firstIndex(of: session.stats)) {
                session.stats = StatsStyle.allCases[$0]
            }
            SliderRow(title: "Độ mờ nút nổi và thông số", value: "\(Int(session.barFade * 100))%", v: $session.barFade, range: 0...0.9)
        }
    }

    private var gyro: some View {
        PanelSection("Ngắm bằng con quay") {
            segment("Con quay", GyroMode.allCases.map(\.title), GyroMode.allCases.firstIndex(of: session.gyro)) {
                session.gyro = GyroMode.allCases[$0]
            }
            SliderRow(title: "Độ nhạy", value: String(format: "%.1f×", session.gyroSens), v: $session.gyroSens, range: 0.2...4)
            Toggle("Đảo trục dọc", isOn: $session.gyroInvertY).font(.text(13))
        }
    }

    private var connection: some View {
        PanelSection("Kết nối") {
            Group {
                LabeledRow("PC", value: session.current)
                LabeledRow("Hình", value: "\(session.link.size) · \(session.link.enc)")
                LabeledRow("Băng thông", value: String(format: "%.1f Mbps", session.link.mbps))
                LabeledRow("Phiên bản app", value: AppUpdate.current)
            }
            .font(.text(12))
            Note("File PC gửi xuống: app Tệp > Trên iPhone > Remote Desk > Nhận từ PC. File gửi lên: Tải về" + "\\" + "Remote Desk trên PC.")
        }
    }
}

/// Một nhóm trong bảng điều khiển: tiêu đề, nền xám trong bo 18 trên tấm kính.
struct PanelSection<Content: View>: View {
    let title: String
    let content: Content
    init(_ title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title).font(.text(13, .semibold)).foregroundStyle(Theme.ink)
            content
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.fill, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }
}

struct SliderRow: View {
    let title: String
    let value: String
    @Binding var v: Double
    let range: ClosedRange<Double>

    var body: some View {
        VStack(spacing: 2) {
            HStack {
                Text(title)
                Spacer()
                Text(value).monospacedDigit().foregroundStyle(Theme.slate)
            }
            .font(.text(12))
            Slider(value: $v, in: range)
        }
    }
}

struct MenuRow: View {
    let title: String
    let value: String

    var body: some View {
        HStack {
            Text(title).foregroundStyle(Theme.ink)
            Spacer()
            Text(value).foregroundStyle(Theme.link).lineLimit(1)
            Image(systemName: "chevron.up.chevron.down").font(.system(size: 10, weight: .semibold)).foregroundStyle(Theme.link)
        }
        .font(.text(13))
    }
}

struct Note: View {
    let text: String
    init(_ text: String) { self.text = text }
    var body: some View { Text(text).font(.text(11)).foregroundStyle(Theme.slate).fixedSize(horizontal: false, vertical: true) }
}

struct KeyEditor: View {
    @ObservedObject var session: Session
    let index: Int
    @Environment(\.dismiss) private var dismiss
    @State private var kind = KeyButton.Kind.key
    @State private var label = ""
    @State private var keys = ""
    @State private var mode = KeyButton.Mode.hold
    @State private var aim = false
    @State private var floating = true
    @State private var sprint = ""
    @State private var size = 1.0
    @State private var cursor = false

    /// Nút theo các ô đang nhập. Giá trị mặc định ghi là nil để file keymap gọn và trang Safari đọc được.
    private var draft: KeyButton? {
        guard session.keymap.indices.contains(index) else { return nil }
        let clean = keys.uppercased().replacingOccurrences(of: " ", with: "")
        var k = session.keymap[index]
        k.kind = kind == .stick ? .stick : nil
        k.keys = clean
        k.label = label.isEmpty ? (clean.isEmpty ? "Chuột" : clean) : label
        k.mode = kind == .key && mode != .hold ? mode : nil
        k.aim = kind == .key && aim ? true : nil
        k.cursor = kind == .key && cursor ? true : nil
        k.floating = kind == .stick && !floating ? false : nil
        k.sprint = kind == .stick && !sprint.isEmpty ? sprint.uppercased() : nil
        k.size = abs(size - 1) < 0.01 ? nil : size
        return k
    }

    var body: some View {
        NavigationView {
            Form {
                Section {
                    Picker("Loại", selection: $kind) {
                        Text("Nút").tag(KeyButton.Kind.key)
                        Text("Cần di chuyển").tag(KeyButton.Kind.stick)
                    }
                    .pickerStyle(.segmented)
                    TextField("Nhãn trên nút", text: $label)
                    TextField(kind == .key ? "Phím, ví dụ CTRL+C" : "Lên,Trái,Xuống,Phải — ví dụ W,A,S,D", text: $keys)
                        .textInputAutocapitalization(.characters)
                        .autocorrectionDisabled()
                } footer: {
                    VStack(alignment: .leading, spacing: 6) {
                        if draft?.isValid == false && !(keys.isEmpty && kind == .key) {
                            Text("PC không hiểu tên phím này.").foregroundStyle(.red)
                        }
                        Text(KeyButton.keyHelp)
                    }
                }

                if kind == .key {
                    Section {
                        Picker("Cách bấm", selection: $mode) {
                            Text("Giữ").tag(KeyButton.Mode.hold)
                            Text("Bật/tắt").tag(KeyButton.Mode.toggle)
                            Text("Liên thanh").tag(KeyButton.Mode.turbo)
                        }
                        .pickerStyle(.segmented)
                        Toggle("Kéo trên nút để xoay camera", isOn: $aim)
                        Toggle("Chạm nền = bấm chuột tại chỗ", isOn: $cursor)
                    } header: { Text("Cách bấm") } footer: {
                        Text("Bật/tắt: chạm một lần là giữ, chạm lại là nhả (ngồi, ngắm, chạy). Liên thanh: giữ là bấm lặp ~15 lần/giây. Kéo để xoay camera: dùng cho nút Bắn, vừa bắn vừa ngắm bằng một ngón. Chạm nền = bấm chuột: lúc nút đang bật, chạm vào màn hình là bấm chuột trái đúng chỗ đó (mở túi đồ, bản đồ; Genshin, Star Rail cần giữ ALT).")
                    }
                } else {
                    Section {
                        Toggle("Cần nổi", isOn: $floating)
                        TextField("Phím khi đẩy hết cỡ, ví dụ SHIFT để chạy", text: $sprint)
                            .textInputAutocapitalization(.characters)
                            .autocorrectionDisabled()
                    } header: { Text("Cần di chuyển") } footer: {
                        Text("Cần nổi: đặt ngón đâu gần cần cũng được, tâm cần dời theo ngón (dễ cho người mới). Tắt đi thì tâm cố định, bấm lệch là đi ngay (cho người đã quen).")
                    }
                }

                Section {
                    VStack(alignment: .leading) {
                        LabeledRow("Kích thước", value: "\(Int(size * 100))%")
                        Slider(value: $size, in: 0.6...2, step: 0.1)
                    }
                    Button("Xoá nút", role: .destructive) {
                        if session.keymap.indices.contains(index) { session.keymap.remove(at: index) }
                        dismiss()
                    }
                }
            }
            .navigationTitle("Sửa nút")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Huỷ") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Xong") { save(); dismiss() }.disabled(draft?.isValid != true)
                }
            }
            .onAppear(perform: load)
        }
        .navigationViewStyle(.stack)
        .sheetStyle()
    }

    private func load() {
        guard session.keymap.indices.contains(index) else { return }
        let k = session.keymap[index]
        kind = k.kind ?? .key
        label = k.label
        keys = k.keys
        mode = k.mode ?? .hold
        aim = k.aim ?? false
        floating = k.floating ?? true
        sprint = k.sprint ?? ""
        size = k.scale
        cursor = k.cursor ?? false
    }

    private func save() {
        if let k = draft { session.keymap[index] = k }
    }
}
