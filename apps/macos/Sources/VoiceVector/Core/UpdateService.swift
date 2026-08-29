import Foundation
import AppKit
import Security

/// One-click in-app updates from GitHub Releases — no framework, no server:
/// check the public latest-release API, download the zip, swap the bundle in
/// place via a detached shell script, relaunch.
///
/// Reality check for ad-hoc-signed builds: replacing the binary changes its
/// signature, so macOS may require re-granting Accessibility after an update.
struct UpdateInfo: Equatable {
    let version: String
    let assetURL: URL
}

enum UpdateService {
    static let repo = "Sammons/voicevector"

    static var currentVersion: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0.0.0-dev"
    }

    /// Local `make app` builds are stamped 0.0.0-dev.
    static var isDevBuild: Bool { currentVersion.hasSuffix("-dev") }

    /// Returns the newest release if it's newer than what's running.
    static func fetchLatest() async throws -> UpdateInfo? {
        var request = URLRequest(url: URL(string: "https://api.github.com/repos/\(repo)/releases/latest")!)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        let json = try HTTP.json(try await HTTP.send(request))
        guard let tag = json["tag_name"] as? String,
              let assets = json["assets"] as? [[String: Any]],
              let asset = assets.first(where: { ($0["name"] as? String) == "VoiceVector-macos.zip" }),
              let urlString = asset["browser_download_url"] as? String,
              let assetURL = URL(string: urlString) else { return nil }
        let version = tag.hasPrefix("v") ? String(tag.dropFirst()) : tag
        guard isNewer(version, than: currentVersion) else { return nil }
        return UpdateInfo(version: version, assetURL: assetURL)
    }

    /// Semver-ish compare; a dev build is always update-eligible.
    static func isNewer(_ candidate: String, than current: String) -> Bool {
        if current.hasSuffix("-dev") { return true }
        let a = candidate.split(separator: ".").compactMap { Int($0) }
        let b = current.split(separator: ".").compactMap { Int($0) }
        for i in 0..<max(a.count, b.count) {
            let x = i < a.count ? a[i] : 0
            let y = i < b.count ? b[i] : 0
            if x != y { return x > y }
        }
        return false
    }

    /// Downloads, verifies, swaps, and relaunches. Only returns on failure.
    @MainActor
    static func downloadAndInstall(_ info: UpdateInfo) async throws {
        let appURL = Bundle.main.bundleURL
        guard appURL.pathExtension == "app" else {
            throw NSError(domain: "VoiceVector", code: 10, userInfo: [
                NSLocalizedDescriptionKey: "Not running from an app bundle — update manually.",
            ])
        }

        let (downloaded, _) = try await HTTP.session.download(from: info.assetURL)
        let workDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("vv-update-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: workDir, withIntermediateDirectories: true)
        try run("/usr/bin/ditto", ["-xk", downloaded.path, workDir.path])

        let newApp = workDir.appendingPathComponent("VoiceVector.app")
        guard FileManager.default.fileExists(atPath: newApp.appendingPathComponent("Contents/MacOS/VoiceVector").path) else {
            throw NSError(domain: "VoiceVector", code: 11, userInfo: [
                NSLocalizedDescriptionKey: "Downloaded update did not contain a valid app bundle.",
            ])
        }

        // Never install an update that isn't signed by the same team as this
        // build (defends the download path even if the transport were subverted).
        try verifySignature(of: newApp)

        // Swap after this process exits, then relaunch.
        let pid = ProcessInfo.processInfo.processIdentifier
        let script = """
        #!/bin/sh
        while kill -0 \(pid) 2>/dev/null; do sleep 0.2; done
        rm -rf "\(appURL.path)"
        /usr/bin/ditto "\(newApp.path)" "\(appURL.path)"
        /usr/bin/xattr -dr com.apple.quarantine "\(appURL.path)" 2>/dev/null
        /usr/bin/open "\(appURL.path)"
        rm -rf "\(workDir.path)"
        """
        let scriptURL = workDir.appendingPathComponent("update.sh")
        try script.write(to: scriptURL, atomically: true, encoding: .utf8)
        let swapper = Process()
        swapper.executableURL = URL(fileURLWithPath: "/bin/sh")
        swapper.arguments = [scriptURL.path]
        try swapper.run()
        Log.info("Updating to \(info.version); relaunching")
        NSApp.terminate(nil)
    }

    /// Requires a valid signature whose team matches the running app's team.
    private static func verifySignature(of app: URL) throws {
        var current: SecCode?
        var currentTeam: String?
        if SecCodeCopySelf([], &current) == errSecSuccess, let current {
            var staticCode: SecStaticCode?
            if SecCodeCopyStaticCode(current, [], &staticCode) == errSecSuccess, let staticCode {
                var info: CFDictionary?
                if SecCodeCopySigningInformation(staticCode, SecCSFlags(rawValue: kSecCSSigningInformation),
                                                 &info) == errSecSuccess,
                   let dict = info as? [String: Any] {
                    currentTeam = dict[kSecCodeInfoTeamIdentifier as String] as? String
                }
            }
        }
        guard let requiredTeam = currentTeam else {
            // Unsigned/ad-hoc dev build updating to a release: allow, but
            // require the download to carry a valid Developer ID signature.
            try run("/usr/bin/codesign", ["--verify", "--deep", "--strict", app.path])
            return
        }
        var newStatic: SecStaticCode?
        guard SecStaticCodeCreateWithPath(app as CFURL, [], &newStatic) == errSecSuccess,
              let newStatic else {
            throw NSError(domain: "VoiceVector", code: 13, userInfo: [
                NSLocalizedDescriptionKey: "Could not read the downloaded app's signature.",
            ])
        }
        guard SecStaticCodeCheckValidity(newStatic, SecCSFlags(rawValue: kSecCSCheckAllArchitectures), nil) == errSecSuccess else {
            throw NSError(domain: "VoiceVector", code: 14, userInfo: [
                NSLocalizedDescriptionKey: "Downloaded update has an invalid signature — refusing to install.",
            ])
        }
        var newInfo: CFDictionary?
        guard SecCodeCopySigningInformation(newStatic, SecCSFlags(rawValue: kSecCSSigningInformation),
                                            &newInfo) == errSecSuccess,
              let newDict = newInfo as? [String: Any],
              let newTeam = newDict[kSecCodeInfoTeamIdentifier as String] as? String,
              newTeam == requiredTeam else {
            throw NSError(domain: "VoiceVector", code: 15, userInfo: [
                NSLocalizedDescriptionKey: "Downloaded update is signed by a different team — refusing to install.",
            ])
        }
    }

    private static func run(_ tool: String, _ arguments: [String]) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: tool)
        process.arguments = arguments
        try process.run()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            throw NSError(domain: "VoiceVector", code: 12, userInfo: [
                NSLocalizedDescriptionKey: "\(tool) failed (\(process.terminationStatus))",
            ])
        }
    }
}
