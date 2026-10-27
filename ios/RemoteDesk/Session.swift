import CryptoKit
import Foundation
import Network
import QuartzCore
import Security

/// Khoá thiết bị PC cấp khi đăng nhập đúng tài khoản. Cất trong Keychain, chỉ trên máy này, đọc được sau lần mở khoá
/// máy đầu tiên. Các lần sau app vào thẳng bằng khoá, không lưu và không gửi lại mật khẩu.
enum Keychain {
    private static func query(_ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: "vn.vdt.remotedesk", kSecAttrAccount as String: account]
    }
    static func get(_ account: String) -> String? {
        var q = query(account)
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return String(data: d, encoding: .utf8)
    }
    static func set(_ account: String, _ value: String) {
        SecItemDelete(query(account) as CFDictionary)
        var q = query(account)
        q[kSecValueData as String] = Data(value.utf8)
        q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(q as CFDictionary, nil)
    }
    static func delete(_ account: String) { SecItemDelete(query(account) as CFDictionary) }
}

struct SavedHost: Codable, Identifiable, Hashable {
    var name: String
    var address: String  // "ip:cổng"
    var user: String?  // tên đăng nhập tài khoản trên PC (mật khẩu không lưu)
    var certHash: String?  // SHA-256 chứng chỉ tự ký của PC, ghi nhớ từ lần vào được đầu tiên
    var addresses: [String]?  // mọi địa chỉ PC báo về (LAN, Tailscale) để ở nhà hay ở ngoài đều tự vào được
    var id: String { address }

    enum Kind { case lan, tailscale, internet }

    /// Thứ tự thử khi kết nối. Đang dùng Wi‑Fi thì thử địa chỉ trong nhà trước (nhanh nhất);
    /// đang dùng 4G thì thử IP công khai của nhà trước (đi thẳng tới PC), rồi VPN, cuối cùng LAN.
    func candidates(onWiFi: Bool) -> [String] {
        var all: [String] = []
        for a in (addresses ?? []) + [address] where !all.contains(a) { all.append(a) }
        let of = { (k: Kind) in all.filter { Self.kind($0) == k } }
        return onWiFi ? of(.lan) + of(.tailscale) + of(.internet) : of(.internet) + of(.tailscale) + of(.lan)
    }

    /// LAN/hotspot (10/8, 172.16/12, 192.168/16), Tailscale (100.64/10) hay IP công khai.
    /// Tên miền (nha.duckdns.org) là đường từ Internet; tên máy trơn hoặc .local coi như trong nhà.
    static func kind(_ addr: String) -> Kind {
        let host = String(addr.split(separator: ":").first ?? "").lowercased()
        let p = host.split(separator: ".").compactMap { Int($0) }
        guard p.count == 4 else { return host.contains(".") && !host.hasSuffix(".local") ? .internet : .lan }
        if p[0] == 100 && (64..<128).contains(p[1]) { return .tailscale }
        let lan = p[0] == 10 || p[0] == 127 || (p[0] == 192 && p[1] == 168) || (p[0] == 172 && (16..<32).contains(p[1]))
            || (p[0] == 169 && p[1] == 254)
        return lan ? .lan : .internet
    }
    static func isTailscale(_ addr: String) -> Bool { kind(addr) == .tailscale }

    /// Mã QR trên PC: remotedesk://pair?n=tên&a=ip[:cổng],ip...&c=mã dùng một lần&f=SHA-256 chứng chỉ (hex).
    /// Lưu mọi địa chỉ (LAN trước) và chứng chỉ đã biết trước, nên lần đầu không phải tin chứng chỉ mù.
    static func pair(_ url: URL) -> (host: SavedHost, code: String)? {
        guard url.scheme?.lowercased() == "remotedesk", url.host?.lowercased() == "pair",
              let q = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems else { return nil }
        func v(_ k: String) -> String? { q.first { $0.name == k }?.value.flatMap { $0.isEmpty ? nil : $0 } }
        let addrs = (v("a") ?? "").split(separator: ",").map { normalize(String($0)) }
        guard let first = addrs.first, let code = v("c") else { return nil }
        return (SavedHost(name: v("n") ?? first, address: first, certHash: v("f")?.lowercased(), addresses: addrs), code)
    }

