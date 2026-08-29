import Foundation

/// Streaming WAV (RIFF, 16-bit PCM) writer. Header sizes are patched on
/// finalize; if the app dies mid-recording the file is still recoverable by
/// fixing the two size fields.
final class WavWriter {
    enum WavError: Error { case cannotOpen }

    let url: URL
    private let handle: FileHandle
    private let sampleRate: Int
    private let channels: Int
    private(set) var dataBytes: UInt32 = 0

    init(url: URL, sampleRate: Int, channels: Int = 1) throws {
        self.url = url
        self.sampleRate = sampleRate
        self.channels = channels
        FileManager.default.createFile(atPath: url.path, contents: nil)
        guard let handle = try? FileHandle(forWritingTo: url) else { throw WavError.cannotOpen }
        self.handle = handle
        try handle.write(contentsOf: Self.header(sampleRate: sampleRate, channels: channels, dataBytes: 0))
    }

    func append(samples: UnsafePointer<Int16>, count: Int) throws {
        let data = Data(bytes: samples, count: count * 2)
        try handle.write(contentsOf: data)
        dataBytes += UInt32(data.count)
    }

    /// Patches sizes and closes the file. Returns total duration in seconds.
    @discardableResult
    func finalize() -> Double {
        let header = Self.header(sampleRate: sampleRate, channels: channels, dataBytes: dataBytes)
        try? handle.seek(toOffset: 0)
        try? handle.write(contentsOf: header)
        try? handle.close()
        return Double(dataBytes) / Double(2 * channels * sampleRate)
    }

    /// In-memory silent WAV, used by the provider connection test to exercise
    /// the real speech-to-text endpoint (scoped API keys may not be allowed to
    /// call anything else).
    static func silentWav(seconds: Double, sampleRate: Int = 16_000) -> Data {
        let dataBytes = UInt32(Double(sampleRate) * seconds) * 2
        var data = header(sampleRate: sampleRate, channels: 1, dataBytes: dataBytes)
        data.append(Data(count: Int(dataBytes)))
        return data
    }

    /// Standalone WAV built from a byte range of an existing (possibly still
    /// growing) recording — used for streamed segment transcription.
    static func sliceWav(fileURL: URL, fromByte: UInt32, toByte: UInt32,
                         sampleRate: Int = WavWriter.defaultRate) throws -> Data {
        let handle = try FileHandle(forReadingFrom: fileURL)
        defer { try? handle.close() }
        try handle.seek(toOffset: UInt64(44 + fromByte))
        let payload = try handle.read(upToCount: Int(toByte - fromByte)) ?? Data()
        var data = header(sampleRate: sampleRate, channels: 1, dataBytes: UInt32(payload.count))
        data.append(payload)
        return data
    }

    static let defaultRate = 16_000

    private static func header(sampleRate: Int, channels: Int, dataBytes: UInt32) -> Data {
        var data = Data(capacity: 44)
        func put(_ string: String) { data.append(contentsOf: string.utf8) }
        func put32(_ value: UInt32) { withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) } }
        func put16(_ value: UInt16) { withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) } }

        let byteRate = UInt32(sampleRate * channels * 2)
        put("RIFF"); put32(36 + dataBytes); put("WAVE")
        put("fmt "); put32(16); put16(1) // PCM
        put16(UInt16(channels)); put32(UInt32(sampleRate)); put32(byteRate)
        put16(UInt16(channels * 2)); put16(16)
        put("data"); put32(dataBytes)
        return data
    }
}
