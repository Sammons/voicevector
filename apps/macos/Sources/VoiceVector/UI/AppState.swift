import Foundation
import SwiftUI
import AppKit

/// Central mutable state: config + dictation controller + hotkey engine.
@MainActor
final class AppState: ObservableObject {
    let configStore: ConfigStore
    let dictation: DictationController
    let hotkey: HotkeyEngine

    @Published var config: AppConfig {
        didSet { persist(oldValue: oldValue) }
    }
    @Published var showSettings = false
    @Published var accessibilityGranted = AXIsProcessTrusted()
    @Published var microphoneGranted = Recorder.permissionGranted

    init() {
        let store = ConfigStore()
        configStore = store
        config = store.config
        store.save() // materialize config.json on first launch so it's editable
        dictation = DictationController(configStore: store)
        hotkey = HotkeyEngine(profiles: store.config.dictationProfiles,
                              startMode: store.config.tapStartMode)

        hotkey.onAction = { [weak self] action, profileID in
            guard let self else { return }
            self.dictation.handle(action, profileID: profileID)
            self.hotkey.recordingActive = self.dictation.isRecording
        }
    }

    private func persist(oldValue: AppConfig) {
        configStore.update { $0 = config }
        if oldValue.dictationProfiles != config.dictationProfiles
            || oldValue.tapStartMode != config.tapStartMode {
            hotkey.reconfigure(profiles: config.dictationProfiles,
                               startMode: config.tapStartMode)
        }
        if oldValue.libraryPath != config.libraryPath {
            dictation.reloadLibraryRoot()
        }
        if oldValue.keepMicWarmAfterRecording != config.keepMicWarmAfterRecording
            || oldValue.keepMicAlwaysWarm != config.keepMicAlwaysWarm {
            dictation.applyWarmPolicy()
        }
    }

    func refreshPermissions() {
        accessibilityGranted = AXIsProcessTrusted()
        microphoneGranted = Recorder.permissionGranted
        if accessibilityGranted, !hotkey.isRunning {
            hotkey.start()
        }
        dictation.applyWarmPolicy()
    }

    /// Prompt for Accessibility (opens the system dialog once).
    func requestAccessibility() {
        let options = ["AXTrustedCheckOptionPrompt": true] as CFDictionary
        AXIsProcessTrustedWithOptions(options)
    }

    var hotkeyDescription: String { HotkeyEngine.describe(config.primaryHotkey) }

    // MARK: Provider helpers

    func addProvider(_ kind: ProviderKind) -> ProviderProfile {
        let profile = ProviderProfile.preset(kind)
        config.providers.append(profile)
        if config.sttProviderID == nil, kind.supportsTranscription {
            config.sttProviderID = profile.id
        }
        if config.cleanup.providerID == nil, kind.supportsChat {
            config.cleanup.providerID = profile.id
        }
        return profile
    }

    func removeProvider(_ profile: ProviderProfile) {
        Keychain.deleteAPIKey(for: profile.id)
        config.providers.removeAll { $0.id == profile.id }
        if config.sttProviderID == profile.id { config.sttProviderID = nil }
        if config.cleanup.providerID == profile.id { config.cleanup.providerID = nil }
    }
}
