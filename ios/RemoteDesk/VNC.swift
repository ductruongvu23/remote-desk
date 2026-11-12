import AVFoundation
import CommonCrypto
import ImageIO
import Network
import SwiftUI
import UIKit
import zlib

/// Điều khiển iPhone/iPad đã jailbreak có TrollVNC (LibVNCServer, cổng 5901, quảng bá _rfb._tcp qua Bonjour) bằng RFB 3.8.
/// Xin mã hoá Tight + JPEG như bản PC (remote_host.cpp): đo trên iPhone 11 Pro Max qua Wi‑Fi ~25 khung/s, chỉ zlib thì
/// ~2,6 khung/s. Chạm = chuột trái (TrollVNC đổi thành chạm), nút Home = chuột phải, nút nguồn = chuột giữa.

enum VNCError: Error {
    case lost, bad, needPassword, badPassword, message(String)
    var text: String {
        switch self {
        case .lost: return "Mất kết nối với iPhone."
        case .bad: return "iPhone gửi dữ liệu hình không hợp lệ."
        case .needPassword: return "TrollVNC trên iPhone này có đặt mật khẩu."
        case .badPassword: return "Sai mật khẩu VNC."
        case .message(let m): return m
        }
    }
}

struct VNCTarget: Codable, Identifiable, Hashable {
    var name: String
    var address: String  // "ip:cổng"
    var id: String { address }

    /// "192.168.0.101" -> "192.168.0.101:5901"
    static func normalize(_ s: String) -> String {
        let t = s.trimmingCharacters(in: .whitespaces)
        return t.contains(":") ? t : t + ":5901"
    }

    /// Địa chỉ của chính máy này: TrollVNC trên máy mình mà điều khiển từ chính nó thì mỗi lần chạm sinh ra chạm mới, máy bị
    /// chạm liên tục tới khi tắt app.
    static func isSelf(_ address: String) -> Bool {
        let host = String(address.split(separator: ":").first ?? "").lowercased()
        var own: Set<String> = ["127.0.0.1", "localhost"]
        var list: UnsafeMutablePointer<ifaddrs>?
        if getifaddrs(&list) == 0 {
            var p = list
            while let i = p {
                if let sa = i.pointee.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) {
                    var b = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                    if getnameinfo(sa, socklen_t(sa.pointee.sa_len), &b, socklen_t(b.count), nil, 0, NI_NUMERICHOST) == 0 {
                        own.insert(String(cString: b))
                    }
                }
                p = i.pointee.ifa_next
            }
            freeifaddrs(list)
        }
        return own.contains(host)
    }
}

/// iPhone (TrollVNC) đã vào được, lưu trên máy; mật khẩu VNC (nếu có) để trong Keychain.
final class VNCStore: ObservableObject {
    @Published var targets: [VNCTarget] {
        didSet { UserDefaults.standard.set(try? JSONEncoder().encode(targets), forKey: "vncTargets") }
    }
    init() {
        targets = (try? JSONDecoder().decode([VNCTarget].self, from: UserDefaults.standard.data(forKey: "vncTargets") ?? Data())) ?? []
    }
    func upsert(_ t: VNCTarget) {
        if let i = targets.firstIndex(where: { $0.address == t.address }) { targets[i] = t } else { targets.append(t) }
    }
}

typealias VNCRead = (Int) throws -> [UInt8]

/// Giải mã Tight, giống bản C++ trong remote_host.cpp (đã đối chiếu từng điểm với luồng thật của TrollVNC).
final class TightDecoder {
    // z_stream phải nằm yên một chỗ: zlib tự kiểm tra địa chỉ của nó giữa các lần gọi
    private let zs: [UnsafeMutablePointer<z_stream>] = (0..<4).map { _ in
        let p = UnsafeMutablePointer<z_stream>.allocate(capacity: 1)
        p.initialize(to: z_stream())
        return p
    }
    private var live = [false, false, false, false]

