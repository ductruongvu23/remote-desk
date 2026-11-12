import CoreMedia
import SwiftUI
import UIKit
import zlib
import Foundation

/// Kiểm tra nhanh phần không cần mạng: tách H.264, dựng định dạng từ SPS/PPS thật của NVENC, keymap.
enum SelfTest {
    static func run() -> Never {
        setvbuf(stdout, nil, _IONBF, 0)  // in ngay từng dòng: app có văng thì log CI vẫn thấy tới bước nào
        print("self-test: bắt đầu")
        var failed = 0
        func check(_ ok: Bool, _ what: String, line: Int = #line) {
            if !ok { failed += 1; print("SELF-TEST LỖI dòng \(line): \(what)") }
        }
        func hex(_ s: String) -> [UInt8] {
            stride(from: 0, to: s.count, by: 2).map { i in
                UInt8(s[s.index(s.startIndex, offsetBy: i)..<s.index(s.startIndex, offsetBy: i + 2)], radix: 16)!
            }
        }

        // SPS/PPS lấy từ remote_host.exe (NVIDIA H.264 Encoder MFT, 1920×1200)
        let sps = hex("674d403295a01e0097b01100000303e80001d4c00f1c2aa0"), pps = hex("68ee3c80")
        let start: [UInt8] = [0, 0, 0, 1]
        let idr: [UInt8] = [0x65, 0x88, 0x80, 0x10, 0x00]  // có byte 0 ở cuối: phải cắt đi
        var packet: [UInt8] = [1]  // cờ khung khoá, rồi AUD, SPS, PPS, IDR (start code 3 byte)
        for part in [[0x09, 0xF0], sps, pps] as [[UInt8]] { packet += start + part }
        packet += [0, 0, 1] + idr
        let (ps, avcc) = VideoRenderer.split(packet, from: 1)
        check(ps == [Data(sps), Data(pps)], "tách SPS/PPS")
        check(avcc == Data([0, 0, 0, 4, 0x65, 0x88, 0x80, 0x10]), "AVCC: \(Array(avcc))")
        if let ps, let f = VideoRenderer.makeFormat(ps, hevc: false) {
            let d = CMVideoFormatDescriptionGetDimensions(f)
            check(d.width == 1920 && d.height == 1200, "kích thước \(d.width)x\(d.height)")
            check(VideoRenderer.sample(avcc, f) != nil, "tạo CMSampleBuffer")
        } else {
            check(false, "CMVideoFormatDescriptionCreateFromH264ParameterSets")
        }

        // HEVC: VPS/SPS/PPS thật từ remote_host.exe (NVIDIA HEVC Encoder MFT, 1920×1080)
        let vps = hex("40010c01ffff21600000030090000003000003007bac09")
        let hsps = hex("42010121600000030090000003000003007ba003c0801107cb96b4a421192e30142000002080000003008000001e04")
        let hpps = hex("4401c1f7c0cc90"), hidr: [UInt8] = [0x26, 0x01, 0xaf, 0x10]
        var hp: [UInt8] = [3]  // cờ: khung khoá + HEVC; rồi AUD (35), VPS, SPS, PPS, IDR
        for part in [[0x46, 0x01, 0x10], vps, hsps, hpps, hidr] as [[UInt8]] { hp += start + part }
        let (hps, havcc) = VideoRenderer.split(hp, from: 1, hevc: true)
        check(hps?.count == 3 && havcc == Data([0, 0, 0, 4] + hidr), "tách HEVC")
        if let hps, let f = VideoRenderer.makeFormat(hps, hevc: true) {
            let d = CMVideoFormatDescriptionGetDimensions(f)
            check(d.width == 1920 && d.height == 1080, "HEVC: kích thước \(d.width)x\(d.height)")
            check(VideoRenderer.sample(havcc, f) != nil, "HEVC: tạo CMSampleBuffer")
        } else {
            check(false, "CMVideoFormatDescriptionCreateFromHEVCParameterSets")
        }
        check(VideoRenderer.split(hp, from: 1, hevc: false).params == nil, "gói HEVC không bị đọc nhầm thành H.264")

        print("self-test: xong phần hình")
        // Keymap: đọc đúng file của PC, tin nhắn lưu có "data" ở cuối
        let json = #"[{"label":"Nhảy","keys":"SPACE","x":0.5,"y":0.25}]"#
        let km = try? JSONDecoder().decode([KeyButton].self, from: Data(json.utf8))
        check(km?.first?.label == "Nhảy" && km?.first?.y == 0.25, "đọc keymap")
        let msg = KeyButton.saveMessage(km ?? []) ?? ""
        check(msg.hasPrefix(#"{"t":"keymap","data":["#) && msg.hasSuffix("]}"), "tin nhắn keymap: \(msg)")
        check(!msg.contains("\"id\""), "keymap không gửi id")
        check(SavedHost.normalize(" 192.168.1.12 ") == "192.168.1.12:8443" && SavedHost.normalize("a:1") == "a:1", "địa chỉ")
        check(LinkStats(ping: 20).quality == 0 && LinkStats(ping: 60).quality == 1 && LinkStats(ping: 150).quality == 2,
              "chấm màu chất lượng mạng")
        check(km?.first?.kind == nil && km?.first?.mode == nil && km?.first?.scale == 1, "file keymap cũ: trường mới để trống")
        let fire = try? JSONDecoder().decode(KeyButton.self, from: Data(#"{"label":"Bắn","keys":"LMB","x":0.7,"y":0.6,"mode":"turbo","aim":true,"size":1.5}"#.utf8))
        check(fire?.mode == .turbo && fire?.aim == true && fire?.scale == 1.5, "đọc nút liên thanh + kéo để ngắm")

        // Địa chỉ: Wi‑Fi thử LAN trước, 4G thử Tailscale trước, không trùng lặp
        let pc = SavedHost(name: "PC", address: "192.168.0.103:8443", addresses: ["192.168.0.103:8443", "100.100.1.2:8443"])
        check(pc.candidates(onWiFi: true) == ["192.168.0.103:8443", "100.100.1.2:8443"], "thứ tự trên Wi‑Fi")
        check(pc.candidates(onWiFi: false) == ["100.100.1.2:8443", "192.168.0.103:8443"], "thứ tự trên 4G")
        check(SavedHost.isTailscale("100.64.0.1:8443") && !SavedHost.isTailscale("100.128.0.1") && !SavedHost.isTailscale("10.0.0.1"),
              "dải Tailscale")
        let out = SavedHost(name: "PC", address: "192.168.0.103:8443",
                            addresses: ["192.168.0.103:8443", "203.0.113.7:8443", "100.100.1.2:8443"])
        check(out.candidates(onWiFi: false).first == "203.0.113.7:8443" && out.candidates(onWiFi: true).last == "203.0.113.7:8443",
              "IP công khai: 4G thử trước, Wi‑Fi thử sau cùng")
        check(SavedHost.kind("172.20.10.2:8443") == .lan && SavedHost.kind("203.0.113.7:8443") == .internet
              && SavedHost.kind("my-pc.local:8443") == .lan && SavedHost.kind("Nha.DuckDNS.org:8443") == .internet
              && SavedHost.kind("mypc:8443") == .lan, "phân loại địa chỉ")

        // Cần di chuyển: vùng chết, 8 hướng, đẩy hết cỡ thì thêm phím chạy
        let wasd = "W,A,S,D"
        check(Stick.keys(dx: 5, dy: 5, radius: 60, map: wasd, sprint: nil).isEmpty, "vùng chết")
        check(Stick.keys(dx: 0, dy: -40, radius: 60, map: wasd, sprint: nil) == ["W"], "lên")
        check(Stick.keys(dx: -40, dy: 0, radius: 60, map: wasd, sprint: nil) == ["A"], "trái")
        check(Stick.keys(dx: 30, dy: 30, radius: 60, map: wasd, sprint: nil) == ["S", "D"], "xuống phải")
        check(Stick.keys(dx: -30, dy: -30, radius: 60, map: wasd, sprint: nil) == ["W", "A"], "lên trái")
        check(Stick.keys(dx: 0, dy: -59, radius: 60, map: wasd, sprint: "shift") == ["W", "SHIFT"], "đẩy hết cỡ để chạy")
        check(Stick.keys(dx: 0, dy: -40, radius: 60, map: "W,A,S", sprint: nil).isEmpty, "cần thiếu phím")

        // Mẫu keymap: cần có đủ 4 phím, hồ sơ đọc/ghi lại được
        let sticks = KeyProfile.presets.flatMap(\.keys).filter(\.isStick)
        check(!sticks.isEmpty && sticks.allSatisfy { $0.keys.split(separator: ",").count == 4 }, "mẫu: cần di chuyển đủ 4 phím")
        let round = (try? JSONEncoder().encode(KeyProfile.presets)).flatMap { try? JSONDecoder().decode([KeyProfile].self, from: $0) }
        check(round?.map(\.name) == KeyProfile.presets.map(\.name) && round?[1].touch == .look, "lưu/đọc hồ sơ")
        check(KeyButton.validKeys("CTRL+SHIFT+ESC") && KeyButton.validKeys("f5") && KeyButton.validKeys("WHEELUP")
              && !KeyButton.validKeys("LSHIFTT") && !KeyButton.validKeys("F25") && !KeyButton.validKeys("Đ") && !KeyButton.validKeys(""),
              "tên phím")
        check(KeyProfile.presets.count >= 15 && Set(KeyProfile.presets.map(\.name)).count == KeyProfile.presets.count, "đủ mẫu, không trùng tên")
        print("self-test: xong keymap, kiểm bố cục \(KeyProfile.presets.count) mẫu")
        // Mỗi mẫu: phím PC hiểu được; nút không chồng nhau, không nằm dưới thanh công cụ, không lòi ra ngoài (iPhone 11 Pro Max nằm ngang)
        let screen = CGRect(x: 0, y: 0, width: 896, height: 414), bar = CGRect(x: 448 - 180, y: 0, width: 360, height: 48)
        for p in KeyProfile.presets {
            for k in p.keys where !k.isValid { check(false, "\(p.name): phím \"\(k.keys)\" của nút \(k.label)") }
            let rects = p.keys.filter { !$0.isStick }.map { k -> CGRect in
                let c = KeyCap(k).bounds.size
                return CGRect(x: k.x * screen.width - c.width / 2, y: k.y * screen.height - c.height / 2, width: c.width, height: c.height)
            }
            for i in rects.indices {
                let name = p.keys.filter { !$0.isStick }[i].label
                check(screen.contains(rects[i]), "\(p.name): nút \(name) lòi ra ngoài màn hình")
                check(!rects[i].intersects(bar), "\(p.name): nút \(name) nằm dưới thanh công cụ")
                for j in rects.indices where j > i && rects[i].insetBy(dx: -3, dy: -3).intersects(rects[j]) {
                    check(false, "\(p.name): nút \(name) chồng lên \(p.keys.filter { !$0.isStick }[j].label)")
                }
            }
        }

        print("self-test: xong bố cục")
        // Mã QR trên PC: địa chỉ thiếu cổng thì thêm 8443, chứng chỉ ghi sẵn để khỏi hỏi tin
        let qr = URL(string: "remotedesk://pair?n=DESKTOP-1&a=192.168.0.103,203.0.113.7:9000&c=0123abcd&f=AABB")!
        let pr = SavedHost.pair(qr)
        check(pr?.host.address == "192.168.0.103:8443" && pr?.host.addresses == ["192.168.0.103:8443", "203.0.113.7:9000"]
              && pr?.code == "0123abcd" && pr?.host.certHash == "aabb" && pr?.host.name == "DESKTOP-1", "đọc mã QR")
        check(SavedHost.pair(URL(string: "remotedesk://pair?a=1.2.3.4")!) == nil && SavedHost.pair(URL(string: "https://x/pair?a=1&c=2")!) == nil,
              "mã QR thiếu mã hoặc sai loại")

        // Chuyển file và cập nhật
        check(AppUpdate.newer("v1.10.0", than: "1.9.2") && !AppUpdate.newer("v1.2.0", than: "1.2.0") && !AppUpdate.newer("1.1", than: "1.2"),
              "so phiên bản")
        check(Files.safeName("../../x/a.txt") == "a.txt" && Files.safeName("..") == "file" && Files.safeName(" ") == "file"
              && Files.safeName("C:\\a\\b.pdf") == "b.pdf", "tên file nhận")
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("st-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: tmp.appendingPathComponent("a.jpg").path, contents: Data())
        check(Files.unique(tmp, "a.jpg").lastPathComponent == "a (1).jpg" && Files.unique(tmp, "b").lastPathComponent == "b", "tên trùng")
        let staged = try? Files.stage(tmp.appendingPathComponent("a.jpg"))
        check(staged?.lastPathComponent == "a.jpg" && staged.map { FileManager.default.fileExists(atPath: $0.path) } == true, "chép file tạm để gửi")
        let src = AppUpdate.addSource("altstore")?.absoluteString ?? ""
        check(src.hasPrefix("altstore://source?url=") && src.contains("altstore.json"), "link nguồn AltStore: \(src)")

        // TrollVNC: độ dài compact, xác thực VNC (vector tính bằng Python), phím, địa chỉ, và giải mã Tight: một màu,
        // zlib nối luồng qua nhiều ô và sau khi reset luồng, bảng màu 1 bit, gradient, JPEG (giống self-test của bản PC)
        func reader(_ d: [UInt8]) -> (VNCRead, () -> Int) {
            var pos = 0
            return ({ n in
                guard pos + n <= d.count else { throw VNCError.lost }
                defer { pos += n }
                return Array(d[pos..<pos + n])
            }, { pos })
        }
        var (rd, at) = reader([0x90, 0x4E, 0x05])
        check((try? TightDecoder.length(rd)) == 10000 && at() == 2, "độ dài compact 2 byte")
        (rd, at) = reader([0xFF, 0xFF, 0x01])
        check((try? TightDecoder.length(rd)) == 0x7FFF && at() == 3, "độ dài compact 3 byte")
        check(VNCClient.auth("trollvnc", Array(0..<16))?.map { String(format: "%02x", $0) }.joined() == "f274f5a243dfda6253d8eb67f74a554c",
              "xác thực VNC (DES)")
        check(VNCClient.keys(for: "A").map { $0 == (0x61, true) } == true && VNCClient.keys(for: ":").map { $0 == (0x3B, true) } == true
              && VNCClient.keys(for: "a").map { $0 == (0x61, false) } == true && VNCClient.keys(for: "\n").map { $0 == (0xFF0D, false) } == true
              && VNCClient.keys(for: "đ") == nil, "phím gõ sang iPhone")
        check(VNCTarget.normalize(" 192.168.0.101 ") == "192.168.0.101:5901" && VNCTarget.isSelf("127.0.0.1:5901")
              && !VNCTarget.isSelf("8.8.8.8:5901"), "địa chỉ iPhone, chặn tự điều khiển chính máy")
        let zp = (0..<2).map { _ -> UnsafeMutablePointer<z_stream> in
            let p = UnsafeMutablePointer<z_stream>.allocate(capacity: 1)
            p.initialize(to: z_stream())
            deflateInit_(p, 6, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size))
            return p
        }
        func zsync(_ id: Int, _ raw: [UInt8]) -> [UInt8] {  // như máy chủ: nén tiếp luồng cũ, đẩy ra bằng Z_SYNC_FLUSH
            var inp = raw, out = [UInt8](repeating: 0, count: raw.count + 64)
            let used = inp.withUnsafeMutableBufferPointer { i in
                out.withUnsafeMutableBufferPointer { o -> Int in
                    zp[id].pointee.next_in = i.baseAddress
                    zp[id].pointee.avail_in = uInt(i.count)
                    zp[id].pointee.next_out = o.baseAddress
                    zp[id].pointee.avail_out = uInt(o.count)
                    deflate(zp[id], Z_SYNC_FLUSH)
                    return o.count - Int(zp[id].pointee.avail_out)
                }
            }
            return Array(out[..<used])
        }
        func clen(_ n: Int) -> [UInt8] {
            n <= 0x7F ? [UInt8(n)] : n <= 0x3FFF ? [UInt8(n & 0x7F | 0x80), UInt8(n >> 7)]
                : [UInt8(n & 0x7F | 0x80), UInt8(n >> 7 & 0x7F | 0x80), UInt8(n >> 14)]
        }
        func rgbPx(_ rgb: [UInt8]) -> [UInt32] {
            stride(from: 0, to: rgb.count, by: 3).map { 0xFF00_0000 | UInt32(rgb[$0]) << 16 | UInt32(rgb[$0 + 1]) << 8 | UInt32(rgb[$0 + 2]) }
        }
        let tight = TightDecoder()
        (rd, at) = reader([0x80, 10, 20, 30])
        check((try? tight.rect(rd, w: 3, h: 2)) == [UInt32](repeating: 0xFF0A_141E, count: 6), "Tight: một màu")
        var rgb = (0..<36).map { UInt8(truncatingIfNeeded: $0 * 7) }
        for k in 0..<3 {  // ô 2 nối tiếp luồng của ô 1; trước ô 3 máy chủ reset luồng 0 rồi nén lại từ đầu
            if k == 2 {
                deflateEnd(zp[0])
                zp[0].pointee = z_stream()
                deflateInit_(zp[0], 6, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size))
            }
            let z = zsync(0, rgb), msg = [UInt8(k == 2 ? 1 : 0)] + clen(z.count) + z
            (rd, at) = reader(msg)
            check((try? tight.rect(rd, w: 4, h: 3)) == rgbPx(rgb) && at() == msg.count, "Tight: zlib ô \(k + 1)")
            rgb = rgb.map { $0 &+ 3 }
        }
        let c0: UInt32 = 0xFF01_0203, c1: UInt32 = 0xFFC8_C9CA
        (rd, at) = reader([0x40, 1, 1, 1, 2, 3, 200, 201, 202, 0b1010_0000, 0b0100_0000, 0, 0b1000_0000])
        check((try? tight.rect(rd, w: 10, h: 2)) == [c1, c0, c1, c0, c0, c0, c0, c0, c0, c1, c0, c0, c0, c0, c0, c0, c0, c0, c1, c0],
              "Tight: bảng 2 màu")
        var res = [UInt8](repeating: 0, count: 18)
        res[0] = 10
        res[1] = 20
        res[2] = 30
        let gz = zsync(1, res)
        (rd, at) = reader([0x50, 2] + clen(gz.count) + gz)
        check((try? tight.rect(rd, w: 3, h: 2)) == [UInt32](repeating: 0xFF0A_141E, count: 6), "Tight: gradient")
        let jpg = hex("ffd8ffe000104a46494600010100000100010000ffdb0043000302020302020303030304030304050805050404050a070706080c0a0c0c0b0a0b0b0d0e12"
                      + "100d0e110e0b0b1016101113141515150c0f171816141812141514ffdb00430103040405040509050509140d0b0d14141414141414141414141414141414"
                      + "14141414141414141414141414141414141414141414141414141414141414141414ffc00011080008001003012200021101031101ffc4001500010100000000"
                      + "000000000000000000000004ffc40014100100000000000000000000000000000000ffc4001501010100000000000000000000000000000708ffc4001411010000"
                      + "0000000000000000000000000000ffda000c03010002110311003f0090007a907fffd9")
        (rd, at) = reader([0x90] + clen(jpg.count) + jpg)
        let jp = (try? tight.rect(rd, w: 16, h: 8)) ?? []
        check(jp.count == 128 && jp.allSatisfy { abs(Int($0 >> 16 & 255) - 200) < 6 && abs(Int($0 >> 8 & 255) - 100) < 6 && abs(Int($0 & 255) - 50) < 6 },
              "Tight: JPEG")
        (rd, at) = reader([0xA0])
        check((try? tight.rect(rd, w: 4, h: 4)) == nil, "Tight: PNG (không xin) bị từ chối")
        for p in zp {
            deflateEnd(p)
            p.deallocate()
        }

        // Danh sách máy: PC đã lưu và PC đang thấy trong mạng cùng tên gộp thành một dòng
        let ms = Machine<SavedHost, String>.merge([SavedHost(name: "PC-A", address: "1.2.3.4:8443"), SavedHost(name: "PC-B", address: "b:8443")],
                                                 ["PC-B", "PC-C"], name: \.name, address: \.address, foundName: { $0 })
        check(ms.map(\.name) == ["PC-A", "PC-B", "PC-C"] && ms[0].found == nil && ms[1].found == "PC-B" && ms[2].saved == nil
              && ms[2].id == "mdns:PC-C" && ms[0].id == "1.2.3.4:8443", "gộp PC đã lưu với PC trong mạng")

        print(failed == 0 ? "self-test OK" : "self-test LỖI: \(failed)")
        usleep(300_000)  // cho simctl --console kịp đọc dòng cuối trước khi app thoát
        exit(failed == 0 ? 0 : 1)
    }
}

/// Ảnh chụp giao diện trên CI (build.sh --test): màn điều khiển xoay ngang, bảng mở sẵn, không cần PC.
struct DemoStream: View {
    @StateObject private var session = Session(host: SavedHost(name: "MAY-CHOI-GAME", address: "192.168.0.102:8443"))

    var body: some View {
        StreamView(session: session, demo: true) {}
            .onAppear {
                if #available(iOS 16, *) {
                    (UIApplication.shared.connectedScenes.first as? UIWindowScene)?
                        .requestGeometryUpdate(.iOS(interfaceOrientations: .landscapeRight))
                }
            }
    }
}
