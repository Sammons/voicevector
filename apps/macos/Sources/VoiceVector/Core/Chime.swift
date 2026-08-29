import Foundation
import AVFoundation

/// Short synthesized chimes for recording start/stop — two soft sine notes with
/// a fast attack and exponential decay, generated in memory (no audio assets).
final class Chime {
    static let shared = Chime()

    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private let format = AVAudioFormat(standardFormatWithSampleRate: 44_100, channels: 1)!
    private var started = false

    private lazy var startBuffer = Chime.makeChime(notes: [(660, 0.0), (990, 0.09)], format: format)
    private lazy var stopBuffer = Chime.makeChime(notes: [(990, 0.0), (660, 0.09)], format: format)
    private lazy var errorBuffer = Chime.makeChime(notes: [(440, 0.0), (330, 0.12)], format: format)

    private init() {
        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: format)
        engine.mainMixerNode.outputVolume = 0.5
    }

    private func ensureRunning() -> Bool {
        if !started || !engine.isRunning {
            do {
                try engine.start()
                started = true
            } catch {
                Log.error("Chime engine failed to start: \(error.localizedDescription)")
                return false
            }
        }
        return true
    }

    private func play(_ buffer: AVAudioPCMBuffer) {
        guard ensureRunning() else { return }
        player.scheduleBuffer(buffer, at: nil, options: .interrupts)
        player.play()
    }

    func playStart() { play(startBuffer) }
    func playStop() { play(stopBuffer) }
    func playError() { play(errorBuffer) }

    /// Renders overlapping decaying sine notes: (frequency Hz, onset seconds).
    private static func makeChime(notes: [(Double, Double)], format: AVAudioFormat) -> AVAudioPCMBuffer {
        let sampleRate = format.sampleRate
        let duration = 0.45
        let frameCount = AVAudioFrameCount(duration * sampleRate)
        let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: frameCount)!
        buffer.frameLength = frameCount
        let samples = buffer.floatChannelData![0]
        for i in 0..<Int(frameCount) {
            let t = Double(i) / sampleRate
            var value = 0.0
            for (freq, onset) in notes where t >= onset {
                let local = t - onset
                let attack = min(local / 0.008, 1.0)
                let decay = exp(-local * 9.0)
                value += sin(2 * .pi * freq * local) * attack * decay * 0.35
            }
            samples[i] = Float(value)
        }
        return buffer
    }
}
