using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using VoiceVector.Shared;

// Dependency-free test harness, mirroring the macOS app's --self-test.
// Compiles the same Shared/ sources the net48 app ships (LangVersion 7.3),
// runnable anywhere dotnet runs.

namespace VoiceVector.SelfTest
{
    public static class Program
    {
        private static readonly List<string> Failures = new List<string>();
        private static int _count;

        private static void Expect(bool condition, string label)
        {
            _count++;
            if (!condition) Failures.Add(label);
        }

        private static bool Seq(TapStateMachine.Act[] actual, params TapStateMachine.Act[] expected)
        {
            return actual.SequenceEqual(expected);
        }

        public static int Main()
        {
            TestJson();
            TestTapStateMachine();
            TestMarkdown();
            TestLibraryFiles();
            TestConfig();
            TestProfiles();
            TestCleanup();
            TestWav();
            TestWebhook();

            if (Failures.Count == 0)
            {
                Console.WriteLine("SELF-TEST PASSED (" + _count + " assertions)");
                return 0;
            }
            Console.WriteLine("SELF-TEST FAILED — " + Failures.Count + "/" + _count + " assertions failed:");
            foreach (var failure in Failures) Console.WriteLine("  x " + failure);
            return 1;
        }

        private static void TestJson()
        {
            var round = Json.ParseObject(Json.Write(new Dictionary<string, object>
            {
                { "s", "he\"llo\nworld" },
                { "n", 12.5 },
                { "i", 42.0 },
                { "b", true },
                { "z", null },
                { "arr", new List<object> { 1.0, "two", false } },
                { "obj", new Dictionary<string, object> { { "k", "v" } } },
            }, indented: true));
            Expect(Json.Str(round, "s") == "he\"llo\nworld", "json: string escaping round trip");
            Expect(Json.Num(round, "n", 0) == 12.5, "json: double round trip");
            Expect(Json.Num(round, "i", 0) == 42.0, "json: integer round trip");
            Expect(Json.Bool(round, "b", false), "json: bool round trip");
            Expect(round.ContainsKey("z") && round["z"] == null, "json: null round trip");
            Expect(Json.Arr(round, "arr").Count == 3, "json: array round trip");
            Expect(Json.Str(Json.Obj(round, "obj"), "k") == "v", "json: nested object");
            Expect(Json.Str(Json.ParseObject("{\"u\":\"\\u0041\\t\"}"), "u") == "A\t", "json: unicode escape");
            Expect((double)Json.Parse("-1.5e2") == -150.0, "json: scientific notation");
            bool threw = false;
            try { Json.Parse("{\"a\":}"); } catch (FormatException) { threw = true; }
            Expect(threw, "json: malformed input throws");
        }

        private static void TestTapStateMachine()
        {
            var machine = new TapStateMachine(TapStartMode.DoubleTap);
            Expect(Seq(machine.KeyDown(0), TapStateMachine.Act.StartRecording), "hold: down starts recording");
            Expect(Seq(machine.KeyUp(1.0), TapStateMachine.Act.Commit), "hold: long release commits");
            Expect(machine.Phase == TapStateMachine.PhaseKind.Idle, "hold: back to idle");

            machine = new TapStateMachine(TapStartMode.DoubleTap);
            Expect(Seq(machine.KeyDown(0), TapStateMachine.Act.StartRecording), "double: first down records");
            Expect(machine.KeyUp(0.1).Length == 0, "double: first short tap waits");
            Expect(machine.KeyDown(0.2).Length == 0, "double: second tap continues");
            Expect(machine.KeyUp(0.3).Length == 0, "double: latched after second tap");
            Expect(machine.Phase == TapStateMachine.PhaseKind.Latched, "double: latched phase");
            Expect(Seq(machine.KeyDown(5.0), TapStateMachine.Act.Commit), "double: stop tap commits");
            Expect(machine.KeyUp(5.1).Length == 0, "double: drain up");
            Expect(machine.Phase == TapStateMachine.PhaseKind.Idle, "double: idle after stop");

            machine = new TapStateMachine(TapStartMode.DoubleTap);
            machine.KeyDown(0); machine.KeyUp(0.1);
            Expect(machine.PendingDeadline != null, "stray: deadline pending");
            Expect(Seq(machine.Expire(0.6), TapStateMachine.Act.Discard), "stray: expiry discards");

            machine = new TapStateMachine(TapStartMode.SingleTap);
            Expect(Seq(machine.KeyDown(0), TapStateMachine.Act.StartRecording), "single: down records");
            Expect(machine.KeyUp(0.1).Length == 0, "single: quick release latches");
            Expect(machine.Phase == TapStateMachine.PhaseKind.Latched, "single: latched");
            Expect(Seq(machine.KeyDown(2.0), TapStateMachine.Act.Commit), "single: next tap commits");

            machine = new TapStateMachine(TapStartMode.DoubleTap);
            machine.KeyDown(0); machine.KeyUp(0.1); machine.KeyDown(0.2);
            Expect(Seq(machine.KeyUp(1.5), TapStateMachine.Act.Commit), "double+hold: commits on release");

            machine = new TapStateMachine(TapStartMode.DoubleTap);
            machine.KeyDown(0);
            Expect(Seq(machine.Cancel(), TapStateMachine.Act.Discard), "cancel discards");
        }