    /// "192.168.1.12" -> "192.168.1.12:8443"
    static func normalize(_ s: String) -> String {
        let t = s.trimmingCharacters(in: .whitespaces)
        return t.contains(":") ? t : t + ":8443"
    }
}

final class HostStore: ObservableObject {
    @Published var hosts: [SavedHost] { didSet { UserDefaults.standard.set(try? JSONEncoder().encode(hosts), forKey: "hosts") } }
    init() {
        hosts = (try? JSONDecoder().decode([SavedHost].self, from: UserDefaults.standard.data(forKey: "hosts") ?? Data())) ?? []
    }
    func upsert(_ h: SavedHost) {
        if let i = hosts.firstIndex(where: { $0.address == h.address }) { hosts[i] = h } else { hosts.append(h) }
    }
}

/// Tìm PC đang chạy remote_host.exe trong mạng LAN (mDNS _remotedesk._tcp).
final class Discovery: ObservableObject {
    @Published var found: [NWBrowser.Result] = []
    private var browser: NWBrowser?

    func start() {
        guard browser == nil else { return }
        let b = NWBrowser(for: .bonjour(type: "_remotedesk._tcp", domain: nil), using: .tcp)
        b.browseResultsChangedHandler = { [weak self] results, _ in self?.found = Array(results) }
        b.start(queue: .main)
        browser = b
    }

    static func name(_ r: NWBrowser.Result) -> String {
        if case .service(let name, _, _, _) = r.endpoint { return name }
        return "\(r.endpoint)"
    }

    /// Đổi dịch vụ mDNS ra "ip:cổng" bằng cách mở thử một kết nối TCP.
    static func resolve(_ endpoint: NWEndpoint, done: @escaping (String?) -> Void) {
        let c = NWConnection(to: endpoint, using: .tcp)
        var finished = false
        let finish: (String?) -> Void = { s in
            guard !finished else { return }
            finished = true
            c.cancel()
            done(s)
        }
        c.stateUpdateHandler = { state in
            switch state {
            case .ready:
                if case .hostPort(let host, let port)? = c.currentPath?.remoteEndpoint {
                    var h = "\(host)"
                    if let i = h.firstIndex(of: "%") { h = String(h[..<i]) }  // bỏ "%en0"
                    if case .ipv6 = host { h = "[\(h)]" }
                    finish("\(h):\(port.rawValue)")
                } else { finish(nil) }
            case .failed, .cancelled: finish(nil)
            default: break
            }
        }
        c.start(queue: .main)
        DispatchQueue.main.asyncAfter(deadline: .now() + 5) { finish(nil) }
    }
}

/// Ngón chạm vào nền màn hình (không trúng nút) làm gì.
enum TouchMode: String, Codable, CaseIterable {
    case trackpad, direct, directRight, look
    var title: String {
        switch self {
        case .trackpad: return "Trackpad"
        case .direct: return "Chạm = chuột trái"
        case .directRight: return "Chạm = chuột phải (MOBA)"
        case .look: return "Vuốt xoay camera (FPS)"
        }
    }
}

/// Con quay (gyro) xoay camera: tắt, luôn bật, hoặc chỉ khi giữ/bật nút có "kéo để ngắm".
enum GyroMode: String, CaseIterable {
    case off, always, aim
    var title: String {
        switch self {
        case .off: return "Tắt"
        case .always: return "Luôn bật"
        case .aim: return "Khi ngắm"
        }
    }
}

