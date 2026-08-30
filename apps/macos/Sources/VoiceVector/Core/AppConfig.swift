import Foundation

// MARK: - Provider profiles

enum ProviderKind: String, Codable, CaseIterable, Identifiable {
    case elevenLabs
    case fireworks
    case cerebras
    case vercelGateway
    case openAICompatible

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .elevenLabs: return "ElevenLabs"
        case .fireworks: return "Fireworks AI"
        case .cerebras: return "Cerebras"
        case .vercelGateway: return "Vercel AI Gateway"
        case .openAICompatible: return "OpenAI-compatible"
        }
    }

    /// Default base URL for the kind. Custom/self-hosted endpoints edit this.
    var defaultBaseURL: String {
        switch self {
        case .elevenLabs: return "https://api.elevenlabs.io"
        case .fireworks: return "https://api.fireworks.ai/inference/v1"
        case .cerebras: return "https://api.cerebras.ai/v1"
        case .vercelGateway: return "https://ai-gateway.vercel.sh/v1"
        case .openAICompatible: return "http://localhost:11434/v1"
        }
    }

    /// Fireworks removed audio inference in June 2026; Cerebras is LLM-only.
    var supportsTranscription: Bool { self != .fireworks && self != .cerebras }

    /// ElevenLabs is STT-only; the others expose chat completions for cleanup.
    var supportsChat: Bool { self != .elevenLabs }

    /// Whether GET /models is expected to work.
    var supportsModelListing: Bool { self != .elevenLabs }

    /// Whether the transcription call accepts a vocabulary hint (ElevenLabs
    /// `keyterms`, Whisper-style `prompt`). The gateway's STT protocol has no
    /// such field; vocabulary still guides cleanup everywhere.
    var supportsVocabulary: Bool { self == .elevenLabs || self == .openAICompatible }
}

/// One configured endpoint (account/server). API key lives in the Keychain,
/// keyed by the profile id — never in this file.
struct ProviderProfile: Codable, Identifiable, Equatable {
    var id: UUID = UUID()
    var kind: ProviderKind
    var name: String
    var baseURL: String
    var sttModel: String
    var chatModel: String

    static func preset(_ kind: ProviderKind) -> ProviderProfile {
        switch kind {
        case .elevenLabs:
            return ProviderProfile(kind: kind, name: "ElevenLabs", baseURL: kind.defaultBaseURL,
                                   sttModel: "scribe_v2", chatModel: "")
        case .fireworks:
            return ProviderProfile(kind: kind, name: "Fireworks", baseURL: kind.defaultBaseURL,
                                   sttModel: "",
                                   chatModel: "accounts/fireworks/models/gpt-oss-20b")
        case .cerebras:
            return ProviderProfile(kind: kind, name: "Cerebras", baseURL: kind.defaultBaseURL,
                                   sttModel: "", chatModel: "gpt-oss-120b")
        case .vercelGateway:
            return ProviderProfile(kind: kind, name: "Vercel AI Gateway", baseURL: kind.defaultBaseURL,
                                   sttModel: "openai/whisper-1",
                                   chatModel: "openai/gpt-4o-mini")
        case .openAICompatible:
            return ProviderProfile(kind: kind, name: "Self-hosted", baseURL: kind.defaultBaseURL,
                                   sttModel: "whisper-1", chatModel: "")
        }
    }
}

// MARK: - Hotkey

/// A recorded hotkey: either a modifier-only key (Right ⌥, Fn, …) tracked via
/// flagsChanged, or a regular key with required modifiers.
struct HotkeySpec: Codable, Equatable {
    var keyCode: UInt16
    /// CGEventFlags raw value of required modifiers (0 for modifier-only keys).
    var modifiers: UInt64
    var isModifierOnly: Bool

    /// Default: Right Option.
    static let `default` = HotkeySpec(keyCode: 61, modifiers: 0, isModifierOnly: true)