        private static void TestMarkdown()
        {
            var entry = new Entry
            {
                Id = "20260825-120000",
                Folder = "Inbox",
                Date = DateTimeOffset.Parse("2026-08-25T12:00:00Z"),
                Duration = 12.4,
                SttLabel = "ElevenLabs/scribe_v2",
                CleanupLabel = "Fireworks/gpt-oss-20b",
                Status = "complete",
                Cleaned = "Hello world.\n\n- bullet one\n- bullet two",
                Raw = "um hello world uh bullet one bullet two",
            };
            var parsed = Library.Parse(Library.Render(entry), entry.Id, entry.Folder);
            Expect(parsed.Cleaned == entry.Cleaned, "markdown: cleaned round trip");
            Expect(parsed.Raw == entry.Raw, "markdown: raw round trip");
            Expect(Math.Abs(parsed.Duration - 12.4) < 0.01, "markdown: duration");
            Expect(parsed.SttLabel == entry.SttLabel, "markdown: stt label");
            Expect(parsed.Date == entry.Date, "markdown: date");

            const string macRendered = "---\ndate: 2026-08-25T12:00:00Z\nduration: 12.4\naudio: 20260825-120000.wav\nstt: ElevenLabs/scribe_v2\ncleanup: Fireworks/gpt-oss-20b\nstatus: complete\n---\n\nHello world.\n\n- bullet one\n- bullet two\n\n## Raw transcript\n\num hello world uh bullet one bullet two\n";
            Expect(Library.Render(entry) == macRendered, "markdown: byte-identical to macOS rendering");

            var shot = Library.Parse(macRendered, entry.Id, entry.Folder);
            shot.Attach(new ScreenshotSet { Images = { new byte[] { 1 }, new byte[] { 2 } }, ActiveIndex = 0, Outlined = true });
            var shotRendered = Library.Render(shot);
            Expect(shotRendered.Contains("status: complete\nscreenshots: 2\nactiveScreenshot: 1\nscreenshotOutline: true\n---\n"),
                   "markdown: screenshot keys rendered after status");
            var shotParsed = Library.Parse(shotRendered, entry.Id, entry.Folder);
            Expect(shotParsed.Screenshots == 2 && shotParsed.ActiveScreenshot == 1 && shotParsed.ScreenshotOutline,
                   "markdown: screenshot keys round trip");

            var bare = Library.Parse("---\ndate: 2026-08-25T12:00:00Z\nstatus: complete\n---\n\nJust text\n", "x", "Inbox");
            Expect(bare.Cleaned == "Just text" && bare.Raw == "Just text", "markdown: bare body");
        }

