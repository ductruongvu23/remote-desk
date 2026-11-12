import PhotosUI
import SwiftUI
import UniformTypeIdentifiers

/// Một lần gửi/nhận file, hiện thành dải nhỏ dưới thanh công cụ.
struct FileTransfer: Equatable {
    var name: String
    var sending: Bool
    var progress = 0.0
    var saved: URL? = nil    // file đã nhận, để chia sẻ hoặc mở trong app Tệp
    var failed: String? = nil
    var finished: Bool { progress >= 1 || failed != nil }
}

enum Files {
    /// File nhận từ PC: Tệp > Trên iPhone > Remote Desk > Nhận từ PC (Info.plist bật UIFileSharingEnabled).
    static var inbox: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Nhận từ PC", isDirectory: true)
    }

    /// Tên PC gửi xuống: chỉ lấy phần tên (không cho "../"), không để rỗng.
    static func safeName(_ n: String) -> String {
        let s = (n.replacingOccurrences(of: "\\", with: "/") as NSString).lastPathComponent
            .trimmingCharacters(in: .whitespaces)
        return s.isEmpty || s == "." || s == ".." ? "file" : s
    }

    /// "a.jpg" đã có thì "a (1).jpg", "a (2).jpg"...
    static func unique(_ dir: URL, _ name: String) -> URL {
        var url = dir.appendingPathComponent(name)
        let stem = (name as NSString).deletingPathExtension, ext = (name as NSString).pathExtension
        var i = 1
        while FileManager.default.fileExists(atPath: url.path) {
            url = dir.appendingPathComponent(ext.isEmpty ? "\(stem) (\(i))" : "\(stem) (\(i)).\(ext)")
            i += 1
        }
        return url
    }

    /// Chép file sắp gửi vào thư mục tạm riêng (file từ app Tệp chỉ đọc được trong lúc mở quyền), gửi xong thì xoá.
    static func stage(_ src: URL, name: String? = nil) throws -> URL {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("send-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let scoped = src.startAccessingSecurityScopedResource()
        defer { if scoped { src.stopAccessingSecurityScopedResource() } }
        let dst = dir.appendingPathComponent(name ?? src.lastPathComponent)
        try FileManager.default.copyItem(at: src, to: dst)
        return dst
    }

    /// Bảng chia sẻ của iOS (lưu vào Ảnh, mở bằng app khác); ShareLink chỉ có từ iOS 16.
    static func share(_ url: URL) {
        let vc = UIActivityViewController(activityItems: [url], applicationActivities: nil)
        let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
        guard var top = scene?.keyWindow?.rootViewController else { return }
        while let p = top.presentedViewController { top = p }
        vc.popoverPresentationController?.sourceView = top.view
        top.present(vc, animated: true)
    }
}

/// Chọn ảnh, video trong Thư viện bằng PHPicker (có từ iOS 14, không cần xin quyền Ảnh). Mỗi mục chép ra file tạm
/// giữ tên và định dạng gốc (HEIC, MOV...) rồi gửi sang PC; video nhận thẳng thành file, không nạp vào bộ nhớ.
struct PhotoPicker: UIViewControllerRepresentable {
    var done: ([URL]) -> Void

    func makeUIViewController(context: Context) -> PHPickerViewController {
        var c = PHPickerConfiguration()
        c.filter = .any(of: [.images, .videos])
        c.selectionLimit = 20
        c.preferredAssetRepresentationMode = .current  // giữ định dạng gốc, không đổi HEIC sang JPEG
        let vc = PHPickerViewController(configuration: c)
        vc.delegate = context.coordinator
        return vc
    }

    func updateUIViewController(_ vc: PHPickerViewController, context: Context) {}
    func makeCoordinator() -> Coordinator { Coordinator(done: done) }

    final class Coordinator: NSObject, PHPickerViewControllerDelegate {
        let done: ([URL]) -> Void
        init(done: @escaping ([URL]) -> Void) { self.done = done }

        func picker(_ picker: PHPickerViewController, didFinishPicking results: [PHPickerResult]) {
            picker.dismiss(animated: true)
            let group = DispatchGroup(), lock = NSLock()
            var urls: [URL] = []
            for r in results {
                let p = r.itemProvider
                guard let type = p.registeredTypeIdentifiers.first(where: { UTType($0)?.conforms(to: .movie) == true })
                        ?? p.registeredTypeIdentifiers.first(where: { UTType($0)?.conforms(to: .image) == true }) else { continue }
                group.enter()
                p.loadFileRepresentation(forTypeIdentifier: type) { url, _ in
                    defer { group.leave() }
                    guard let url else { return }  // file tạm chỉ còn trong lúc gọi lại: chép ngay
                    let ext = url.pathExtension.isEmpty ? (UTType(type)?.preferredFilenameExtension ?? "bin") : url.pathExtension
                    let name = p.suggestedName.map { "\($0).\(ext)" } ?? url.lastPathComponent
                    if let s = try? Files.stage(url, name: name) { lock.locked { urls.append(s) } }
                }
            }
            group.notify(queue: .main) { [done] in done(urls) }
        }
    }
}

/// Bản mới trên GitHub Release. App cài bằng Sideloadly/AltStore không cập nhật qua App Store: báo bản mới, mở trang tải,
/// hoặc thêm nguồn AltStore/SideStore để chúng tự cập nhật.
enum AppUpdate {
    static let repo = "ductruongvu23/remote-desk"
    static let page = URL(string: "https://github.com/\(repo)/releases/latest")!
    static let source = "https://github.com/\(repo)/releases/latest/download/altstore.json"
    static var current: String { Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0" }

    /// "1.10.0" mới hơn "1.9.2"; bỏ chữ v đầu tag.
    static func newer(_ a: String, than b: String) -> Bool {
        a.trimmingCharacters(in: CharacterSet(charactersIn: "vV")).compare(b.trimmingCharacters(in: CharacterSet(charactersIn: "vV")),
                                                                          options: .numeric) == .orderedDescending
    }

    /// Tag bản mới nhất, nil nếu không hỏi được (không mạng, quá hạn mức GitHub).
    static func latest() async -> String? {
        var req = URLRequest(url: URL(string: "https://api.github.com/repos/\(repo)/releases/latest")!)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        guard let (d, _) = try? await URLSession.shared.data(for: req),
              let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else { return nil }
        return j["tag_name"] as? String
    }

    static func addSource(_ scheme: String) -> URL? {
        var c = URLComponents()
        c.scheme = scheme
        c.host = "source"
        c.queryItems = [URLQueryItem(name: "url", value: source)]
        return c.url
    }
}