    /// A freshly added profile's placeholder. Key code 0 is a real key on
    /// macOS (the letter A), so "unset" is exactly this triple — the engine
    /// must never match it (it would swallow plain A).
    static let unset = HotkeySpec(keyCode: 0, modifiers: 0, isModifierOnly: false)
    var isSet: Bool { self != HotkeySpec.unset }
}

/// One hotkey + its cleanup policy. Anything not overridden inherits the
/// global cleanup settings; `cleanupEnabled = false` means raw transcript.
struct DictationProfile: Codable, Equatable, Identifiable {
    var id: UUID = UUID()
    var name: String = "Default"
    var hotkey: HotkeySpec = .default
    /// Legacy on/off switch, kept on the wire for older builds. Derived from
    /// `cleanupMode` whenever that is set.
    var cleanupEnabled: Bool = true
    /// Off / light / rich for this hotkey. nil = legacy profile: inherit the
    /// global mode (or off when `cleanupEnabled` is false).
    var cleanupMode: CleanupMode?
    /// nil = use the global cleanup provider.
    var cleanupProviderID: UUID?
    /// "" = use the global prompt (built-in or custom).
    var customPrompt: String = ""
    /// nil = use the global transcription provider.
    var sttProviderID: UUID?
    /// Extra vocabulary for this hotkey, appended to the global list.
    var vocabulary: String = ""
    /// Stage the cleaned text in the HUD instead of pasting: speak changes
    /// (revised by the review model), ⏎ pastes, Esc discards.
    var reviewBeforePaste: Bool = false
    /// nil = use the cleanup provider for revisions.
    var reviewProviderID: UUID?
    /// Attach a screenshot of the frontmost window to cleanup and review
    /// calls as context. Needs Screen Recording permission.
    var screenshotContext: Bool = false

    enum CodingKeys: String, CodingKey {
        case id, name, hotkey, cleanupEnabled, cleanupMode, cleanupProviderID, customPrompt
        case sttProviderID, vocabulary, reviewBeforePaste, reviewProviderID, screenshotContext
    }

    init() {}

    init(name: String, hotkey: HotkeySpec, cleanupMode: CleanupMode? = nil) {
        self.name = name
        self.hotkey = hotkey
        self.cleanupMode = cleanupMode
        if let cleanupMode { cleanupEnabled = cleanupMode != .off }
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decodeIfPresent(UUID.self, forKey: .id) ?? UUID()
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? "Default"
        hotkey = try c.decodeIfPresent(HotkeySpec.self, forKey: .hotkey) ?? .default
        cleanupEnabled = try c.decodeIfPresent(Bool.self, forKey: .cleanupEnabled) ?? true
        cleanupMode = try c.decodeIfPresent(CleanupMode.self, forKey: .cleanupMode)
        cleanupProviderID = try c.decodeIfPresent(UUID.self, forKey: .cleanupProviderID)
        customPrompt = try c.decodeIfPresent(String.self, forKey: .customPrompt) ?? ""
        sttProviderID = try c.decodeIfPresent(UUID.self, forKey: .sttProviderID)
        vocabulary = try c.decodeIfPresent(String.self, forKey: .vocabulary) ?? ""
        reviewBeforePaste = try c.decodeIfPresent(Bool.self, forKey: .reviewBeforePaste) ?? false
        reviewProviderID = try c.decodeIfPresent(UUID.self, forKey: .reviewProviderID)
        screenshotContext = try c.decodeIfPresent(Bool.self, forKey: .screenshotContext) ?? false
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(id, forKey: .id)
        try c.encode(name, forKey: .name)
        try c.encode(hotkey, forKey: .hotkey)
        try c.encode(cleanupMode.map { $0 != .off } ?? cleanupEnabled, forKey: .cleanupEnabled)
        try c.encodeIfPresent(cleanupMode, forKey: .cleanupMode)
        try c.encodeIfPresent(cleanupProviderID, forKey: .cleanupProviderID)
        try c.encode(customPrompt, forKey: .customPrompt)
        try c.encodeIfPresent(sttProviderID, forKey: .sttProviderID)
        try c.encode(vocabulary, forKey: .vocabulary)
        try c.encode(reviewBeforePaste, forKey: .reviewBeforePaste)
        try c.encodeIfPresent(reviewProviderID, forKey: .reviewProviderID)
        try c.encode(screenshotContext, forKey: .screenshotContext)
    }
}