        private static void TestLibraryFiles()
        {
            var root = Path.Combine(Path.GetTempPath(), "vv-test-" + Guid.NewGuid());
            try
            {
                var library = new Library(root);
                Expect(library.FolderNames().SequenceEqual(new[] { "Inbox" }), "library: Inbox auto-created");
                library.CreateFolder("Work Notes");
                Expect(library.FolderNames().SequenceEqual(new[] { "Inbox", "Work Notes" }),
                       "library: folder created");

                var slot = library.NewEntrySlot("Inbox");
                var entry = new Entry
                {
                    Id = slot.Key, Folder = "Inbox", Date = DateTimeOffset.Now,
                    Duration = 1, Cleaned = "hi", Raw = "hi",
                };
                library.Save(entry);
                Expect(library.EntryCount("Inbox") == 1, "library: entry saved");
                Expect(library.Entries("Inbox", 0, 10)[0].Cleaned == "hi", "library: entry listed");
                entry.Cleaned = "updated";
                library.Save(entry);
                Expect(library.GetEntry("Inbox", entry.Id).Cleaned == "updated", "library: entry updated");
                var set = new ScreenshotSet { Images = { new byte[] { 0xFF, 0xD8, 1 }, new byte[] { 0xFF, 0xD8, 2 } }, ActiveIndex = 1 };
                library.SaveScreenshots(entry.Id, "Inbox", set);
                entry.Attach(set);
                library.Save(entry);
                var loaded = library.LoadScreenshots(library.GetEntry("Inbox", entry.Id));
                Expect(loaded != null && loaded.Images.Count == 2 && loaded.Images[1][2] == 2 && loaded.ActiveIndex == 1 && !loaded.Outlined,
                       "library: screenshots saved beside the entry and reloaded");
                Expect(loaded.Attachments()[1].Caption == "Display 2 of 2 — ACTIVE: the dictated text will be inserted here."
                       && loaded.Attachments()[0].Caption == "Display 1 of 2.", "library: reloaded captions");
                Expect(library.EntryCount("Inbox") == 1, "library: screenshot files are not entries");
                library.Delete(entry);
                Expect(library.EntryCount("Inbox") == 0, "library: entry deleted");
                Expect(!File.Exists(Path.Combine(library.FolderPath("Inbox"), entry.ScreenshotFilename(1))),
                       "library: screenshots deleted with the entry");
            }
            finally
            {
                try { Directory.Delete(root, true); } catch { }
            }
        }

        private static void TestConfig()
        {
            var config = new AppConfig();
            config.Providers.Add(ProviderProfile.Preset(ProviderKind.ElevenLabs));
            config.Providers.Add(ProviderProfile.Preset(ProviderKind.VercelGateway));
            config.SttProviderId = config.Providers[0].Id;
            config.Cleanup.ProviderId = config.Providers[1].Id;
            config.Cleanup.CustomPrompt = "My prompt";
            config.FolderWebhooks["Inbox"] = new WebhookConfig
            {
                Url = "https://x.test/h", IncludeAudio = true, Enabled = true,
            };
            var decoded = AppConfig.FromJson(Json.ParseObject(Json.Write(config.ToJson(), indented: true)));
            Expect(decoded.Providers.Count == 2, "config: providers round trip");
            Expect(decoded.SttProviderId == config.SttProviderId, "config: stt provider id");
            Expect(decoded.Cleanup.ProviderId == config.Cleanup.ProviderId, "config: cleanup provider id");
            Expect(decoded.Cleanup.CustomPrompt == "My prompt", "config: custom prompt");
            Expect(decoded.FolderWebhooks["Inbox"].IncludeAudio, "config: webhook round trip");
            Expect(decoded.Providers[0].Kind == ProviderKind.ElevenLabs, "config: kind wire names");

            var partial = AppConfig.FromJson(Json.ParseObject("{\"cleanup\":{\"mode\":\"light\"},\"unknown\":1}"));
            Expect(partial.Cleanup.Mode == CleanupMode.Light, "config: tolerant partial decode");
            Expect(partial.PlaySounds && partial.ChunkedTranscription, "config: defaults kept");
            Expect(!ProviderKind.Fireworks.SupportsTranscription(), "config: fireworks is chat-only");
            Expect(!ProviderKind.Cerebras.SupportsTranscription(), "config: cerebras is chat-only");
            Expect(ProviderKind.VercelGateway.SupportsTranscription()
                   && ProviderKind.VercelGateway.SupportsChat(), "config: gateway does both");
            Expect(ProviderKind.ElevenLabs.SupportsVocabulary() && ProviderKind.OpenAICompatible.SupportsVocabulary()
                   && !ProviderKind.VercelGateway.SupportsVocabulary(),
                   "providers: vocabulary support matches the STT calls that send it");
        }

