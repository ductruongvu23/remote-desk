import SwiftUI
import UIKit

/// Token theo bản hướng dẫn Apple "white gallery": nền trắng/#f5f5f7, chữ Ink, thẻ bo 28 không bóng,
/// xanh #0071e3 chỉ cho nút hành động nhỏ. Bản tối theo trang tối của Apple. Tự đổi theo Giao diện của iOS.
enum Theme {
    static let canvas = dyn(0xFFFFFF, 0x000000)
    static let band = dyn(0xF5F5F7, 0x161617)
    static let card = dyn(0xFFFFFF, 0x1D1D1F)
    static let hairline = dyn(0xD6D6D6, 0x424245)
    static let ink = dyn(0x1D1D1F, 0xF5F5F7)
    static let slate = dyn(0x707070, 0xA1A1A6)
    static let steel = dyn(0x86868B, 0x86868B)
    static let link = dyn(0x0066CC, 0x2997FF)
    static let pill = dyn(0x0071E3, 0x0071E3)
    static let orange = dyn(0xB64400, 0xF56300)
    /// Rãnh mờ nổi trên video (kiểu "Carousel Playback Control"): rgba(210,210,215,0.64), chữ đen 56%.
    static let uiOverlay = UIColor.dyn(0xD2D2D7, 0x424245, alpha: 0.64)
    static let uiOverlayInk = UIColor.dyn(0x000000, 0xFFFFFF, alpha: 0.72)
    static let overlay = Color(uiOverlay), overlayInk = Color(uiOverlayInk)

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

/// Nút xanh nhỏ cho hành động chính (Pricing Blue pill).
struct BluePill: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.text(14)).tracking(-0.12)
            .foregroundStyle(.white)
            .padding(.horizontal, 16).padding(.vertical, 8)
            .background(Theme.pill.opacity(configuration.isPressed ? 0.75 : 1), in: Capsule())
    }
}

/// Nút pill viền mảnh, không tô màu (Outlined Explore Pill).
struct OutlinePill: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.text(14)).tracking(-0.12)
            .foregroundStyle(Theme.ink)
            .padding(.horizontal, 16).padding(.vertical, 8)
            .background(configuration.isPressed ? Theme.band : Color.clear, in: Capsule())
            .overlay(Capsule().stroke(Theme.steel, lineWidth: 1))
    }
}

extension View {
    /// Thẻ trắng bo 28, không viền, không bóng.
    func card(_ radius: CGFloat = 28) -> some View {
        background(Theme.card, in: RoundedRectangle(cornerRadius: radius, style: .continuous))
    }

    /// Rãnh mờ nổi trên video: lớp mờ hệ thống phía dưới, phủ màu rãnh lên trên, không bóng.
    func overlayTrack(_ radius: CGFloat) -> some View {
        let shape = RoundedRectangle(cornerRadius: radius, style: .continuous)
        return background(Theme.overlay, in: shape).background(.ultraThinMaterial, in: shape)
    }
}