enum TapStartMode: String, Codable, CaseIterable {
    /// Double-tap starts, single tap stops. Hold-to-talk always works.
    case doubleTap
    /// Single tap starts, single tap stops. Hold-to-talk always works.
    case singleTap

    var label: String {
        switch self {
        case .doubleTap: return "Double-tap to start"
        case .singleTap: return "Single tap to start"
        }
    }
}

// MARK: - Cleanup

enum CleanupMode: String, Codable, CaseIterable {
    case off
    /// Fix fillers/punctuation only; keep the wording.
    case light
    /// Light + dictated formatting (bullets, headings, paragraphs).
    case rich

    var label: String {
        switch self {
        case .off: return "Off (raw transcript)"
        case .light: return "Light (fillers & punctuation)"
        case .rich: return "Rich (formatting & bullets)"
        }
    }
}

struct CleanupConfig: Codable, Equatable {
    var mode: CleanupMode = .rich
    /// Profile used for the chat-completion cleanup call.
    var providerID: UUID?
    /// Comma/newline-separated words & names the models should get right.
    var vocabulary: String = ""
    /// Empty = use the built-in prompt for the selected mode; otherwise this
    /// replaces it (vocabulary is still appended).
    var customPrompt: String = ""

    init() {}

    /// Tolerant decoding so adding fields never resets an existing config.json.
    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        mode = try container.decodeIfPresent(CleanupMode.self, forKey: .mode) ?? .rich
        providerID = try container.decodeIfPresent(UUID.self, forKey: .providerID)
        vocabulary = try container.decodeIfPresent(String.self, forKey: .vocabulary) ?? ""
        customPrompt = try container.decodeIfPresent(String.self, forKey: .customPrompt) ?? ""
    }
}

// MARK: - Webhooks

struct WebhookConfig: Codable, Equatable {
    var url: String = ""
    var includeAudio: Bool = false
    var enabled: Bool = false
}

// MARK: - Root config

struct AppConfig: Codable, Equatable {
    var version: Int = 1
    var wizardCompleted: Bool = false

    /// Any number of hotkeys, each with its own cleanup policy. Always ≥1;
    /// the first is the default shown in the wizard/status line.
    var dictationProfiles: [DictationProfile] = [DictationProfile()]
    var tapStartMode: TapStartMode = .doubleTap

    var providers: [ProviderProfile] = []
    var sttProviderID: UUID?
    var cleanup = CleanupConfig()

    var playSounds: Bool = true
    /// Transcribe completed phrases during silence pauses (long dictations
    /// finish almost instantly at stop; cleanup still runs once at the end).
    var chunkedTranscription: Bool = true
    /// If false, transcripts are only copied to the clipboard, never auto-pasted.
    var autoPaste: Bool = true
    /// Use AppleScript System Events instead of CGEvent for the ⌘V synth —
    /// works around macOS 26 silently dropping events from ad-hoc-signed apps.
    var appleScriptPaste: Bool = false

    /// Folder new dictations land in.
    var activeFolder: String = "Inbox"
    /// Folder name → webhook. Folders themselves are directories on disk.
    var folderWebhooks: [String: WebhookConfig] = [:]

    /// Root of recordings/transcripts; `~` is expanded on use.
    var libraryPath: String = "~/Documents/VoiceVector"

    /// Opening an external audio interface takes ~0.5 s. Keep the input
    /// open for 15 s after a recording so back-to-back takes start instantly
    /// (the system's mic-in-use indicator stays on meanwhile).
    var keepMicWarmAfterRecording: Bool = true
    /// Keep the input open whenever the app is running — instant start,
    /// always; the mic-in-use indicator is always on.
    var keepMicAlwaysWarm: Bool = false