        private static void TestProfiles()
        {
            // Legacy single-hotkey config migrates into profile[0].
            var legacy = AppConfig.FromJson(Json.ParseObject(
                "{\"hotkey\":{\"keyCode\":123,\"modifiers\":3,\"isModifierOnly\":false}}"));
            Expect(legacy.DictationProfiles.Count == 1, "profiles: legacy migrates to one profile");
            Expect(legacy.PrimaryHotkey.KeyCode == 123 && legacy.PrimaryHotkey.Modifiers == 3
                   && !legacy.PrimaryHotkey.IsModifierOnly, "profiles: legacy hotkey preserved");

            // Profiles round-trip through JSON (matching the macOS wire keys).
            var config = new AppConfig();
            config.Providers.Add(ProviderProfile.Preset(ProviderKind.Cerebras));
            config.DictationProfiles.Add(new DictationProfile
            {
                Name = "Raw",
                Hotkey = new HotkeySpec { KeyCode = 0x77, Modifiers = 0, IsModifierOnly = false },
                CleanupEnabled = false,
                CleanupProviderId = config.Providers[0].Id,
                CustomPrompt = "Terse.",
            });
            var decoded = AppConfig.FromJson(Json.ParseObject(Json.Write(config.ToJson(), indented: true)));
            Expect(decoded.DictationProfiles.Count == 2, "profiles: round trip count");
            var raw = decoded.DictationProfiles[1];
            Expect(raw.Id == config.DictationProfiles[1].Id, "profiles: id round trip");
            Expect(raw.Name == "Raw" && !raw.CleanupEnabled, "profiles: fields round trip");
            Expect(raw.CleanupProviderId == config.Providers[0].Id, "profiles: provider id round trip");
            Expect(raw.CustomPrompt == "Terse." && raw.Hotkey.KeyCode == 0x77,
                   "profiles: prompt + hotkey round trip");

            // Effective() resolution against the globals.
            config.Cleanup.Mode = CleanupMode.Rich;
            config.Cleanup.ProviderId = null;
            var effectiveRaw = CleanupEngine.Effective(raw, decoded);
            Expect(!effectiveRaw.Enabled, "profiles: cleanupEnabled=false disables cleanup");
            var withProvider = new DictationProfile { CleanupProviderId = decoded.Providers[0].Id };
            var effectiveProvider = CleanupEngine.Effective(withProvider, decoded);
            Expect(effectiveProvider.Enabled
                   && effectiveProvider.Provider != null
                   && effectiveProvider.Provider.Id == decoded.Providers[0].Id,
                   "profiles: profile provider overrides global");
            var withPrompt = new DictationProfile { CustomPrompt = "Override." };
            Expect(CleanupEngine.Effective(withPrompt, decoded).Config.CustomPrompt == "Override.",
                   "profiles: profile prompt overrides global");
            var defaults = CleanupEngine.Effective(new DictationProfile(), decoded);
            Expect(defaults.Enabled && defaults.Config.CustomPrompt == decoded.Cleanup.CustomPrompt,
                   "profiles: default profile inherits globals");
            decoded.Cleanup.Mode = CleanupMode.Off;
            Expect(!CleanupEngine.Effective(new DictationProfile(), decoded).Enabled,
                   "profiles: legacy profile inherits global mode");
            var light = new DictationProfile { CleanupMode = CleanupMode.Light };
            Expect(CleanupEngine.Effective(light, decoded).Config.Mode == CleanupMode.Light,
                   "profiles: explicit mode wins over global off");
            decoded.Cleanup.Mode = CleanupMode.Rich;
            Expect(!CleanupEngine.Effective(new DictationProfile { CleanupEnabled = false }, decoded).Enabled,
                   "profiles: legacy cleanupEnabled=false forces raw");
            var offProfile = new DictationProfile { CleanupMode = CleanupMode.Off };
            var offJson = offProfile.ToJson();
            Expect(Json.Str(offJson, "cleanupMode") == "off" && !Json.Bool(offJson, "cleanupEnabled", true),
                   "profiles: off mode round-trips and mirrors legacy cleanupEnabled");
            Expect(DictationProfile.FromJson(offJson).CleanupMode == CleanupMode.Off,
                   "profiles: cleanupMode parses");

            var sttA = ProviderProfile.Preset(ProviderKind.ElevenLabs);
            var sttB = ProviderProfile.Preset(ProviderKind.VercelGateway);
            decoded.Providers.Add(sttA);
            decoded.Providers.Add(sttB);
            decoded.SttProviderId = sttA.Id;
            decoded.Cleanup.Vocabulary = "Luna";
            var code = new DictationProfile
            {
                CleanupMode = CleanupMode.Light, SttProviderId = sttB.Id, Vocabulary = "OrbStack, SwiftPM",
            };
            var codePolicy = CleanupEngine.Effective(code, decoded);
            Expect(codePolicy.Stt != null && codePolicy.Stt.Id == sttB.Id,
                   "profiles: transcriber override resolves");
            Expect(codePolicy.Config.Vocabulary == "Luna, OrbStack, SwiftPM",
                   "profiles: vocabulary merges global + hotkey");
            var inherited = CleanupEngine.Effective(new DictationProfile(), decoded);
            Expect(inherited.Stt != null && inherited.Stt.Id == sttA.Id,
                   "profiles: default transcriber inherited");
            Expect(CleanupEngine.MergeVocabulary("", " a, b ") == "a, b"
                   && CleanupEngine.MergeVocabulary("x", "") == "x",
                   "profiles: vocabulary merge edge cases");
            var codeRound = DictationProfile.FromJson(code.ToJson());
            Expect(codeRound.SttProviderId == sttB.Id && codeRound.Vocabulary == "OrbStack, SwiftPM",
                   "profiles: transcriber + vocabulary round trip");
        }