/// Dải thông số trên màn hình: ẩn, gọn (chấm màu, khung/s, ping) hoặc đủ.
enum StatsStyle: String, CaseIterable {
    case off, compact, full
    var title: String {
        switch self {
        case .off: return "Ẩn"
        case .compact: return "Gọn"
        case .full: return "Đủ"
        }
    }
}

/// Thông số hiện trên màn hình điều khiển.
struct LinkStats: Equatable {
    var fps = 0, ping = 0
    var mbps = 0.0
    var size = "", enc = "", network = ""
    /// 0 tốt, 1 tạm, 2 kém: theo ping (fps thấp khi màn hình PC đứng yên là bình thường)
    var quality: Int { ping < 40 ? 0 : ping < 100 ? 1 : 2 }
}

enum StreamState: Equatable {
    case connecting, streaming, badLogin
    case certChanged
    case ended(String)  // PC đóng phiên, không tự nối lại
    case failed(String)
}

/// Một phiên điều khiển: WebSocket (wss) tới remote_host.exe, nhận hình, gửi chuột/phím/keymap.
final class Session: NSObject, ObservableObject, Identifiable, URLSessionWebSocketDelegate {
    @Published var state = StreamState.connecting
    @Published var link = LinkStats()
    @Published var editing = false
    @Published var editIndex: Int?
    // Hồ sơ keymap theo game; hồ sơ đang dùng quyết định nút và kiểu chạm. Lưu trên iPhone.
    @Published var profiles: [KeyProfile] { didSet { UserDefaults.standard.set(try? JSONEncoder().encode(profiles), forKey: "profiles") } }
    @Published var activeID: UUID {
        didSet {
            UserDefaults.standard.set(activeID.uuidString, forKey: "activeProfile")
            saveKeymap()
        }
    }
    private var activeIndex: Int { profiles.firstIndex { $0.id == activeID } ?? 0 }
    var keymap: [KeyButton] { get { profiles[activeIndex].keys } set { profiles[activeIndex].keys = newValue } }
    var mode: TouchMode { get { profiles[activeIndex].touch } set { profiles[activeIndex].touch = newValue } }
    var profileName: String { get { profiles[activeIndex].name } set { profiles[activeIndex].name = newValue } }
    var profilePreset: String? { profiles[activeIndex].preset }
    // Cài đặt người dùng, lưu trong UserDefaults
    @Published var keysHidden = Session.pref("keysHidden", false) { didSet { UserDefaults.standard.set(keysHidden, forKey: "keysHidden") } }
    @Published var keyFade = Session.pref("keyFade", 0.15) { didSet { UserDefaults.standard.set(keyFade, forKey: "keyFade") } }  // độ mờ nút 0...0.9
    @Published var sensitivity = Session.pref("sensitivity", 2.2) { didSet { UserDefaults.standard.set(sensitivity, forKey: "sensitivity") } }
    @Published var stats = StatsStyle(rawValue: Session.pref("stats", "full")) ?? .full { didSet { UserDefaults.standard.set(stats.rawValue, forKey: "stats") } }
    @Published var barFade = Session.pref("barFade", 0.0) { didSet { UserDefaults.standard.set(barFade, forKey: "barFade") } }  // độ mờ thanh công cụ + thông số
    @Published var barCollapsed = Session.pref("barCollapsed", false) { didSet { UserDefaults.standard.set(barCollapsed, forKey: "barCollapsed") } }
    @Published var lookSens = Session.pref("lookSens", 1.5) { didSet { UserDefaults.standard.set(lookSens, forKey: "lookSens") } }
    @Published var gyro = GyroMode(rawValue: Session.pref("gyro", "off")) ?? .off { didSet { UserDefaults.standard.set(gyro.rawValue, forKey: "gyro") } }
    @Published var gyroSens = Session.pref("gyroSens", 1.0) { didSet { UserDefaults.standard.set(gyroSens, forKey: "gyroSens") } }
    @Published var gyroInvertY = Session.pref("gyroInvertY", false) { didSet { UserDefaults.standard.set(gyroInvertY, forKey: "gyroInvertY") } }

