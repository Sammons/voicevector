using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace VoiceVector.Shared
{
    // Mirrors docs/config-schema.md. JSON mapping is explicit and tolerant by
    // construction: missing keys keep defaults, unknown keys are ignored.

    public enum ProviderKind { ElevenLabs, Fireworks, Cerebras, VercelGateway, OpenAICompatible }

    public static class ProviderKindInfo
    {
        public static bool SupportsTranscription(this ProviderKind kind)
        {
            return kind != ProviderKind.Fireworks && kind != ProviderKind.Cerebras;
        }

        /// <summary>Whether the STT call accepts a vocabulary hint (ElevenLabs
        /// keyterms, Whisper-style prompt). Vocabulary guides cleanup regardless.</summary>
        public static bool SupportsVocabulary(this ProviderKind kind)
        {
            return kind == ProviderKind.ElevenLabs || kind == ProviderKind.OpenAICompatible;
        }

        public static bool SupportsChat(this ProviderKind kind)
        {
            return kind != ProviderKind.ElevenLabs;
        }

        public static bool SupportsModelListing(this ProviderKind kind)
        {
            return kind != ProviderKind.ElevenLabs;
        }

        public static string DisplayName(this ProviderKind kind)
        {
            switch (kind)
            {
                case ProviderKind.ElevenLabs: return "ElevenLabs";
                case ProviderKind.Fireworks: return "Fireworks AI";
                case ProviderKind.Cerebras: return "Cerebras";
                case ProviderKind.VercelGateway: return "Vercel AI Gateway";
                default: return "OpenAI-compatible";
            }
        }

        public static string DefaultBaseUrl(this ProviderKind kind)
        {
            switch (kind)
            {
                case ProviderKind.ElevenLabs: return "https://api.elevenlabs.io";
                case ProviderKind.Fireworks: return "https://api.fireworks.ai/inference/v1";
                case ProviderKind.Cerebras: return "https://api.cerebras.ai/v1";
                case ProviderKind.VercelGateway: return "https://ai-gateway.vercel.sh/v1";
                default: return "http://localhost:11434/v1";
            }
        }

        /// <summary>camelCase wire name, matching the JSON schema.</summary>
        public static string WireName(this ProviderKind kind)
        {
            var name = kind.ToString();
            return char.ToLowerInvariant(name[0]) + name.Substring(1);
        }

        public static ProviderKind KindFromWire(string wire)
        {
            foreach (ProviderKind kind in Enum.GetValues(typeof(ProviderKind)))
                if (kind.WireName() == wire) return kind;
            return ProviderKind.OpenAICompatible;
        }
    }

    public class ProviderProfile
    {
        public Guid Id = Guid.NewGuid();
        public ProviderKind Kind = ProviderKind.OpenAICompatible;
        public string Name = "";
        public string BaseUrl = "";
        public string SttModel = "";
        public string ChatModel = "";

        public static ProviderProfile Preset(ProviderKind kind)
        {
            var profile = new ProviderProfile { Kind = kind, BaseUrl = kind.DefaultBaseUrl() };
            switch (kind)
            {
                case ProviderKind.ElevenLabs:
                    profile.Name = "ElevenLabs"; profile.SttModel = "scribe_v2"; break;
                case ProviderKind.Fireworks:
                    profile.Name = "Fireworks";
                    profile.ChatModel = "accounts/fireworks/models/gpt-oss-20b"; break;
                case ProviderKind.Cerebras:
                    profile.Name = "Cerebras"; profile.ChatModel = "gpt-oss-120b"; break;
                case ProviderKind.VercelGateway:
                    profile.Name = "Vercel AI Gateway";
                    profile.SttModel = "openai/gpt-4o-mini-transcribe";
                    profile.ChatModel = "openai/gpt-4o-mini"; break;
                default:
                    profile.Name = "Self-hosted"; profile.SttModel = "whisper-1"; break;
            }
            return profile;
        }

        public Dictionary<string, object> ToJson()
        {
            return new Dictionary<string, object>
            {
                { "id", Id.ToString("D") },
                { "kind", Kind.WireName() },
                { "name", Name },
                { "baseUrl", BaseUrl },
                { "sttModel", SttModel },
                { "chatModel", ChatModel },
            };
        }

        public static ProviderProfile FromJson(Dictionary<string, object> d)
        {
            var profile = new ProviderProfile();
            Guid id;
            if (Guid.TryParse(Json.Str(d, "id"), out id)) profile.Id = id;
            profile.Kind = ProviderKindInfo.KindFromWire(Json.Str(d, "kind", "openAICompatible"));
            profile.Name = Json.Str(d, "name");
            profile.BaseUrl = Json.Str(d, "baseUrl");
            profile.SttModel = Json.Str(d, "sttModel");
            profile.ChatModel = Json.Str(d, "chatModel");
            return profile;
        }
    }

    public class HotkeySpec
    {
        /// <summary>Windows virtual-key code. Default 0xA5 = Right Alt.</summary>
        public int KeyCode = 0xA5;
        /// <summary>1=Ctrl, 2=Alt, 4=Shift, 8=Win; 0 for modifier-only keys.</summary>
        public int Modifiers = 0;
        public bool IsModifierOnly = true;

        public bool SameAs(HotkeySpec other)
        {
            return other != null && KeyCode == other.KeyCode
                   && Modifiers == other.Modifiers && IsModifierOnly == other.IsModifierOnly;
        }
    }

    /// <summary>One hotkey + its cleanup policy; unset pieces inherit the
    /// global cleanup settings. CleanupEnabled=false means raw transcript.</summary>
    public class DictationProfile
    {
        public Guid Id = Guid.NewGuid();
        public string Name = "Default";
        public HotkeySpec Hotkey = new HotkeySpec();
        /// <summary>Legacy on/off switch kept on the wire for older builds;
        /// derived from CleanupMode whenever that is set.</summary>
        public bool CleanupEnabled = true;
        /// <summary>Off/Light/Rich for this hotkey. null = legacy profile:
        /// inherit the global mode (or off when CleanupEnabled is false).</summary>
        public CleanupMode? CleanupMode;
        /// <summary>null = use the global cleanup provider.</summary>
        public Guid? CleanupProviderId;
        /// <summary>"" = use the global prompt.</summary>
        public string CustomPrompt = "";
        /// <summary>null = use the global transcription provider.</summary>
        public Guid? SttProviderId;
        /// <summary>Extra vocabulary for this hotkey, appended to the global list.</summary>
        public string Vocabulary = "";

        public Dictionary<string, object> ToJson()
        {
            return new Dictionary<string, object>
            {
                { "id", Id.ToString("D") },
                { "name", Name },
                { "hotkey", new Dictionary<string, object>
                    {
                        { "keyCode", (double)Hotkey.KeyCode },
                        { "modifiers", (double)Hotkey.Modifiers },
                        { "isModifierOnly", Hotkey.IsModifierOnly },
                    } },
                { "cleanupEnabled", CleanupMode.HasValue
                    ? CleanupMode.Value != Shared.CleanupMode.Off : CleanupEnabled },
                { "cleanupMode", CleanupMode.HasValue
                    ? CleanupMode.Value.ToString().ToLowerInvariant() : (object)null },
                { "cleanupProviderID", CleanupProviderId.HasValue
                    ? CleanupProviderId.Value.ToString("D") : (object)null },
                { "customPrompt", CustomPrompt },
                { "sttProviderID", SttProviderId.HasValue
                    ? SttProviderId.Value.ToString("D") : (object)null },
                { "vocabulary", Vocabulary },
            };
        }

        public static DictationProfile FromJson(Dictionary<string, object> d)
        {
            var profile = new DictationProfile();
            Guid id;
            if (Guid.TryParse(Json.Str(d, "id"), out id)) profile.Id = id;
            profile.Name = Json.Str(d, "name", "Default");
            var hotkey = Json.Obj(d, "hotkey");
            if (hotkey != null)
            {
                profile.Hotkey.KeyCode = (int)Json.Num(hotkey, "keyCode", 0xA5);
                profile.Hotkey.Modifiers = (int)Json.Num(hotkey, "modifiers", 0);
                profile.Hotkey.IsModifierOnly = Json.Bool(hotkey, "isModifierOnly", true);
            }
            profile.CleanupEnabled = Json.Bool(d, "cleanupEnabled", true);
            var modeName = Json.Str(d, "cleanupMode");
            if (modeName.Length > 0) profile.CleanupMode = AppConfig.ParseCleanupMode(modeName);
            Guid cp;
            if (Guid.TryParse(Json.Str(d, "cleanupProviderID"), out cp))
                profile.CleanupProviderId = cp;
            profile.CustomPrompt = Json.Str(d, "customPrompt");
            Guid sp;
            if (Guid.TryParse(Json.Str(d, "sttProviderID"), out sp)) profile.SttProviderId = sp;
            profile.Vocabulary = Json.Str(d, "vocabulary");
            return profile;
        }
    }

    public enum TapStartMode { DoubleTap, SingleTap }

    public enum CleanupMode { Off, Light, Rich }

    public class CleanupConfig
    {
        public CleanupMode Mode = CleanupMode.Rich;
        public Guid? ProviderId;
        public string Vocabulary = "";
        public string CustomPrompt = "";
    }

    public class WebhookConfig
    {
        public string Url = "";
        public bool IncludeAudio;
        public bool Enabled;
    }

    public class AppConfig
    {
        public int Version = 1;
        public bool WizardCompleted;
        /// <summary>Any number of hotkeys, each with a cleanup policy; always >= 1.</summary>
        public List<DictationProfile> DictationProfiles =
            new List<DictationProfile> { new DictationProfile() };
        public TapStartMode TapStartMode = TapStartMode.DoubleTap;

        public static CleanupMode ParseCleanupMode(string mode)
        {
            return mode == "off" ? CleanupMode.Off
                : mode == "light" ? CleanupMode.Light : CleanupMode.Rich;
        }

        public HotkeySpec PrimaryHotkey
        {
            get { return DictationProfiles.Count > 0 ? DictationProfiles[0].Hotkey : new HotkeySpec(); }
        }
        public List<ProviderProfile> Providers = new List<ProviderProfile>();
        public Guid? SttProviderId;
        public CleanupConfig Cleanup = new CleanupConfig();
        public bool PlaySounds = true;
        public bool ChunkedTranscription = true;
        public bool AutoPaste = true;
        public string ActiveFolder = "Inbox";
        public Dictionary<string, WebhookConfig> FolderWebhooks = new Dictionary<string, WebhookConfig>();
        public string LibraryPath = "~/Documents/VoiceVector";

        public string ExpandedLibraryPath
        {
            get
            {
                if (!LibraryPath.StartsWith("~")) return LibraryPath;
                var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
                return Path.Combine(home, LibraryPath.TrimStart('~', '/', '\\'));
            }
        }

        public static string DefaultPath
        {
            get
            {
                return Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "VoiceVector", "config.json");
            }
        }

        // -- JSON mapping ------------------------------------------------------

        public Dictionary<string, object> ToJson()
        {
            var webhooks = new Dictionary<string, object>();
            foreach (var pair in FolderWebhooks)
                webhooks[pair.Key] = new Dictionary<string, object>
                {
                    { "url", pair.Value.Url },
                    { "includeAudio", pair.Value.IncludeAudio },
                    { "enabled", pair.Value.Enabled },
                };
            return new Dictionary<string, object>
            {
                { "version", (double)Version },
                { "wizardCompleted", WizardCompleted },
                { "dictationProfiles",
                  DictationProfiles.Select(p => (object)p.ToJson()).ToList() },
                { "tapStartMode", TapStartMode == TapStartMode.DoubleTap ? "doubleTap" : "singleTap" },
                { "providers", Providers.Select(p => (object)p.ToJson()).ToList() },
                { "sttProviderID", SttProviderId.HasValue ? SttProviderId.Value.ToString("D") : (object)null },
                { "cleanup", new Dictionary<string, object>
                    {
                        { "mode", Cleanup.Mode.ToString().ToLowerInvariant() },
                        { "providerID", Cleanup.ProviderId.HasValue ? Cleanup.ProviderId.Value.ToString("D") : (object)null },
                        { "vocabulary", Cleanup.Vocabulary },
                        { "customPrompt", Cleanup.CustomPrompt },
                    } },
                { "playSounds", PlaySounds },
                { "chunkedTranscription", ChunkedTranscription },
                { "autoPaste", AutoPaste },
                { "activeFolder", ActiveFolder },
                { "folderWebhooks", webhooks },
                { "libraryPath", LibraryPath },
            };
        }

        public static AppConfig FromJson(Dictionary<string, object> d)
        {
            var config = new AppConfig();
            config.Version = (int)Json.Num(d, "version", 1);
            config.WizardCompleted = Json.Bool(d, "wizardCompleted", false);
            var profiles = Json.Arr(d, "dictationProfiles");
            if (profiles != null && profiles.Count > 0)
            {
                config.DictationProfiles.Clear();
                foreach (var item in profiles)
                {
                    var obj = item as Dictionary<string, object>;
                    if (obj != null) config.DictationProfiles.Add(DictationProfile.FromJson(obj));
                }
                if (config.DictationProfiles.Count == 0)
                    config.DictationProfiles.Add(new DictationProfile());
            }
            else
            {
                // Migrate the legacy single-hotkey field.
                var hotkey = Json.Obj(d, "hotkey");
                if (hotkey != null)
                {
                    var p0 = config.DictationProfiles[0];
                    p0.Hotkey.KeyCode = (int)Json.Num(hotkey, "keyCode", 0xA5);
                    p0.Hotkey.Modifiers = (int)Json.Num(hotkey, "modifiers", 0);
                    p0.Hotkey.IsModifierOnly = Json.Bool(hotkey, "isModifierOnly", true);
                }
            }
            config.TapStartMode = Json.Str(d, "tapStartMode", "doubleTap") == "singleTap"
                ? TapStartMode.SingleTap : TapStartMode.DoubleTap;
            var providers = Json.Arr(d, "providers");
            if (providers != null)
                foreach (var item in providers)
                    if (item is Dictionary<string, object> obj)
                        config.Providers.Add(ProviderProfile.FromJson(obj));
            Guid stt;
            if (Guid.TryParse(Json.Str(d, "sttProviderID"), out stt)) config.SttProviderId = stt;
            var cleanup = Json.Obj(d, "cleanup");
            if (cleanup != null)
            {
                config.Cleanup.Mode = ParseCleanupMode(Json.Str(cleanup, "mode", "rich"));
                Guid cp;
                if (Guid.TryParse(Json.Str(cleanup, "providerID"), out cp)) config.Cleanup.ProviderId = cp;
                config.Cleanup.Vocabulary = Json.Str(cleanup, "vocabulary");
                config.Cleanup.CustomPrompt = Json.Str(cleanup, "customPrompt");
            }
            config.PlaySounds = Json.Bool(d, "playSounds", true);
            config.ChunkedTranscription = Json.Bool(d, "chunkedTranscription", true);
            config.AutoPaste = Json.Bool(d, "autoPaste", true);
            config.ActiveFolder = Json.Str(d, "activeFolder", "Inbox");
            var webhooks = Json.Obj(d, "folderWebhooks");
            if (webhooks != null)
                foreach (var pair in webhooks)
                    if (pair.Value is Dictionary<string, object> w)
                        config.FolderWebhooks[pair.Key] = new WebhookConfig
                        {
                            Url = Json.Str(w, "url"),
                            IncludeAudio = Json.Bool(w, "includeAudio", false),
                            Enabled = Json.Bool(w, "enabled", false),
                        };
            config.LibraryPath = Json.Str(d, "libraryPath", "~/Documents/VoiceVector");
            return config;
        }

        public static AppConfig Load(string path = null)
        {
            path = path ?? DefaultPath;
            try
            {
                if (File.Exists(path))
                    return FromJson(Json.ParseObject(File.ReadAllText(path)));
            }
            catch (Exception e)
            {
                Log.Error("Config load failed, using defaults: " + e.Message);
            }
            return new AppConfig();
        }

        public void Save(string path = null)
        {
            path = path ?? DefaultPath;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                File.WriteAllText(path, Json.Write(ToJson(), indented: true));
            }
            catch (Exception e)
            {
                Log.Error("Config save failed: " + e.Message);
            }
        }
    }

    /// <summary>In-memory error ring surfaced in Settings.</summary>
    public static class Log
    {
        private static readonly object Guard = new object();
        private static readonly List<string> Ring = new List<string>();

        public static event Action<string> OnError;

        public static void Error(string message)
        {
            lock (Guard)
            {
                Ring.Add(DateTime.Now.ToString("HH:mm:ss") + "  " + message);
                if (Ring.Count > 200) Ring.RemoveAt(0);
            }
            var handler = OnError;
            if (handler != null) handler(message);
        }

        public static IList<string> RecentErrors
        {
            get { lock (Guard) return Ring.ToArray(); }
        }
    }
}
