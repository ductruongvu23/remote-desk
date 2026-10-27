import CoreMedia
import Foundation

/// Kiểm tra nhanh phần không cần mạng: tách H.264, dựng định dạng từ SPS/PPS thật của NVENC, keymap.
enum SelfTest {
    static func run() -> Never {
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
        let pc = SavedHost(name: "PC", address: "192.168.0.103:8443", addresses: ["192.168.0.103:8443", "100.99.115.103:8443"])
        check(pc.candidates(onWiFi: true) == ["192.168.0.103:8443", "100.99.115.103:8443"], "thứ tự trên Wi‑Fi")
        check(pc.candidates(onWiFi: false) == ["100.99.115.103:8443", "192.168.0.103:8443"], "thứ tự trên 4G")
        check(SavedHost.isTailscale("100.64.0.1:8443") && !SavedHost.isTailscale("100.128.0.1") && !SavedHost.isTailscale("10.0.0.1"),
              "dải Tailscale")
        let out = SavedHost(name: "PC", address: "192.168.0.103:8443",
                            addresses: ["192.168.0.103:8443", "14.162.162.13:8443", "100.99.115.103:8443"])
        check(out.candidates(onWiFi: false).first == "14.162.162.13:8443" && out.candidates(onWiFi: true).last == "14.162.162.13:8443",
              "IP công khai: 4G thử trước, Wi‑Fi thử sau cùng")
        check(SavedHost.kind("172.20.10.2:8443") == .lan && SavedHost.kind("14.162.162.13:8443") == .internet
              && SavedHost.kind("my-pc.local:8443") == .lan, "phân loại địa chỉ")

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

        print(failed == 0 ? "self-test OK" : "self-test LỖI: \(failed)")
        exit(failed == 0 ? 0 : 1)
    }
}
