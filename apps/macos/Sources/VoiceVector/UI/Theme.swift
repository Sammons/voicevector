import SwiftUI

/// One accent, quiet grays, generous whitespace. Everything visual funnels
/// through here so the app stays coherent.
enum Theme {
    /// Deep violet — the VoiceVector brand accent.
    static let accent = Color(red: 0.45, green: 0.35, blue: 0.95)
    static let accentSoft = accent.opacity(0.14)
    static let danger = Color(red: 0.85, green: 0.30, blue: 0.30)

    static let corner: CGFloat = 10
    static let padding: CGFloat = 14

    static func card() -> some ShapeStyle {
        .background.secondary
    }
}

extension View {
    /// Rounded card container used across the app.
    func vvCard() -> some View {
        padding(Theme.padding)
            .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: Theme.corner))
    }

    func vvSectionTitle() -> some View {
        font(.system(.subheadline, design: .rounded).weight(.semibold))
            .foregroundStyle(.secondary)
            .textCase(.uppercase)
    }
}

/// Small pill label (status, counts).
struct Pill: View {
    var text: String
    var color: Color = Theme.accent

    var body: some View {
        Text(text)
            .font(.system(.caption2, design: .rounded).weight(.semibold))
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .background(color.opacity(0.14), in: Capsule())
            .foregroundStyle(color)
    }
}
