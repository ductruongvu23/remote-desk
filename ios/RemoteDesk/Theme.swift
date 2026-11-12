import SwiftUI
import UIKit

/// Màu của hệ thống iOS (tự đổi sáng/tối, theo cả chế độ tương phản cao) và xanh nhấn #0071E3 như bản PC.
/// Lớp nổi trên hình (nút, dải thông số, bảng điều khiển) dùng kính lỏng thật trên iOS 26, lớp mờ trên bản cũ hơn.
enum Theme {
    static let fill = Color(UIColor.tertiarySystemFill)
    static let ink = Color(UIColor.label)
    static let slate = Color(UIColor.secondaryLabel)
    static let link = dyn(0x0066CC, 0x2997FF)
    static let pill = dyn(0x0071E3, 0x0A84FF)
    /// Màu thương hiệu (biểu tượng app): xanh trời sang tím.
    static let brand = LinearGradient(colors: [Color(UIColor(hex: 0x4FACFE)), Color(UIColor(hex: 0x7B3FD1))],
                                      startPoint: .topLeading, endPoint: .bottomTrailing)
    /// Nút keymap vẽ bằng UIKit trên hình: rãnh mờ rgba(210,210,215,0.64), chữ đen 72%.
    static let uiOverlay = UIColor.dyn(0xD2D2D7, 0x424245, alpha: 0.64)
    static let uiOverlayInk = UIColor.dyn(0x000000, 0xFFFFFF, alpha: 0.72)

    static func dyn(_ light: UInt32, _ dark: UInt32) -> Color { Color(UIColor.dyn(light, dark)) }
}

extension UIColor {
    static func dyn(_ light: UInt32, _ dark: UInt32, alpha: CGFloat = 1) -> UIColor {
        UIColor { $0.userInterfaceStyle == .dark ? UIColor(hex: dark, alpha: alpha) : UIColor(hex: light, alpha: alpha) }
    }
    convenience init(hex: UInt32, alpha: CGFloat = 1) {
        self.init(red: CGFloat(hex >> 16 & 0xFF) / 255, green: CGFloat(hex >> 8 & 0xFF) / 255,
                  blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
    }
}

extension Font {
    /// SF Pro Display cho tiêu đề (hệ thống tự chọn Display ở cỡ lớn), SF Pro Text cho chữ thường.
    static func display(_ size: CGFloat) -> Font { .system(size: size, weight: .semibold) }
    static func text(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font { .system(size: size, weight: weight) }
}

extension View {
    /// Nút chính: kính tô màu nhấn trên iOS 26, nút tô màu của hệ thống trên bản cũ.
    @ViewBuilder func primaryStyle() -> some View {
        if #available(iOS 26, *) { buttonStyle(.glassProminent) } else { buttonStyle(.borderedProminent).buttonBorderShape(.capsule) }
    }

    /// Nút phụ: kính trong trên iOS 26, nút xám của hệ thống trên bản cũ.
    @ViewBuilder func secondaryStyle() -> some View {
        if #available(iOS 26, *) { buttonStyle(.glass) } else { buttonStyle(.bordered).buttonBorderShape(.capsule) }
    }

    /// Sheet nửa màn hình bo 28 (iOS 16+); iOS 15 hiện toàn trang.
    @ViewBuilder func sheetStyle() -> some View {
        if #available(iOS 16.4, *) {
            presentationDetents([.medium, .large]).presentationCornerRadius(28)
        } else if #available(iOS 16, *) {
            presentationDetents([.medium, .large])
        } else {
            self
        }
    }

    /// Màn điều khiển: ẩn thanh Home, hoãn cử chỉ hệ thống ở mép để vuốt trong game không bị thoát (iOS 16+).
    @ViewBuilder func gameScreen() -> some View {
        if #available(iOS 16, *) { persistentSystemOverlays(.hidden).defersSystemGestures(on: .all) } else { self }
    }

    /// Lớp nổi trên hình: kính lỏng trên iOS 26, lớp mờ trên bản cũ. Luôn tối: hình game phía sau thường tối,
    /// chữ trắng trên kính tối đọc rõ hơn ở cả chế độ sáng.
    @ViewBuilder func glass<S: Shape>(_ shape: S, interactive: Bool = false) -> some View {
        if #available(iOS 26, *) {
            glassEffect(interactive ? .regular.interactive() : .regular, in: shape).environment(\.colorScheme, .dark)
        } else {
            background(.thinMaterial, in: shape).environment(\.colorScheme, .dark)
        }
    }

    /// Nút nổi nhỏ, dải thông số trên hình.
    func overlayTrack(_ radius: CGFloat) -> some View {
        glass(RoundedRectangle(cornerRadius: radius, style: .continuous), interactive: true)
    }

    /// Thẻ báo giữa màn (đang kết nối, lỗi) trên hình.
    func glassCard() -> some View { glass(RoundedRectangle(cornerRadius: 28, style: .continuous)) }
}

/// Dòng "tên ... giá trị" như LabeledContent (chỉ có từ iOS 16).
struct LabeledRow: View {
    let title: String, value: String
    init(_ title: String, value: String) { self.title = title; self.value = value }
    var body: some View {
        HStack {
            Text(title)
            Spacer()
            Text(value).foregroundStyle(.secondary)
        }
    }
}

extension NSLock {
    /// Như withLock (iOS 16+), chạy cả iOS 15.
    func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock()
        defer { unlock() }
        return try body()
    }
}