    private static func pref<T>(_ key: String, _ def: T) -> T { UserDefaults.standard.object(forKey: key) as? T ?? def }

    private(set) var host: SavedHost
    private var password: String?  // chỉ giữ trong bộ nhớ tới khi PC cấp khoá thiết bị
    private var pairCode: String?  // mã dùng một lần trong QR trên PC, thay cho mật khẩu
    var onHostUpdate: ((SavedHost) -> Void)?
    let renderer = VideoRenderer()
    weak var view: StreamUIView?

    private var urlSession: URLSession!
    private var task: URLSessionWebSocketTask?
    private var timer: Timer?
    private var closedByUser = false
    private let lock = NSLock()
    private var pinnedHash: String?, seenHash: String?, trustNext = false, authMsg = ""  // dùng trên hàng đợi delegate, có khoá
    private var lastCandidate = true  // có khoá: lần thử cuối mới báo "chứng chỉ đã đổi", lần trước đó thì chuyển địa chỉ
    private var candidates: [String] = [], attempt = 0
    private(set) var current = ""  // địa chỉ đang dùng
    private let path = NWPathMonitor()
    private var onWiFi = true

    init(host: SavedHost, password: String? = nil, pairCode: String? = nil) {
        self.host = host
        self.password = password
        self.pairCode = pairCode
        let d = UserDefaults.standard
        var ps = (try? JSONDecoder().decode([KeyProfile].self, from: d.data(forKey: "profiles") ?? Data())) ?? []
        if ps.isEmpty {
            ps = KeyProfile.presets
            if let old = try? JSONDecoder().decode([KeyButton].self, from: d.data(forKey: "keymap") ?? Data()) { ps[0].keys = old }  // keymap bản trước
        }
        profiles = ps
        let saved = UUID(uuidString: d.string(forKey: "activeProfile") ?? "")
        activeID = ps.first { $0.id == saved }?.id ?? ps[0].id
        pinnedHash = host.certHash
        super.init()
        let q = OperationQueue()
        q.maxConcurrentOperationCount = 1  // nhận hình theo đúng thứ tự
        urlSession = URLSession(configuration: .ephemeral, delegate: self, delegateQueue: q)
        renderer.onNeedKey = { [weak self] in DispatchQueue.main.async { self?.send(["t": "key"]) } }
        path.pathUpdateHandler = { [weak self] p in
            DispatchQueue.main.async { self?.onWiFi = p.usesInterfaceType(.wifi) || p.usesInterfaceType(.wiredEthernet) }
        }
        path.start(queue: .global(qos: .utility))
    }

    deinit { path.cancel() }

    // MARK: kết nối

    func connect() {
        closedByUser = false
        state = .connecting
        candidates = host.candidates(onWiFi: onWiFi)
        attempt = 0
        open()
    }

