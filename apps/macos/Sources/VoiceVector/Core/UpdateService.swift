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

    /// Semver-ish compare; a dev build is always update-eligible. A
    /// pre-release suffix ("0.7.0-beta.1") is stripped before the numeric
    /// compare and ranks below the same version's stable release.
    static func isNewer(_ candidate: String, than current: String) -> Bool {
        if current.hasSuffix("-dev") { return true }
        func parse(_ v: String) -> (numbers: [Int], prerelease: Bool) {
            let parts = v.split(separator: "-", maxSplits: 1, omittingEmptySubsequences: false)
            let numbers = parts[0].split(separator: ".").map { Int($0) ?? 0 }
            return (numbers, parts.count > 1)
        }
        let a = parse(candidate), b = parse(current)
        for i in 0..<max(a.numbers.count, b.numbers.count) {
            let x = i < a.numbers.count ? a.numbers[i] : 0
            let y = i < b.numbers.count ? b.numbers[i] : 0
            if x != y { return x > y }
        }
        return b.prerelease && !a.prerelease
    }

    /// Downloads, verifies, and stages the swap: on return a detached script is
    /// waiting for this process to exit, after which it replaces the bundle and
    /// reopens it. The caller must then quit via `relaunch()`.
    @MainActor
    static func downloadAndInstall(_ info: UpdateInfo) async throws {
        let appURL = Bundle.main.bundleURL
        guard appURL.pathExtension == "app" else {
            throw NSError(domain: "VoiceVector", code: 10, userInfo: [
                NSLocalizedDescriptionKey: "Not running from an app bundle — update manually.",
            ])
        }

        // App Translocation mounts a quarantined app read-only at a random
        // path; the swap would silently fail and relaunch the old copy.
        if appURL.path.contains("/AppTranslocation/") {
            throw NSError(domain: "VoiceVector", code: 16, userInfo: [
                NSLocalizedDescriptionKey: "macOS is running VoiceVector from a quarantined location. Move it to Applications, relaunch, then update.",
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

        // Swap after this process exits, then relaunch. The wait is bounded:
        // if the app is somehow still alive after 20 s the script quits it
        // itself (TERM, then KILL), so an update can never leave the user
        // force-quitting by hand.
        // Copy first, then swap with a rollback: the old bundle is only
        // removed once the new one is in place, so a failed copy (disk full,
        // permissions) leaves the installed app intact.
        let pid = ProcessInfo.processInfo.processIdentifier
        let script = """
        #!/bin/sh
        app=\(shellQuote(appURL.path))
        new=\(shellQuote(newApp.path))
        work=\(shellQuote(workDir.path))
        n=0
        while kill -0 \(pid) 2>/dev/null; do
          n=$((n + 1))
          [ "$n" -eq 100 ] && kill -TERM \(pid) 2>/dev/null
          [ "$n" -eq 125 ] && kill -KILL \(pid) 2>/dev/null
          sleep 0.2
        done
        rm -rf "$app.new" "$app.old"
        if /usr/bin/ditto "$new" "$app.new"; then
          if mv "$app" "$app.old"; then
            if mv "$app.new" "$app"; then rm -rf "$app.old"; else mv "$app.old" "$app"; fi
          fi
          /usr/bin/xattr -dr com.apple.quarantine "$app" 2>/dev/null
        fi
        rm -rf "$app.new"
        /usr/bin/open "$app"
        rm -rf "$work"
        """
        let scriptURL = workDir.appendingPathComponent("update.sh")
        try script.write(to: scriptURL, atomically: true, encoding: .utf8)
        let swapper = Process()
        swapper.executableURL = URL(fileURLWithPath: "/bin/sh")
        swapper.arguments = [scriptURL.path]
        try swapper.run()
        Log.info("Updating to \(info.version); swapper staged, relaunching")
    }

    /// Quit so the staged swapper can replace the bundle and reopen it.
    ///
    /// `NSApp.terminate` is silently deferred while a SwiftUI `.sheet` is
    /// presented (AppKit waits for the sheet's modal session to end, which it
    /// never does), and the updater lives in the Settings sheet — so callers
    /// dismiss Settings and yield to the run loop before calling this. If
    /// terminate still returns, exit directly: nothing needs flushing, config
    /// saves are synchronous and the swapper only starts once this pid is gone.
    @MainActor
    static func relaunch() -> Never {
        NSApp.terminate(nil)
        Log.error("NSApp.terminate returned during update; exiting directly")
        exit(0)
    }

    /// Single-quote a path for /bin/sh so spaces, quotes and `$` are literal.
    private static func shellQuote(_ path: String) -> String {
        "'" + path.replacingOccurrences(of: "'", with: "'\\''") + "'"
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