        private static void TestCleanup()
        {
            Expect(CleanupEngine.ParseVocabulary("Luna, VoiceVector\n OrbStack ,,\n")
                       .SequenceEqual(new[] { "Luna", "VoiceVector", "OrbStack" }),
                   "cleanup: vocabulary parsing");

            var config = new CleanupConfig();
            Expect(CleanupEngine.SystemPrompt(config) == CleanupEngine.DefaultPrompt(CleanupMode.Rich),
                   "cleanup: default prompt used when no custom");
            config.CustomPrompt = "My own prompt.";
            config.Vocabulary = "Luna";
            var custom = CleanupEngine.SystemPrompt(config);
            Expect(custom.StartsWith("My own prompt.") && custom.Contains("Luna"),
                   "cleanup: custom prompt + vocab");
            Expect(CleanupEngine.PostProcess("```md\nHi\n```", "raw") == "Hi", "cleanup: fence stripping");
            Expect(CleanupEngine.PostProcess("<transcript>\nHi\n</transcript>", "raw") == "Hi",
                   "cleanup: delimiter stripping");
            Expect(CleanupEngine.PostProcess("  ", "raw") == "raw", "cleanup: empty reply keeps raw");
            Expect(CleanupEngine.WrapTranscript("x") == "<transcript>\nx\n</transcript>",
                   "cleanup: transcript wrapping");
            Expect(CleanupEngine.ReviewMessage("Hi", "shorter")
                   == "<draft>\nHi\n</draft>\n<instruction>\nshorter\n</instruction>",
                   "review: message wraps draft and instruction");
            byte[] fpC, fpS;
            using (var sha = System.Security.Cryptography.SHA256.Create())
            {
                fpC = sha.ComputeHash(System.Text.Encoding.UTF8.GetBytes("client-cert"));
                fpS = sha.ComputeHash(System.Text.Encoding.UTF8.GetBytes("server-cert"));
            }
            var nonceC = Enumerable.Repeat((byte)1, 32).ToArray();
            var nonceS = Enumerable.Repeat((byte)2, 32).ToArray();
            Expect(PeerCrypto.PairingCode(fpC, fpS, nonceC, nonceS) == "636241",
                   "peer: pairing code test vector");
            var framed = PeerCrypto.Frame(new Dictionary<string, object> { { "t", "hello" }, { "name", "win" } });
            var buffer = new List<byte>(framed) { 9, 9 };
            int consumed;
            var parsed = PeerCrypto.ParseFrame(buffer, out consumed);
            Expect(parsed != null && Json.Str(parsed, "t") == "hello" && consumed == framed.Length,
                   "peer: frame round trip leaves trailing bytes");
            int none;
            Expect(PeerCrypto.ParseFrame(new List<byte> { 0, 0 }, out none) == null && none == 0,
                   "peer: incomplete frame is null");
            int bad;
            Expect(PeerCrypto.ParseFrame(new List<byte> { 0xFF, 0xFF, 0xFF, 0xFF }, out bad) == null && bad == -1,
                   "peer: oversized frame rejected");
            Expect(PeerCrypto.ToHex(PeerCrypto.FromHex("ab01")) == "ab01", "peer: hex round trip");
            var verdict = CleanupEngine.ParseRouterVerdict("Sure: {\"machine\": \"win\", \"window\": 42}");
            Expect(verdict != null && verdict.Machine == "win" && verdict.Window == 42,
                   "router: verdict parsed out of prose");
            var bare2 = CleanupEngine.ParseRouterVerdict("{\"machine\":\"m\"}");
            Expect(bare2 != null && bare2.Window == 0, "router: missing window is 0");
            Expect(CleanupEngine.ParseRouterVerdict("no json here") == null, "router: garbage is null");
            var routerMsg = CleanupEngine.RouterMessage("hi",
                new List<Tuple<string, bool, string>> { Tuple.Create("win", true, "1: A — B") });
            Expect(routerMsg.Contains("<draft>\nhi\n</draft>")
                   && routerMsg.Contains("(current: the user dictated here)"), "router: message shape");
            var mmJson = Json.ParseObject("{\"peers\":[{\"name\":\"x\",\"fingerprint\":\"ab\"}]}");
            var mm = MultiMachineConfig.FromJson(mmJson);
            Expect(!mm.Enabled && mm.Port == 47800 && mm.Peers.Count == 1
                   && mm.Peers[0].Name == "x" && !mm.Peers[0].AllowDeliver,
                   "peer: tolerant config decoding");
            var mmBack = MultiMachineConfig.FromJson(mm.ToJson());
            Expect(mmBack.Peers.Count == 1 && mmBack.Peers[0].Fingerprint == "ab",
                   "peer: config round trip");

            var caps = new ScreenshotSet { Images = { new byte[] { 1 }, new byte[] { 2 } }, ActiveIndex = 0, Outlined = true }.Attachments();
            Expect(caps[0].Caption == "Display 1 of 2 — ACTIVE: the dictated text will be inserted here; the target window is outlined in red."
                   && caps[1].Caption == "Display 2 of 2.", "screenshot: per-display captions");
            Expect(new ScreenshotSet { Images = { new byte[] { 1 } } }.Attachments()[0].Caption
                   == "Display 1 of 1 (which display is active is not known on this desktop).", "screenshot: unknown-active caption");
            Expect(CleanupEngine.PostProcess("<draft>\nHello\n</draft>", "x") == "Hello",
                   "review: echoed draft delimiters stripped");
            var reviewProfile = new DictationProfile
            {
                ReviewBeforePaste = true, ScreenshotContext = true, ReviewProviderId = Guid.NewGuid(),
            };
            var reviewRound = DictationProfile.FromJson(reviewProfile.ToJson());
            Expect(reviewRound.ReviewBeforePaste && reviewRound.ScreenshotContext
                   && reviewRound.ReviewProviderId == reviewProfile.ReviewProviderId,
                   "review: profile options round trip");
            var plainProfile = DictationProfile.FromJson(new Dictionary<string, object>());
            Expect(!plainProfile.ReviewBeforePaste && !plainProfile.ScreenshotContext,
                   "review: options default off");

            var prompts = FindSharedPrompts();
            if (prompts != null)
            {
                Expect(File.ReadAllText(Path.Combine(prompts, "cleanup-rich.txt")).Trim()
                       == CleanupEngine.DefaultPrompt(CleanupMode.Rich).Trim(),
                       "cleanup: rich prompt matches shared/prompts");
                Expect(File.ReadAllText(Path.Combine(prompts, "cleanup-light.txt")).Trim()
                       == CleanupEngine.DefaultPrompt(CleanupMode.Light).Trim(),
                       "cleanup: light prompt matches shared/prompts");
                Expect(File.ReadAllText(Path.Combine(prompts, "review.txt")).Trim()
                       == CleanupEngine.ReviewPrompt.Trim(),
                       "review: prompt matches shared/prompts");
                Expect(File.ReadAllText(Path.Combine(prompts, "router.txt")).Trim()
                       .Replace("\r\n", "\n") == CleanupEngine.RouterPrompt.Trim(),
                       "router: prompt matches shared/prompts");
            }

            var sp = ProviderProfile.Preset(ProviderKind.VercelGateway);
            sp.SttModel = sp.ChatModel = "google/gemini-2.5-flash";
            Expect(CleanupEngine.SinglePassEligible(sp, sp, CleanupMode.Rich), "single-pass: same provider+model");
            Expect(!CleanupEngine.SinglePassEligible(sp, sp, CleanupMode.Off), "single-pass: off mode blocks");
            var spOther = ProviderProfile.Preset(ProviderKind.VercelGateway);
            Expect(!CleanupEngine.SinglePassEligible(sp, spOther, CleanupMode.Rich),
                   "single-pass: different provider blocks");
            sp.SttModel = "openai/whisper-1";
            Expect(!CleanupEngine.SinglePassEligible(sp, sp, CleanupMode.Rich),
                   "single-pass: different models block");
        }

