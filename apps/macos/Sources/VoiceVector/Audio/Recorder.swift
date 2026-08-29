import Foundation
import AVFoundation

/// Microphone capture: AVAudioEngine input tap → AVAudioConverter → 16 kHz mono
/// 16-bit WAV on disk. Publishes a smoothed input level for the HUD waveform.
final class Recorder {
    static let targetSampleRate = 16_000

    /// 0…1, updated on an internal queue while recording.
    private(set) var level: Float = 0

    /// Streamed transcription: when `chunking`, a WAV slice is emitted after
    /// ~2 s of silence (segments need ≥5 s of audio containing speech). The
    /// master recording file is unaffected. Called on the recorder queue.
    var chunking = false
    var onSegment: ((Data, Int) -> Void)?
    /// Byte offset (into WAV data) where the un-flushed tail begins.
    private(set) var tailStartByte: UInt32 = 0
    private var segmentIndex = 0
    private var lastVoicedAt: TimeInterval = 0
    private var voicedInSegment = false
    private let silenceCutAfter: TimeInterval = 2.0
    private let minSegmentSeconds = 5.0
    private let voiceRMSThreshold: Float = 0.015

    private let engine = AVAudioEngine()
    private var converter: AVAudioConverter?
    private var writer: WavWriter?
    private var startedAt: Date?
    private let queue = DispatchQueue(label: "io.sammons.voicevector.recorder")

    var isRecording: Bool { writer != nil }

    static func requestPermission() async -> Bool {
        await AVCaptureDevice.requestAccess(for: .audio)
    }

    static var permissionGranted: Bool {
        AVCaptureDevice.authorizationStatus(for: .audio) == .authorized
    }

    func start(to url: URL) throws {
        guard writer == nil else { return }

        let input = engine.inputNode
        let inputFormat = input.inputFormat(forBus: 0)
        guard inputFormat.sampleRate > 0 else {
            throw NSError(domain: "VoiceVector", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "No audio input device available."])
        }

        let targetFormat = AVAudioFormat(commonFormat: .pcmFormatInt16,
                                         sampleRate: Double(Self.targetSampleRate),
                                         channels: 1, interleaved: true)!
        guard let converter = AVAudioConverter(from: inputFormat, to: targetFormat) else {
            throw NSError(domain: "VoiceVector", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "Audio format conversion unavailable."])
        }
        self.converter = converter

        let writer = try WavWriter(url: url, sampleRate: Self.targetSampleRate)
        self.writer = writer
        startedAt = Date()
        tailStartByte = 0
        segmentIndex = 0
        voicedInSegment = false
        lastVoicedAt = ProcessInfo.processInfo.systemUptime

        input.installTap(onBus: 0, bufferSize: 4096, format: inputFormat) { [weak self] buffer, _ in
            self?.queue.async { self?.process(buffer: buffer, converter: converter, writer: writer) }
        }

        engine.prepare()
        try engine.start()
    }

    /// Stops capture, closes the file. Returns duration in seconds.
    @discardableResult
    func stop() -> Double {
        guard let writer else { return 0 }
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        self.writer = nil
        converter = nil
        level = 0
        var duration: Double = 0
        queue.sync { duration = writer.finalize() }
        startedAt = nil
        return duration
    }

    /// Stops and deletes the file (canceled/stray recordings).
    func discard() {
        guard let url = writer?.url else { return }
        stop()
        try? FileManager.default.removeItem(at: url)
    }

    var elapsed: TimeInterval {
        startedAt.map { Date().timeIntervalSince($0) } ?? 0
    }

    /// RMS (0…1 scale) of the loudest channel, whatever the sample format.
    static func sourceRMS(_ buffer: AVAudioPCMBuffer) -> Float {
        let frames = Int(buffer.frameLength)
        let channels = Int(buffer.format.channelCount)
        guard frames > 0, channels > 0 else { return 0 }
        let interleaved = buffer.format.isInterleaved
        let stride = interleaved ? channels : 1
        var loudest: Float = 0

        func accumulate(_ sample: (Int) -> Float) -> Float {
            var sum: Float = 0
            var index = 0
            for _ in 0..<frames {
                let v = sample(index)
                sum += v * v
                index += stride
            }
            return sqrt(sum / Float(frames))
        }

        for channel in 0..<channels {
            // For interleaved data every channel lives in plane 0, offset by
            // its index; for non-interleaved data each channel has a plane.
            let plane = interleaved ? 0 : channel
            let offset = interleaved ? channel : 0
            let rms: Float
            if let data = buffer.floatChannelData {
                let base = data[plane] + offset
                rms = accumulate { Float(base[$0]) }
            } else if let data = buffer.int32ChannelData {
                let base = data[plane] + offset
                rms = accumulate { Float(base[$0]) / 2_147_483_648 }
            } else if let data = buffer.int16ChannelData {
                let base = data[plane] + offset
                rms = accumulate { Float(base[$0]) / 32_768 }
            } else {
                return 0
            }
            loudest = max(loudest, rms)
        }
        return loudest
    }

    private func process(buffer: AVAudioPCMBuffer, converter: AVAudioConverter, writer: WavWriter) {
        // Level metering from the source buffer. Devices arrive as Float32,
        // Int16, or Int32 (24-bit interfaces such as a Focusrite Scarlett), in
        // any channel count, interleaved or not — the loudest channel wins so
        // a mic on input 2 still meters. Also drives silence-gap chunking.
        let rms = Self.sourceRMS(buffer)
        if buffer.frameLength > 0 {
            // Perceptual-ish scaling; smooth decay so the waveform breathes.
            let scaled = min(1, rms * 18)
            level = max(scaled, level * 0.7)
        }

        // Silence-gap segmentation for streamed transcription.
        if chunking {
            let now = ProcessInfo.processInfo.systemUptime
            if rms > voiceRMSThreshold {
                lastVoicedAt = now
                voicedInSegment = true
            } else if voicedInSegment, now - lastVoicedAt >= silenceCutAfter {
                let current = writer.dataBytes
                let segmentSeconds = Double(current - tailStartByte) / Double(2 * Self.targetSampleRate)
                if segmentSeconds >= minSegmentSeconds,
                   let slice = try? WavWriter.sliceWav(fileURL: writer.url,
                                                       fromByte: tailStartByte, toByte: current) {
                    onSegment?(slice, segmentIndex)
                    segmentIndex += 1
                    tailStartByte = current
                    voicedInSegment = false
                }
            }
        }

        let ratio = Double(Self.targetSampleRate) / buffer.format.sampleRate
        let capacity = AVAudioFrameCount(Double(buffer.frameLength) * ratio + 32)
        guard let out = AVAudioPCMBuffer(pcmFormat: converter.outputFormat, frameCapacity: capacity) else { return }

        var fed = false
        var error: NSError?
        converter.convert(to: out, error: &error) { _, status in
            if fed {
                status.pointee = .noDataNow
                return nil
            }
            fed = true
            status.pointee = .haveData
            return buffer
        }
        if let error {
            Log.error("Audio conversion failed: \(error.localizedDescription)")
            return
        }
        if out.frameLength > 0, let samples = out.int16ChannelData?[0] {
            do {
                try writer.append(samples: samples, count: Int(out.frameLength))
            } catch {
                Log.error("WAV write failed: \(error.localizedDescription)")
            }
        }
    }
}