    /// Mở kết nối tới địa chỉ thứ `attempt`; thất bại thì closed() chuyển sang địa chỉ kế tiếp.
    private func open() {
        task?.cancel(with: .goingAway, reason: nil)
        current = candidates[attempt]
        let last = attempt + 1 >= candidates.count
        // Tin đăng nhập: khoá thiết bị nếu đã có, tài khoản nếu vừa nhập mật khẩu, và báo giải mã được HEVC
        var auth: [String: Any] = ["hevc": true]
        if let tok = Keychain.get(host.address) { auth["token"] = tok }
        if let password, !password.isEmpty {
            auth["user"] = host.user ?? ""
            auth["pass"] = password
        }
        if let pairCode { auth["pair"] = pairCode }
        let msg = (try? JSONSerialization.data(withJSONObject: auth)).flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
        lock.withLock {
            authMsg = msg
            lastCandidate = last
        }
        guard let url = URL(string: "wss://\(current)/ws") else { state = .failed("Địa chỉ không hợp lệ: \(current)"); return }
        var req = URLRequest(url: url)
        req.timeoutInterval = last ? 10 : 4  // địa chỉ LAN khi đang ở ngoài thường không trả lời: đừng chờ lâu
        let t = urlSession.webSocketTask(with: req)
        t.maximumMessageSize = 16 << 20  // khung khoá 1080p có thể vài trăm KB
        task = t
        t.resume()
        receive(t)
        if timer == nil {
            timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.tick() }
        }
    }

    func disconnect() {
        closedByUser = true
        timer?.invalidate()
        timer = nil
        task?.cancel(with: .normalClosure, reason: nil)
        task = nil
    }

    func retry(user: String? = nil, password: String? = nil) {
        if let user { host.user = user }
        if let password { self.password = password }
        connect()
    }

    /// Người dùng đồng ý tin chứng chỉ mới của PC (PC đổi IP thì tự tạo chứng chỉ khác).
    func trustNewCert() {
        lock.withLock { trustNext = true }
        connect()
    }

    func send(_ m: [String: Any]) {
        guard let d = try? JSONSerialization.data(withJSONObject: m), let s = String(data: d, encoding: .utf8) else { return }
        send(text: s)
    }

    func send(text: String, on t: URLSessionWebSocketTask? = nil) {
        (t ?? task)?.send(.string(text)) { _ in }
    }

    func tapKey(_ keys: String) {
        send(["t": "k", "keys": keys, "d": 1])
        send(["t": "k", "keys": keys, "d": 0])
    }

    /// Gửi keymap đang dùng lên PC (trang Safari dùng chung). Trên iPhone đã tự lưu theo hồ sơ.
    func saveKeymap() {
        if let m = KeyButton.saveMessage(keymap) { send(text: m) }
    }

    func duplicateProfile() {
        var p = profiles[activeIndex]
        p.id = UUID()
        p.name += " (bản sao)"
        profiles.append(p)
        activeID = p.id
    }

    func addProfile(from preset: KeyProfile) {
        var p = preset
        p.id = UUID()
        profiles.append(p)
        activeID = p.id
    }

    func resetProfile() {
        guard let p = KeyProfile.presets.first(where: { $0.name == profilePreset }) else { return }
        profiles[activeIndex].keys = p.keys
        profiles[activeIndex].touch = p.touch
        saveKeymap()
    }

    func deleteProfile() {
        guard profiles.count > 1 else { return }
        profiles.remove(at: activeIndex)
        activeID = profiles[0].id
    }

    private func tick() {
        guard state == .streaming else { return }
        send(["t": "ping", "c": CACurrentMediaTime() * 1000])
        let (frames, bytes) = renderer.takeCounters()
        link.fps = frames
        link.mbps = Double(bytes) * 8 / 1_000_000
        // shortcut: đoán loại mạng theo dải địa chỉ đang dùng (Tailscale 100.64/10, hotspot 172.20.10.x)
        switch SavedHost.kind(current) {
        case .internet: link.network = "Internet, thẳng tới PC"
        case .tailscale: link.network = "Tailscale"
        case .lan: link.network = current.hasPrefix("172.20.10.") ? "Hotspot / cáp USB" : "Wi‑Fi / LAN"
        }
    }

    private func receive(_ t: URLSessionWebSocketTask) {
        t.receive { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(.data(let d)): self.renderer.enqueue(d)
            case .success(.string(let s)): self.handle(s)
            case .failure(let e):
                DispatchQueue.main.async { self.closed(t, e) }
                return
            @unknown default: break
            }
            self.receive(t)
        }
    }

    private func handle(_ s: String) {
        guard let d = s.data(using: .utf8), let m = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
              let type = m["t"] as? String else { return }
        let c = m["c"] as? Double, e = m["enc"] as? String, sz = m["size"] as? [Int], addrs = m["addrs"] as? [String]
        if type == "hello", let tok = m["token"] as? String, tok.count == 64 { Keychain.set(host.address, tok) }  // vừa đăng nhập
        if type == "badlogin" { Keychain.delete(host.address) }  // khoá cũ bị thu hồi (PC đổi tài khoản / đăng xuất hết)
        DispatchQueue.main.async { [self] in
            switch type {
            case "hello":
                state = .streaming
                lock.withLock {
                    if let h = seenHash { host.certHash = h; pinnedHash = h }
                    trustNext = false
                }
                if let addrs, !addrs.isEmpty { host.addresses = addrs }
                password = nil  // đã có khoá thiết bị: không giữ mật khẩu, mã QR nữa
                pairCode = nil
                onHostUpdate?(host)
            case "badlogin":
                state = .badLogin
            case "badpair":
                state = .ended("Mã QR đã dùng rồi hoặc hết hạn. Bấm \"Ghép bằng mã QR\" trên PC rồi quét lại.")
            case "noaccount":
                state = .ended("PC chưa có tài khoản. Mở Remote Desk trên PC và bấm \"Tạo tài khoản\".")
            case "locked":
                state = .ended("Nhập sai quá nhiều lần nên PC tạm khoá địa chỉ này. Thử lại sau vài phút.")
            case "pong":
                if let c { link.ping = Int(CACurrentMediaTime() * 1000 - c) }
                link.enc = e ?? ""
                if let sz, sz.count == 2 { link.size = "\(sz[0])×\(sz[1])" }
            default: break
            }
        }
    }

    private func closed(_ t: URLSessionWebSocketTask, _ error: Error) {
        guard t === task, !closedByUser else { return }
        switch t.closeCode.rawValue {
        case 4000: state = .ended("Một thiết bị khác đã kết nối tới PC.")
        case 4001: state = .ended("PC đã ngắt kết nối.")
        default:
            switch state {
            case .badLogin, .certChanged, .ended: break
            case .streaming:  // rớt mạng giữa chừng (vd. đổi Wi‑Fi sang 4G): tự nối lại, thử lại mọi địa chỉ
                state = .connecting
                DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
                    if self?.closedByUser == false { self?.connect() }
                }
            default:
                if attempt + 1 < candidates.count {  // địa chỉ này không vào được: thử địa chỉ kế (LAN ↔ Tailscale)
                    attempt += 1
                    open()
                } else {
                    state = .failed(error.localizedDescription)
                }
            }
        }
    }

    // MARK: URLSessionWebSocketDelegate

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didOpenWithProtocol protocol: String?) {
        send(text: lock.withLock { authMsg }, on: webSocketTask)
    }

    /// PC dùng chứng chỉ tự ký: tin theo kiểu "lần đầu" (như SSH), sau đó chỉ nhận đúng chứng chỉ đã nhớ.
    func urlSession(_ session: URLSession, didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        guard challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
              let trust = challenge.protectionSpace.serverTrust,
              let leaf = (SecTrustCopyCertificateChain(trust) as? [SecCertificate])?.first
        else { return completionHandler(.performDefaultHandling, nil) }
        let hash = SHA256.hash(data: SecCertificateCopyData(leaf) as Data).map { String(format: "%02x", $0) }.joined()
        let (ok, last): (Bool, Bool) = lock.withLock {
            guard pinnedHash == nil || pinnedHash == hash || trustNext else { return (false, lastCandidate) }
            seenHash = hash
            return (true, lastCandidate)
        }
        if ok {
            completionHandler(.useCredential, URLCredential(trust: trust))
        } else {
            // Ở mạng lạ, địa chỉ LAN cũ có thể là máy khác: chỉ hỏi người dùng khi đã hết địa chỉ để thử
            if last { DispatchQueue.main.async { self.state = .certChanged } }
            completionHandler(.cancelAuthenticationChallenge, nil)
        }
    }
}
