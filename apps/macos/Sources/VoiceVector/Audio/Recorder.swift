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
    /// Voice detection is relative to a tracked noise floor so quiet,
    /// un-boosted inputs (audio interfaces at conservative gain) still
    /// segment; anything above the absolute ceiling always counts.
    private let voiceRMSCeiling: Float = 0.015
    private let voiceRMSFloor: Float = 0.001
    private var noiseFloor: Float = 0.001
    /// HUD level on a fixed, quantized scale — deliberately simple: −60 dBFS
    /// is silence, −15 dBFS is full, crushed to 5 steps. A conservatively
    /// gained interface shows a bar or two, a hot built-in mic four or five,
    /// and room noise on either reads as flat.
    static func displayLevel(rms: Float) -> Float {
        let db = 20 * log10(max(rms, 1e-6))
        let normalized = min(1, max(0, (db + 60) / 45))
        return (normalized * 5).rounded(.up) / 5
    }

    private func meter(_ rms: Float) -> Float { Self.displayLevel(rms: rms) }

    /// Adaptive VAD: true when `rms` is clearly above the running noise floor.
    func isVoiced(_ rms: Float) -> Bool {
        // Floor tracks downward fast and drifts upward slowly.
        if rms < noiseFloor { noiseFloor = rms } else { noiseFloor = min(rms, noiseFloor * 1.02) }
        return rms > voiceRMSCeiling || (rms > voiceRMSFloor && rms > noiseFloor * 3)
    }

    private let engine = AVAudioEngine()
    /// Warm policy (see AppConfig.keepMicWarm*): external interfaces take
    /// ~0.5 s to open, so the engine can stay running between recordings.
    var warmAfterRecording = true
    var alwaysWarm = false
    private let warmIdleSeconds: TimeInterval = 15
    private var idleStop: DispatchWorkItem?

    /// Applies the warm policy while idle: opens the input for always-warm,
    /// or releases it when warming was turned off.
    func applyWarmPolicy() {
        queue.async { [self] in
            guard writer == nil else { return }
            if alwaysWarm {
                idleStop?.cancel(); idleStop = nil
                if !engine.isRunning {
                    engine.prepare()
                    do { try engine.start() } catch { Log.error("Mic warm-up failed: \(error.localizedDescription)") }
                }
            } else if !warmAfterRecording, engine.isRunning {
                idleStop?.cancel(); idleStop = nil
                engine.stop()
            }
        }
    }
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

    /// Opens the input device and starts capturing. Runs on the recorder
    /// queue because opening an external interface (USB audio, clock lock,
    /// high sample rates) can take a noticeable fraction of a second — the
    /// caller shows the HUD immediately and hears about failures via
    /// `completion` on the main thread. `stop()`/`discard()` serialize behind
    /// a start still in flight.
    func start(to url: URL, completion: @escaping (Error?) -> Void) {
        queue.async { [self] in
            do {
                try startOnQueue(to: url)
                DispatchQueue.main.async { completion(nil) }
            } catch {
                DispatchQueue.main.async { completion(error) }
            }
        }
    }

    private func startOnQueue(to url: URL) throws {
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
        noiseFloor = 0.001

        input.installTap(onBus: 0, bufferSize: 4096, format: inputFormat) { [weak self] buffer, _ in
            self?.queue.async { self?.process(buffer: buffer, converter: converter, writer: writer) }
        }

        idleStop?.cancel()
        idleStop = nil
        if !engine.isRunning {
            engine.prepare()
            try engine.start()
        }
    }

    /// Stops capture, closes the file. Returns duration in seconds.
    @discardableResult
    func stop() -> Double {
        var duration: Double = 0
        queue.sync { [self] in
            guard let writer else { return }
            engine.inputNode.removeTap(onBus: 0)
            self.writer = nil
            converter = nil
            level = 0
            duration = writer.finalize()
            startedAt = nil
            idleStop?.cancel()
            idleStop = nil
            if alwaysWarm {
                // Device stays open.
            } else if warmAfterRecording {
                // Leave the device open briefly; release it once clearly idle.
                let work = DispatchWorkItem { [weak self] in
                    guard let self, self.writer == nil, !self.alwaysWarm else { return }
                    self.engine.stop()
                }
                idleStop = work
                queue.asyncAfter(deadline: .now() + warmIdleSeconds, execute: work)
            } else {
                engine.stop()
            }
        }
        return duration
    }

    /// Stops and deletes the file (canceled/stray recordings).
    func discard() {
        var url: URL?
        queue.sync { url = writer?.url }
        guard let url else { return }
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
        let voiced = isVoiced(rms)
        if buffer.frameLength > 0 {
            // Smooth decay so the waveform breathes.
            level = max(meter(rms), level * 0.7)
        }

        // Silence-gap segmentation for streamed transcription.
        if chunking {
            let now = ProcessInfo.processInfo.systemUptime
            if voiced {
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