    deinit {
        for i in 0..<4 {
            if live[i] { inflateEnd(zs[i]) }
            zs[i].deallocate()
        }
    }

    /// Độ dài "compact": 1–3 byte, mỗi byte 7 bit (byte thứ ba đủ 8 bit).
    static func length(_ read: VNCRead) throws -> Int {
        var n = 0
        for i in 0..<3 {
            let b = Int(try read(1)[0])
            n |= (i < 2 ? b & 0x7F : b) << (7 * i)
            if i < 2 && b & 0x80 == 0 { break }
        }
        return n
    }

    private func unzip(_ id: Int, _ input: [UInt8], _ size: Int) throws -> [UInt8] {
        let z = zs[id]
        if !live[id] {
            z.pointee = z_stream()
            guard inflateInit_(z, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else { throw VNCError.bad }
            live[id] = true
        }
        var out = [UInt8](repeating: 0, count: size), inp = input, sink: UInt8 = 0, got = 0
        try inp.withUnsafeMutableBufferPointer { i in
            z.pointee.next_in = i.baseAddress
            z.pointee.avail_in = uInt(i.count)
            while z.pointee.avail_in > 0 {  // sau dữ liệu còn dấu đồng bộ (00 00 FF FF): đọc hết nhưng không được ra thêm byte
                let full = got == size
                let (r, made): (Int32, Int) = out.withUnsafeMutableBufferPointer { o in
                    withUnsafeMutablePointer(to: &sink) { s in
                        z.pointee.next_out = full ? s : o.baseAddress! + got
                        z.pointee.avail_out = full ? 1 : uInt(size - got)
                        let before = z.pointee.avail_out
                        let r = inflate(z, Z_SYNC_FLUSH)
                        return (r, Int(before - z.pointee.avail_out))
                    }
                }
                if full && made > 0 { throw VNCError.bad }
                got += made
                if r == Z_STREAM_END || r == Z_BUF_ERROR { break }
                if r != Z_OK { throw VNCError.bad }
            }
        }
        guard got == size else { throw VNCError.bad }
        return out
    }

    static func jpeg(_ data: [UInt8], _ w: Int, _ h: Int) -> [UInt32]? {
        guard let src = CGImageSourceCreateWithData(Data(data) as CFData, nil),
              let img = CGImageSourceCreateImageAtIndex(src, 0, nil), img.width == w, img.height == h else { return nil }
        var out = [UInt32](repeating: 0, count: w * h)
        let ok = out.withUnsafeMutableBytes { b -> Bool in
            guard let ctx = CGContext(data: b.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                      space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: VNCClient.bitmapInfo) else { return false }
            ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        return ok ? out : nil
    }

    /// Một ô w×h ra điểm ảnh 32 bit little-endian XRGB (trong bộ nhớ là B, G, R, X). Điểm Tight 3 byte R, G, B.
    func rect(_ read: VNCRead, w: Int, h: Int) throws -> [UInt32] {
        func px(_ p: [UInt8], _ i: Int) -> UInt32 { 0xFF00_0000 | UInt32(p[i]) << 16 | UInt32(p[i + 1]) << 8 | UInt32(p[i + 2]) }
        var cc = Int(try read(1)[0])
        for i in 0..<4 where cc >> i & 1 == 1 && live[i] {
            inflateEnd(zs[i])
            live[i] = false
        }
        cc >>= 4
        if cc == 8 { return [UInt32](repeating: px(try read(3), 0), count: w * h) }  // cả ô một màu
        if cc == 9 {
            let n = try Self.length(read)
            guard n <= 32 << 20, let out = Self.jpeg(try read(n), w, h) else { throw VNCError.bad }
            return out
        }
        guard cc < 9 else { throw VNCError.bad }  // 0x0A là PNG của TightPNG: không xin
        let filter = cc & 4 != 0 ? Int(try read(1)[0]) : 0
        var pal: [UInt32] = []
        var bits = 24
        if filter == 1 {
            let k = Int(try read(1)[0]) + 1
            let p = try read(k * 3)
            pal = (0..<k).map { px(p, $0 * 3) }
            bits = k == 2 ? 1 : 8
        } else if filter > 2 {
            throw VNCError.bad
        }
        let row = bits == 24 ? w * 3 : (w * bits + 7) / 8, size = row * h
        let d: [UInt8]
        if size < 12 {  // dưới 12 byte thì gửi thẳng, không nén
            d = try read(size)
        } else {
            let n = try Self.length(read)
            guard n <= 32 << 20 else { throw VNCError.bad }
            d = try unzip(cc & 3, try read(n), size)
        }
        var out = [UInt32](repeating: 0, count: w * h)
        switch filter {
        case 1:
            for y in 0..<h {
                for x in 0..<w {
                    let i = bits == 1 ? Int(d[y * row + x / 8] >> (7 - x % 8) & 1) : Int(d[y * row + x])
                    guard i < pal.count else { throw VNCError.bad }
                    out[y * w + x] = pal[i]
                }
            }
        case 2:  // gradient: mỗi điểm = dự đoán (trái + trên − trên trái, kẹp 0...255) + phần dư
            var prev = [UInt8](repeating: 0, count: w * 3), cur = prev
            for y in 0..<h {
                for x in 0..<w {
                    for c in 0..<3 {
                        let est = x == 0 ? Int(prev[c])
                            : min(max(Int(prev[x * 3 + c]) + Int(cur[(x - 1) * 3 + c]) - Int(prev[(x - 1) * 3 + c]), 0), 255)
                        cur[x * 3 + c] = UInt8(truncatingIfNeeded: est + Int(d[y * row + x * 3 + c]))
                    }
                }
                for x in 0..<w { out[y * w + x] = px(cur, x * 3) }
                swap(&prev, &cur)
            }
        default:
            for i in 0..<w * h { out[i] = px(d, i * 3) }
        }
        return out
    }
}

/// Một phiên điều khiển iPhone qua TrollVNC: luồng riêng đọc RFB (đọc chặn), hình đẩy lên lớp của VNCCanvas.
final class VNCClient: ObservableObject, Identifiable {
    enum State: Equatable { case connecting, live, failed(String, askPassword: Bool) }
    static let bitmapInfo = CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue

    @Published var state = State.connecting
    let target: VNCTarget
    var password: String?
    var onConnected: ((VNCTarget) -> Void)?
    weak var view: VNCCanvas?
    private(set) var size = CGSize.zero  // khung iPhone, đọc trên luồng chính
    private var conn: NWConnection?
    private var stopped = false
    private var rbuf: [UInt8] = [], rpos = 0
    private let lock = NSLock()
    private var latest: CGImage?, scheduled = false
    private var lastX = 0, lastY = 0

    init(target: VNCTarget, password: String?) {
        self.target = target
        self.password = password
    }

    func connect() {
        stopped = false
        state = .connecting
        let parts = target.address.split(separator: ":")
        guard let host = parts.first, let pn = UInt16(parts.count > 1 ? String(parts[1]) : "5901"),
              let port = NWEndpoint.Port(rawValue: pn) else {
            state = .failed("Địa chỉ không hợp lệ: \(target.address)", askPassword: false)
            return
        }
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let c = NWConnection(host: NWEndpoint.Host(String(host)), port: port, using: NWParameters(tls: nil, tcp: tcp))
        conn = c
        rbuf = []
        rpos = 0
        Thread { [self] in run(c) }.start()
    }

    func disconnect() {
        stopped = true
        conn?.cancel()
    }

    private func run(_ c: NWConnection) {
        var err: VNCError?
        do { try session(c) } catch let e as VNCError { err = e } catch { err = .lost }
        c.cancel()
        DispatchQueue.main.async { [self] in
            guard !stopped, let err else { return }
            if case .badPassword = err { Keychain.delete("vnc:" + target.address) }
            let ask: Bool
            switch err {
            case .needPassword, .badPassword: ask = true
            default: ask = false
            }
            state = .failed(err.text, askPassword: ask)
        }
    }

    // MARK: đọc / ghi

    private func receive(_ c: NWConnection) throws -> [UInt8] {
        var got: Data?
        let sem = DispatchSemaphore(value: 0)
        c.receive(minimumIncompleteLength: 1, maximumLength: 1 << 18) { d, _, _, _ in
            got = d
            sem.signal()
        }
        sem.wait()
        guard let got, !got.isEmpty else { throw VNCError.lost }
        return [UInt8](got)
    }

    private func reader(_ c: NWConnection) -> VNCRead {
        return { [self] n in
            while rbuf.count - rpos < n {
                if rpos > 0 {
                    rbuf.removeFirst(rpos)
                    rpos = 0
                }
                rbuf += try receive(c)
            }
            defer { rpos += n }
            return Array(rbuf[rpos..<rpos + n])
        }
    }

    private func send(_ b: [UInt8]) { conn?.send(content: Data(b), completion: .contentProcessed { _ in }) }

    func pointer(_ mask: Int, _ x: Int, _ y: Int) {
        lastX = x
        lastY = y
        send([5, UInt8(mask), UInt8(x >> 8), UInt8(x & 255), UInt8(y >> 8), UInt8(y & 255)])
    }

    /// Bấm nút cứng của iPhone: 4 = Home, 2 = nút nguồn (TrollVNC đọc chuột phải / chuột giữa).
    func button(_ mask: Int) {
        pointer(mask, lastX, lastY)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [self] in pointer(0, lastX, lastY) }
    }

    func key(_ k: UInt32, _ down: Bool) {
        send([4, down ? 1 : 0, 0, 0, UInt8(k >> 24), UInt8(k >> 16 & 255), UInt8(k >> 8 & 255), UInt8(k & 255)])
    }

    /// Ký tự gõ trên bàn phím -> (keysym, cần Shift). TrollVNC đổi keysym ra phím HID gốc (':' thành phím ';') mà không tự
    /// thêm Shift, nên chữ hoa và ký tự trên của phím phải kèm Shift. Chữ có dấu thì để bàn phím của iPhone kia tự gõ Telex.
    static func keys(for ch: Character) -> (UInt32, Bool)? {
        if ch == "\n" { return (0xFF0D, false) }
        if ch == "\t" { return (0xFF09, false) }
        guard let a = ch.asciiValue, a >= 0x20, a < 0x7F else { return nil }
        if let base = shifted[ch] { return (UInt32(base.asciiValue!), true) }
        if ch.isUppercase { return (UInt32(a) + 32, true) }
        return (UInt32(a), false)
    }
    private static let shifted: [Character: Character] = [
        ":": ";", "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
        "_": "-", "+": "=", "{": "[", "}": "]", "|": "\\", "\"": "'", "<": ",", ">": ".", "?": "/", "~": "`",
    ]

    func type(_ text: String) {
        for ch in text {
            guard let (k, shift) = Self.keys(for: ch) else { continue }
            if shift { key(0xFFE1, true) }
            key(k, true)
            key(k, false)
            if shift { key(0xFFE1, false) }
        }
    }

    /// Xác thực VNC cổ điển: mã DES 16 byte thử thách bằng mật khẩu (8 ký tự đầu, mỗi byte đảo thứ tự bit).
    static func auth(_ pass: String, _ challenge: [UInt8]) -> [UInt8]? {
        var key = [UInt8](repeating: 0, count: 8)
        for (i, c) in pass.utf8.prefix(8).enumerated() {
            for b in 0..<8 where c >> b & 1 == 1 { key[i] |= 1 << (7 - b) }
        }
        var out = [UInt8](repeating: 0, count: 16), moved = 0
        let st = CCCrypt(CCOperation(kCCEncrypt), CCAlgorithm(kCCAlgorithmDES), CCOptions(kCCOptionECBMode), key, kCCKeySizeDES,
                         nil, challenge, 16, &out, 16, &moved)
        return Int(st) == kCCSuccess && moved == 16 ? out : nil
    }

    // MARK: phiên RFB

    private func session(_ c: NWConnection) throws {
        let ready = DispatchSemaphore(value: 0)
        var ok = false
        c.stateUpdateHandler = { st in
            switch st {
            case .ready: ok = true; ready.signal()
            case .failed, .waiting, .cancelled: ready.signal()
            default: break
            }
        }
        c.start(queue: DispatchQueue(label: "vnc"))
        if ready.wait(timeout: .now() + 6) == .timedOut || !ok {
            throw VNCError.message("Không kết nối được iPhone. Kiểm tra TrollVNC đã bật và hai máy cùng mạng Wi‑Fi.")
        }
        let read = reader(c)
        func u16(_ b: [UInt8], _ i: Int) -> Int { Int(b[i]) << 8 | Int(b[i + 1]) }
        func u32(_ b: [UInt8], _ i: Int) -> Int { u16(b, i) << 16 | u16(b, i + 2) }
        let ver = try read(12)
        guard ver.starts(with: Array("RFB 003.".utf8)) else { throw VNCError.message("Địa chỉ này không phải TrollVNC (VNC).") }
        guard let minor = Int(String(decoding: ver[8..<11], as: UTF8.self)), minor >= 8 else {
            throw VNCError.message("Máy chủ VNC quá cũ (cần RFB 3.8).")
        }
        send(Array("RFB 003.008\n".utf8))
        let n = Int(try read(1)[0])
        guard n > 0 else { throw VNCError.message("iPhone từ chối kết nối.") }
        let types = try read(n)
        let sec: UInt8 = types.contains(1) ? 1 : types.contains(2) ? 2 : 0  // 1 = không mật khẩu, 2 = mật khẩu VNC
        guard sec != 0 else { throw VNCError.message("iPhone dùng kiểu đăng nhập VNC chưa hỗ trợ.") }
        guard sec == 1 || !(password ?? "").isEmpty else { throw VNCError.needPassword }
        send([sec])
        if sec == 2 {
            guard let r = Self.auth(password ?? "", try read(16)) else { throw VNCError.bad }
            send(r)
        }
        guard try read(4) == [0, 0, 0, 0] else { throw sec == 2 ? VNCError.badPassword : VNCError.message("iPhone từ chối kết nối.") }
        if sec == 2, let password { Keychain.set("vnc:" + target.address, password) }
        send([1])  // dùng chung với các máy khác đang xem
        let si = try read(24)
        var W = u16(si, 0), H = u16(si, 2)
        let nl = u32(si, 20)
        guard W > 0, H > 0, W <= 8192, H <= 8192, nl <= 4096 else { throw VNCError.bad }
        let name = String(decoding: try read(nl), as: UTF8.self)
        // 32 bit, little-endian, đỏ ở bit 16: trong bộ nhớ là B, G, R, X
        send([0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0])
        // Tight, CopyRect, Raw, đổi cỡ khung (xoay máy), JPEG chất lượng 6, nén zlib mức 2 (như mặc định của noVNC)
        let enc: [Int32] = [7, 1, 0, -223, -26, -254]
        send([2, 0, 0, UInt8(enc.count)] + enc.flatMap { e in [24, 16, 8, 0].map { UInt8(truncatingIfNeeded: e >> $0) } })
        var fb = [UInt32](repeating: 0, count: W * H)
        resized(W, H, name)
        let tight = TightDecoder()
        var full = true  // xin cả màn lúc đầu và sau khi khung đổi cỡ, còn lại chỉ phần thay đổi
        while !stopped {
            send([3, full ? 0 : 1, 0, 0, 0, 0, UInt8(W >> 8), UInt8(W & 255), UInt8(H >> 8), UInt8(H & 255)])
            full = false
            var t = try read(1)[0]
            while t != 0 {  // bỏ qua chuông, clipboard, bảng màu tới khi gặp khung hình
                switch t {
                case 2: break
                case 1: let b = try read(5); _ = try read(u16(b, 3) * 6)
                case 3:
                    var left = u32(try read(7), 3)
                    while left > 0 {
                        _ = try read(min(left, 1 << 16))
                        left -= min(left, 1 << 16)
                    }
                default: throw VNCError.bad
                }
                t = try read(1)[0]
            }
            let hdr = try read(3)
            for _ in 0..<u16(hdr, 1) {
                let b = try read(12)
                let x = u16(b, 0), y = u16(b, 2), w = u16(b, 4), h = u16(b, 6)
                let e = Int32(truncatingIfNeeded: u32(b, 8))
                if e == -223 {  // TrollVNC xoay khung theo máy
                    guard w > 0, h > 0, w <= 8192, h <= 8192 else { throw VNCError.bad }
                    W = w
                    H = h
                    fb = [UInt32](repeating: 0, count: W * H)
                    resized(W, H, name)
                    full = true
                    continue
                }
                guard x + w <= W, y + h <= H else { throw VNCError.bad }
                var px: [UInt32]
                switch e {
                case 1:
                    let s = try read(4)
                    let sx = u16(s, 0), sy = u16(s, 2)
                    guard sx + w <= W, sy + h <= H else { throw VNCError.bad }
                    px = []
                    px.reserveCapacity(w * h)
                    for r in 0..<h { px += fb[(sy + r) * W + sx..<(sy + r) * W + sx + w] }
                case 0:
                    px = try read(w * h * 4).withUnsafeBytes { Array($0.bindMemory(to: UInt32.self)) }
                case 7:
                    px = try tight.rect(read, w: w, h: h)
                default:
                    throw VNCError.message("iPhone gửi kiểu mã hoá hình chưa hỗ trợ.")
                }
                fb.withUnsafeMutableBufferPointer { f in
                    px.withUnsafeBufferPointer { p in
                        for r in 0..<h { (f.baseAddress! + (y + r) * W + x).update(from: p.baseAddress! + r * w, count: w) }
                    }
                }
            }
            present(fb, W, H)
        }
    }

    private func resized(_ w: Int, _ h: Int, _ name: String) {
        DispatchQueue.main.async { [self] in size = CGSize(width: w, height: h) }
    }

    /// Đưa khung mới lên màn hình; luồng chính bận thì chỉ giữ khung mới nhất, không xếp hàng.
    private func present(_ fb: [UInt32], _ w: Int, _ h: Int) {
        guard let provider = CGDataProvider(data: fb.withUnsafeBufferPointer { Data(buffer: $0) } as CFData),
              let img = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                                space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: Self.bitmapInfo),
                                provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent) else { return }
        let first = lock.locked { () -> Bool in
            latest = img
            defer { scheduled = true }
            return !scheduled
        }
        guard first else { return }
        DispatchQueue.main.async { [self] in
            let img = lock.locked { () -> CGImage? in
                scheduled = false
                return latest
            }
            view?.layer.contents = img
            if state != .live {
                state = .live
                onConnected?(target)
            }
        }
    }
}