    var expandedLibraryURL: URL {
        URL(fileURLWithPath: (libraryPath as NSString).expandingTildeInPath, isDirectory: true)
    }

    /// The first profile's hotkey — wizard/status-line convenience.
    var primaryHotkey: HotkeySpec {
        get { dictationProfiles.first?.hotkey ?? .default }
        set {
            if dictationProfiles.isEmpty { dictationProfiles = [DictationProfile()] }
            dictationProfiles[0].hotkey = newValue
        }
    }

    init() {}

    /// Tolerant decoding so adding fields never resets a user's config.json.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        version = try c.decodeIfPresent(Int.self, forKey: .version) ?? 1
        wizardCompleted = try c.decodeIfPresent(Bool.self, forKey: .wizardCompleted) ?? false
        if let profiles = try c.decodeIfPresent([DictationProfile].self,
                                                forKey: .dictationProfiles),
           !profiles.isEmpty {
            dictationProfiles = profiles
        } else {
            // Migrate the legacy single-hotkey field.
            struct Legacy: Codable { var hotkey: HotkeySpec? }
            let legacy = try? Legacy(from: decoder)
            var profile = DictationProfile()
            profile.hotkey = legacy?.hotkey ?? .default
            dictationProfiles = [profile]
        }
        tapStartMode = try c.decodeIfPresent(TapStartMode.self, forKey: .tapStartMode) ?? .doubleTap
        providers = try c.decodeIfPresent([ProviderProfile].self, forKey: .providers) ?? []
        sttProviderID = try c.decodeIfPresent(UUID.self, forKey: .sttProviderID)
        cleanup = try c.decodeIfPresent(CleanupConfig.self, forKey: .cleanup) ?? CleanupConfig()
        playSounds = try c.decodeIfPresent(Bool.self, forKey: .playSounds) ?? true
        chunkedTranscription = try c.decodeIfPresent(Bool.self, forKey: .chunkedTranscription) ?? true
        autoPaste = try c.decodeIfPresent(Bool.self, forKey: .autoPaste) ?? true
        appleScriptPaste = try c.decodeIfPresent(Bool.self, forKey: .appleScriptPaste) ?? false
        activeFolder = try c.decodeIfPresent(String.self, forKey: .activeFolder) ?? "Inbox"
        folderWebhooks = try c.decodeIfPresent([String: WebhookConfig].self, forKey: .folderWebhooks) ?? [:]
        libraryPath = try c.decodeIfPresent(String.self, forKey: .libraryPath) ?? "~/Documents/VoiceVector"
        keepMicWarmAfterRecording = try c.decodeIfPresent(Bool.self, forKey: .keepMicWarmAfterRecording) ?? true
        keepMicAlwaysWarm = try c.decodeIfPresent(Bool.self, forKey: .keepMicAlwaysWarm) ?? false
    }
}

// MARK: - Store

/// Loads/saves config.json inside the library folder. Pretty-printed so the
/// user can read and edit it by hand.
final class ConfigStore {
    private(set) var config: AppConfig
    private let fileURL: URL

    /// Config lives in Application Support (it must be readable before we know
    /// the library path, which it contains).
    static var defaultURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("VoiceVector/config.json")
    }

    init(fileURL: URL = ConfigStore.defaultURL) {
        self.fileURL = fileURL
        if let data = try? Data(contentsOf: fileURL),
           let loaded = try? JSONDecoder().decode(AppConfig.self, from: data) {
            config = loaded
        } else {
            config = AppConfig()
        }
    }

    func update(_ mutate: (inout AppConfig) -> Void) {
        mutate(&config)
        save()
    }

    func save() {
        do {
            let dir = fileURL.deletingLastPathComponent()
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            let data = try encoder.encode(config)
            try data.write(to: fileURL, options: .atomic)
        } catch {
            Log.error("Failed to save config: \(error.localizedDescription)")
        }
    }
}