        private static void TestWav()
        {
            var path = Path.Combine(Path.GetTempPath(), "vv-wav-" + Guid.NewGuid() + ".wav");
            try
            {
                var writer = new WavWriter(path);
                var ramp = new short[16000];
                for (int i = 0; i < ramp.Length; i++) ramp[i] = (short)i;
                writer.Append(ramp, ramp.Length);
                Expect(writer.DataBytes == 32000, "wav: DataBytes tracks writes");
                var slice = WavWriter.SliceWav(path, 16000, 32000);
                Expect(slice.Length == 44 + 16000, "wav: slice size");
                Expect(BitConverter.ToUInt32(slice, 40) == 16000, "wav: slice header size");
                Expect(BitConverter.ToInt16(slice, 44) == 8000, "wav: slice starts mid-stream");
                var duration = writer.FinalizeFile();
                Expect(Math.Abs(duration - 1.0) < 0.001, "wav: duration");

                var bytes = File.ReadAllBytes(path);
                Expect(bytes.Length == 44 + 32000, "wav: file size");
                Expect(bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F',
                       "wav: RIFF magic");
                Expect(BitConverter.ToUInt32(bytes, 40) == 32000, "wav: data chunk size");
            }
            finally
            {
                try { File.Delete(path); } catch { }
            }
            Expect(WavWriter.SilentWav(0.3).Length == 44 + 9600, "wav: silent sample size");
            Expect(WavWriter.StartChime().Length > 44, "wav: chime generated");
        }

        private static void TestWebhook()
        {
            var entry = new Entry
            {
                Id = "20260825-120000", Folder = "Inbox",
                Date = DateTimeOffset.Parse("2026-08-25T12:00:00Z"),
                Duration = 12.4, Raw = "raw", Cleaned = "clean",
                SttLabel = "s", CleanupLabel = "c",
            };
            var payload = Json.ParseObject(WebhookSender.BuildPayload(entry));
            Expect(Json.Str(payload, "app") == "voicevector-windows"
                   && Json.Str(payload, "id") == "20260825-120000"
                   && Json.Str(payload, "date") == "2026-08-25T12:00:00Z",
                   "webhook: payload shape");
        }

        private static string FindSharedPrompts()
        {
            var dir = AppContext.BaseDirectory;
            for (int i = 0; i < 8 && dir != null; i++, dir = Path.GetDirectoryName(dir))
            {
                var candidate = Path.Combine(dir, "shared", "prompts");
                if (Directory.Exists(candidate)) return candidate;
            }
            return null;
        }
    }
}