/// Lớp hiện màn hình iPhone kia; chạm một ngón thành chạm trên iPhone kia, bàn phím iOS gõ sang.
final class VNCCanvas: UIView, UIKeyInput {
    weak var client: VNCClient?
    private var down = false

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        layer.contentsGravity = .resizeAspect
        isMultipleTouchEnabled = false  // TrollVNC chỉ có một ngón
    }
    required init?(coder: NSCoder) { fatalError() }

    private func point(_ t: UITouch) -> (Int, Int)? {
        guard let c = client, c.size.width > 0 else { return nil }
        let r = AVMakeRect(aspectRatio: c.size, insideRect: bounds), p = t.location(in: self)
        let x = Int((p.x - r.minX) / r.width * c.size.width), y = Int((p.y - r.minY) / r.height * c.size.height)
        return (min(max(x, 0), Int(c.size.width) - 1), min(max(y, 0), Int(c.size.height) - 1))
    }
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = touches.first, let (x, y) = point(t) else { return }
        down = true
        client?.pointer(1, x, y)
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard down, let t = touches.first, let (x, y) = point(t) else { return }
        client?.pointer(1, x, y)
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard down, let t = touches.first, let (x, y) = point(t) else { return }
        down = false
        client?.pointer(0, x, y)
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { touchesEnded(touches, with: event) }

    func toggleKeyboard() { _ = isFirstResponder ? resignFirstResponder() : becomeFirstResponder() }
    override var canBecomeFirstResponder: Bool { true }
    var hasText: Bool { true }  // để phím xoá luôn gửi được
    var autocorrectionType: UITextAutocorrectionType = .no
    var autocapitalizationType: UITextAutocapitalizationType = .none
    var spellCheckingType: UITextSpellCheckingType = .no
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartDashesType: UITextSmartDashesType = .no
    func insertText(_ text: String) { client?.type(text) }
    func deleteBackward() {
        client?.key(0xFF08, true)
        client?.key(0xFF08, false)
    }
}

