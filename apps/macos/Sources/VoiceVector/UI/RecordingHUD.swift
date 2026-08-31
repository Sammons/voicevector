import SwiftUI
import AppKit

/// Floating pill near the bottom of the screen while recording/processing.
/// A non-activating panel, so focus stays in the app being dictated into.
@MainActor
final class RecordingHUD {
    private var panel: NSPanel?
    private let dictation: DictationController

    init(dictation: DictationController) {
        self.dictation = dictation
    }

    func show() {
        if panel == nil {
            // Tall enough for the review staging card above the pill; the
            // view is bottom-aligned and the rest stays transparent.
            let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 520, height: 480),
                                styleMask: [.nonactivatingPanel, .fullSizeContentView, .borderless],
                                backing: .buffered, defer: false)
            panel.level = .statusBar
            panel.isOpaque = false
            panel.backgroundColor = .clear
            panel.hasShadow = true
            panel.ignoresMouseEvents = true
            panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
            panel.contentView = NSHostingView(rootView: HUDView(dictation: dictation))
            self.panel = panel
        }
        position()
        panel?.orderFrontRegardless()
    }

    func hide() {
        panel?.orderOut(nil)
    }

    private func position() {
        guard let panel else { return }
        // Follow the cursor's screen: NSScreen.main is the key window's
        // screen, which on multi-monitor setups is often not where the user
        // is dictating — the HUD would appear on the wrong display.
        let mouse = NSEvent.mouseLocation
        let screen = NSScreen.screens.first(where: { NSMouseInRect(mouse, $0.frame, false) })
            ?? NSScreen.main ?? NSScreen.screens.first
        guard let screen else { return }
        let frame = screen.visibleFrame
        let size = panel.frame.size
        panel.setFrameOrigin(NSPoint(x: frame.midX - size.width / 2,
                                     y: frame.minY + 84))
    }
}

struct HUDView: View {
    @ObservedObject var dictation: DictationController
    @State private var bars: [CGFloat] = Array(repeating: 0.15, count: 14)
    /// Monotonic tick so the clock repaints even when the level is static
    /// (identical @State values would otherwise skip the render pass).
    @State private var tick = 0

    private let timer = Timer.publish(every: 0.05, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(spacing: 10) {
            if let draft = dictation.reviewDraft {
                stagingCard(draft)
            }
            pill
        }
        .frame(width: 520, height: 480, alignment: .bottom)
        .onReceive(timer) { _ in
            tick += 1
            guard dictation.state == .recording else { return }
            var next = Array(bars.dropFirst())
            next.append(max(0.12, CGFloat(dictation.level)))
            bars = next
        }
    }

    private func stagingCard(_ draft: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            ScrollView {
                Text(draft)
                    .font(.system(.body, design: .rounded))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: 170)
            if let route = dictation.reviewRoute {
                HStack(spacing: 6) {
                    Image(systemName: "arrow.uturn.right")
                    Text(route)
                        .lineLimit(1)
                    Spacer()
                }
                .font(.caption.weight(.semibold))
                .foregroundStyle(Theme.accent)
            }
            if let image = dictation.routeImage, let nsImage = NSImage(data: image) {
                routePreview(nsImage)
            }
            HStack(spacing: 14) {
                Label(reviewHint, systemImage: reviewHintIcon)
                    .font(.caption.weight(.medium))
                    .foregroundStyle(dictation.state == .reviewing ? Color.secondary : Theme.accent)
                Spacer()
                Text(dictation.routeFields.count > 1 ? "⇥ cycle field   ⏎ send   esc discard"
                                                     : "⏎ send   esc discard")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(14)
        .frame(width: 520)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(Theme.accent.opacity(0.35), lineWidth: 1))
    }

    /// Target window thumbnail with the selected input field highlighted.
    private func routePreview(_ image: NSImage) -> some View {
        let aspect = image.size.height > 0 ? image.size.width / image.size.height : 1.6
        let width: CGFloat = 300
        let height = min(180, width / max(0.5, aspect))
        let fields = dictation.routeFields
        let selected = dictation.routeSelectedField
        return VStack(alignment: .leading, spacing: 4) {
            ZStack(alignment: .topLeading) {
                Image(nsImage: image)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(width: width, height: height)
                    .clipShape(RoundedRectangle(cornerRadius: 8))
                GeometryReader { geo in
                    if selected < fields.count {
                        let f = fields[selected].frame
                        Rectangle()
                            .stroke(Theme.accent, lineWidth: 2)
                            .background(Theme.accent.opacity(0.18))
                            .frame(width: max(6, f.width * geo.size.width),
                                   height: max(6, f.height * geo.size.height))
                            .offset(x: f.minX * geo.size.width, y: f.minY * geo.size.height)
                    }
                }
                .frame(width: width, height: height)
            }
            .frame(width: width, height: height)
            if selected < fields.count {
                Text("Field \(selected + 1) of \(fields.count): \(fields[selected].label)")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
        }
    }

    private var reviewHint: String {
        switch dictation.state {
        case .recording: return "Listening for a change…"
        case .processing(let step): return step
        default: return "Press the hotkey and say a change"
        }
    }

    private var reviewHintIcon: String {
        switch dictation.state {
        case .recording: return "mic.fill"
        case .processing: return "sparkles"
        default: return "text.bubble"
        }
    }

    private var pill: some View {
        HStack(spacing: 12) {
            switch dictation.state {
            case .recording:
                Image(systemName: "mic.fill")
                    .foregroundStyle(Theme.accent)
                waveform
                Text(elapsedLabel)
                    .id(tick)
                    .font(.system(.callout, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .frame(minWidth: 40)
            case .processing(let step):
                ProgressView().controlSize(.small)
                Text(step)
                    .font(.system(.callout, design: .rounded).weight(.medium))
            case .reviewing:
                Image(systemName: "doc.text")
                    .foregroundStyle(Theme.accent)
                Text("Reviewing")
                    .font(.system(.callout, design: .rounded).weight(.medium))
            default:
                EmptyView()
            }
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 12)
        .background(.ultraThinMaterial, in: Capsule())
        .overlay(Capsule().stroke(Theme.accent.opacity(0.35), lineWidth: 1))
        .frame(width: 240, height: 56)
    }

    private var waveform: some View {
        HStack(spacing: 3) {
            ForEach(bars.indices, id: \.self) { index in
                Capsule()
                    .fill(Theme.accent.opacity(0.85))
                    .frame(width: 3, height: 6 + bars[index] * 22)
            }
        }
        .animation(.linear(duration: 0.05), value: bars)
    }

    private var elapsedLabel: String {
        let seconds = Int(dictation.elapsed)
        return String(format: "%d:%02d", seconds / 60, seconds % 60)
    }
}
