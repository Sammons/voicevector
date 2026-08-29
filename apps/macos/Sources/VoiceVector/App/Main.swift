import AppKit

// SPM executable entry: no nib, no storyboard — everything is code.
@main
struct VoiceVectorMain {
    @MainActor
    static func main() {
        if CommandLine.arguments.contains("--self-test") {
            SelfTest.run()
        }
        if let index = CommandLine.arguments.firstIndex(of: "--probe-audio") {
            let seconds = index + 1 < CommandLine.arguments.count
                ? Double(CommandLine.arguments[index + 1]) ?? 4 : 4
            AudioProbe.run(seconds: seconds)
        }
        if let index = CommandLine.arguments.firstIndex(of: "--snapshot-ui") {
            let dir = index + 1 < CommandLine.arguments.count
                ? CommandLine.arguments[index + 1] : "/tmp/voicevector-ui"
            SnapshotUI.run(outputDir: dir)
        }
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.regular)
        app.run()
    }
}