struct VNCCanvasRep: UIViewRepresentable {
    let client: VNCClient
    func makeUIView(context: Context) -> VNCCanvas {
        let v = VNCCanvas()
        v.client = client
        client.view = v
        return v
    }
    func updateUIView(_ v: VNCCanvas, context: Context) {}
}

/// Màn điều khiển iPhone qua TrollVNC: hình kín màn, một nút nhỏ giữa mép trên mở các nút cứng và bàn phím.
struct VNCView: View {
    @ObservedObject var client: VNCClient
    var close: () -> Void
    @State private var pass = ""

    var body: some View {
        ZStack(alignment: .top) {
            Color.black.ignoresSafeArea()
            VNCCanvasRep(client: client).ignoresSafeArea()
            Menu {
                Button { client.button(4) } label: { Label("Nút Home", systemImage: "circle") }
                Button { client.view?.toggleKeyboard() } label: { Label("Bàn phím", systemImage: "keyboard") }
                Button { client.button(2) } label: { Label("Nút nguồn (khoá màn hình)", systemImage: "power") }
                Button(role: .destructive, action: close) { Label("Ngắt", systemImage: "xmark") }
            } label: {
                Image(systemName: "slider.horizontal.3").font(.system(size: 14, weight: .semibold)).frame(width: 46, height: 28)
            }
            .foregroundStyle(.primary)
            .overlayTrack(14)
            .accessibilityLabel("Nút Home, bàn phím, ngắt")
            status.frame(maxHeight: .infinity)
        }
        .statusBarHidden()
        .gameScreen()
        .onAppear {
            UIApplication.shared.isIdleTimerDisabled = true
            client.connect()
        }
        .onDisappear {
            UIApplication.shared.isIdleTimerDisabled = false
            client.disconnect()
        }
    }

    @ViewBuilder private var status: some View {
        switch client.state {
        case .connecting:
            HStack(spacing: 12) {
                ProgressView()
                Text("Đang kết nối tới \(client.target.name)…").font(.text(17))
            }
            .foregroundStyle(Theme.ink)
            .padding(.horizontal, 28)
            .padding(.vertical, 20)
            .glassCard()
        case .failed(let msg, let ask):
            VStack(spacing: 16) {
                Text(msg).font(.text(17)).foregroundStyle(Theme.ink).multilineTextAlignment(.center)
                if ask {
                    SecureField("Mật khẩu VNC (đặt trong Cài đặt > TrollVNC)", text: $pass)
                        .textFieldStyle(.roundedBorder)
                        .frame(maxWidth: 300)
                }
                HStack(spacing: 12) {
                    Button(ask ? "Kết nối" : "Thử lại") {
                        if ask { client.password = pass }
                        client.connect()
                    }
                    .primaryStyle()
                    Button("Đóng", action: close).secondaryStyle()
                }
            }
            .padding(28)
            .frame(maxWidth: 380)
            .glassCard()
        case .live:
            EmptyView()
        }
    }
}
