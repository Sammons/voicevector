import AVFoundation

/// `VoiceVector --probe-audio [seconds]` — prints the default input device's
/// format and per-channel levels, for diagnosing metering with external
/// interfaces. Uses the app's own microphone grant.
enum AudioProbe {
    static func run(seconds: Double) -> Never {
        let engine = AVAudioEngine()
        let input = engine.inputNode
        let format = input.inputFormat(forBus: 0)
        print("input format: \(format)")
        print("  sampleRate=\(format.sampleRate) channels=\(format.channelCount) "
              + "commonFormat=\(format.commonFormat.rawValue) interleaved=\(format.isInterleaved) "
              + "(1=float32 2=float64 3=int16 4=int32)")
        var buffers = 0
        input.installTap(onBus: 0, bufferSize: 4096, format: format) { buffer, _ in
            buffers += 1
            guard buffers % 4 == 0 else { return }
            let frames = Int(buffer.frameLength)
            let channels = Int(format.channelCount)
            var perChannel: [String] = []
            for c in 0..<channels {
                var sum: Float = 0
                if let data = buffer.floatChannelData {
                    let stride = format.isInterleaved ? channels : 1
                    let base = format.isInterleaved ? data[0] + c : data[c]
                    for i in 0..<frames { let v = base[i * stride]; sum += v * v }
                } else if let data = buffer.int32ChannelData {
                    let stride = format.isInterleaved ? channels : 1
                    let base = format.isInterleaved ? data[0] + c : data[c]
                    for i in 0..<frames { let v = Float(base[i * stride]) / 2_147_483_648; sum += v * v }
                } else if let data = buffer.int16ChannelData {
                    let stride = format.isInterleaved ? channels : 1
                    let base = format.isInterleaved ? data[0] + c : data[c]
                    for i in 0..<frames { let v = Float(base[i * stride]) / 32_768; sum += v * v }
                }
                perChannel.append(String(format: "%.5f", sqrt(sum / Float(max(frames, 1)))))
            }
            let meter = Recorder.sourceRMS(buffer)
            print("buffer \(buffers): frames=\(frames) rms/ch=\(perChannel) sourceRMS=\(String(format: "%.5f", meter)) level=\(String(format: "%.2f", Recorder.displayLevel(rms: meter, peak: 0.005, noise: 0.0005)))")
        }
        do {
            engine.prepare()
            try engine.start()
        } catch {
            print("engine start failed: \(error)")
            exit(1)
        }
        RunLoop.main.run(until: Date().addingTimeInterval(seconds))
        engine.stop()
        exit(0)
    }
}
