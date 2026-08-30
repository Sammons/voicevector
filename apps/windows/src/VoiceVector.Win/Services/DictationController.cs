using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Threading;
using VoiceVector.Shared;

namespace VoiceVector.Win.Services
{
    /// <summary>
    /// One dictation at a time: hotkey → record → transcribe → clean → paste →
    /// save → webhook. Mirrors the macOS pipeline exactly (single-pass and
    /// streamed silence-gap transcription included).
    /// </summary>
    public sealed class DictationController
    {
        public enum StateKind { Idle, Recording, Processing, Reviewing, Failed }

        /// <summary>Text staged in the HUD while a review session is active.</summary>
        public string ReviewDraft { get; private set; }

        private sealed class ReviewSession
        {
            public Entry Entry;
            public string AudioPath;
            public string Folder;
            public AppConfig Config;
            public DictationProfile Profile;
            public CleanupEngine.EffectiveCleanup Policy;
            public ScreenshotSet Screenshots;
            public List<WindowInfo> Windows;
            public RouteTarget Route;
            public int Revisions;
        }
        /// <summary>Where the router decided the draft should go.</summary>
        public sealed class RouteTarget
        {
            public string Machine = "";
            public uint Window;            // 0 = whatever is focused there
            public PeerRef Peer;           // null = this machine
            public string Label = "";
        }

        /// <summary>Router verdict shown in the staging card, or null.</summary>
        public string ReviewRoute;

        private List<WindowInfo> _pendingWindows = new List<WindowInfo>();
        private ReviewSession _review;
        private string _commandPath;
        private ScreenshotSet _pendingScreenshots;

        public StateKind State { get; private set; }
        public string StateDetail { get; private set; }

        public event Action StateChanged;
        public event Action LibraryChanged;
        public event Action<string> Notice;

        public readonly WasapiRecorder Recorder = new WasapiRecorder();

        private readonly Func<AppConfig> _config;
        private readonly Func<Library> _library;
        private readonly KeyboardHook _hook;
        private readonly Dispatcher _dispatcher;
        private KeyValuePair<string, string>? _slot; // id, audioPath
        private string _slotFolder;

        private readonly object _segmentLock = new object();
        private readonly List<Task<string>> _segmentTasks = new List<Task<string>>();

        private static readonly string FakeAudioPath =
            Environment.GetEnvironmentVariable("VV_FAKE_AUDIO");

        public DictationController(Func<AppConfig> config, Func<Library> library,
                                   KeyboardHook hook, Dispatcher dispatcher)
        {
            _config = config;
            _library = library;
            _hook = hook;
            _dispatcher = dispatcher;
            State = StateKind.Idle;
            StateDetail = "";
        }

        /// <summary>Pushes the mic warm policy from config into the recorder.</summary>
        public void ApplyWarmPolicy()
        {
            var config = _config();
            Recorder.WarmAfterRecording = config.KeepMicWarmAfterRecording;
            Recorder.AlwaysWarm = config.KeepMicAlwaysWarm;
            Recorder.ApplyWarmPolicy();
        }

        public bool IsRecording { get { return State == StateKind.Recording; } }
        public bool IsBusy
        {
            get { return State == StateKind.Recording || State == StateKind.Processing; }
        }

        public void Handle(TapStateMachine.Act action, Guid profileId)
        {
            switch (action)
            {
                case TapStateMachine.Act.StartRecording: StartRecording(profileId); break;
                case TapStateMachine.Act.Commit: FinishRecording(); break;
                case TapStateMachine.Act.Discard: DiscardRecording(); break;
            }
        }

        private Guid _activeProfileId;

        public void StartRecording(Guid? profileId = null)
        {
            if (IsBusy) return;
            if (State == StateKind.Reviewing) { StartCommandRecording(); return; }
            var config = _config();
            _activeProfileId = profileId
                ?? (config.DictationProfiles.Count > 0 ? config.DictationProfiles[0].Id : Guid.Empty);
            var folder = config.ActiveFolder;
            var slot = _library().NewEntrySlot(folder);
            _slot = slot;
            _slotFolder = folder;

            // Screenshots of what the user is looking at, before anything moves.
            _pendingScreenshots = null;
            var startProfile = config.DictationProfiles.FirstOrDefault(p => p.Id == _activeProfileId);
            if (startProfile != null && startProfile.ScreenshotContext && FakeAudioPath == null)
            {
                _pendingScreenshots = ScreenCapture.AllScreens();
                _library().SaveScreenshots(slot.Key, folder, _pendingScreenshots);
            }
            _pendingWindows = new List<WindowInfo>();
            if (startProfile != null && startProfile.RouterEnabled && startProfile.ReviewBeforePaste
                && FakeAudioPath == null)
                _pendingWindows = WindowInventory.List();

            // Arm silence-gap streaming (not with fake audio or single-pass).
            lock (_segmentLock) _segmentTasks.Clear();
            var dictProfile = config.DictationProfiles.FirstOrDefault(p => p.Id == _activeProfileId);
            var armPolicy = CleanupEngine.Effective(dictProfile, config);
            var stt = armPolicy.Stt;
            if (config.ChunkedTranscription && FakeAudioPath == null && stt != null
                && (!armPolicy.Enabled || !CleanupEngine.SinglePassEligible(
                        stt, armPolicy.Provider, armPolicy.Config.Mode)))
            {
                var client = new ProviderClient(stt, KeyStore.GetApiKey(stt.Id));
                var vocabulary = CleanupEngine.ParseVocabulary(armPolicy.Config.Vocabulary);
                Recorder.Chunking = true;
                Recorder.OnSegment = (data, index) =>
                {
                    var task = Task.Run(async () =>
                        (await client.TranscribeAsync(data, "segment" + index + ".wav",
                                                      vocabulary).ConfigureAwait(false)).Text);
                    lock (_segmentLock) _segmentTasks.Add(task);
                };
            }
            else
            {
                Recorder.Chunking = false;
                Recorder.OnSegment = null;
            }

            if (FakeAudioPath != null)
            {
                SetState(StateKind.Recording, "Recording…");
                _hook.RecordingActive = true;
                Diag.Breadcrumb("state Recording (fake audio)");
                return;
            }
            try
            {
                Recorder.Start(slot.Value);
                SetState(StateKind.Recording, "Recording…");
                _hook.RecordingActive = true;
                Chime.PlayStart(config.PlaySounds);
            }
            catch (Exception e)
            {
                _slot = null;
                Fail("Could not start recording: " + e.Message);
            }
        }

        public void FinishRecording()
        {
            if (_review != null && _commandPath != null) { FinishCommandRecording(); return; }
            if (State != StateKind.Recording || _slot == null) return;
            var slot = _slot.Value;
            var folder = _slotFolder;
            _slot = null;
            _hook.RecordingActive = false;
            uint tailStartByte = Recorder.TailStartByte;
            double duration;
            if (FakeAudioPath != null)
            {
                File.Copy(FakeAudioPath, slot.Value, true);
                var bytes = new FileInfo(slot.Value).Length;
                duration = Math.Max(0.6, (bytes - 44) / 32000.0);
            }
            else
            {
                duration = Recorder.Stop();
                Chime.PlayStop(_config().PlaySounds);
            }

            if (duration < 0.5)
            {
                try { File.Delete(slot.Value); } catch { }
                SetState(StateKind.Idle, "");
                return;
            }

            SetState(StateKind.Processing, "Transcribing…");
            var _ = ProcessAsync(slot.Key, slot.Value, folder, duration, tailStartByte,
                                 _activeProfileId);
        }

        public void DiscardRecording()
        {
            if (State != StateKind.Recording) return;
            Recorder.Discard();
            if (_slot != null && (_review == null || _commandPath == null))
            {
                _library().DeleteScreenshots(_slot.Value.Key, _slotFolder);
                _pendingScreenshots = null;
            }
            _slot = null;
            _hook.RecordingActive = false;
            if (_review != null && _commandPath != null)
            {
                _commandPath = null;
                SetState(StateKind.Reviewing, "");
            }
            else
            {
                SetState(StateKind.Idle, "");
            }
        }

        // -- review session (staged draft, spoken revisions) -----------------------

        private void BeginReview(Entry entry, string audioPath, string folder, AppConfig config,
                                 DictationProfile profile, CleanupEngine.EffectiveCleanup policy)
        {
            _review = new ReviewSession
            {
                Entry = entry, AudioPath = audioPath, Folder = folder, Config = config,
                Profile = profile, Policy = policy, Screenshots = _pendingScreenshots,
                Windows = _pendingWindows,
            };
            ReviewDraft = entry.Cleaned;
            ReviewRoute = null;
            SetState(StateKind.Reviewing, "");
            if (profile != null && profile.RouterEnabled)
            {
                ReviewRoute = "Routing…";
                SetState(StateKind.Reviewing, "");
                var _ = RunRouterAsync();
            }
        }

        // -- AI routing (docs/multi-machine.md) ------------------------------

        private static ProviderProfile RouterProvider(ReviewSession session)
        {
            if (session.Profile != null && session.Profile.RouterProviderId.HasValue)
            {
                var chosen = session.Config.Providers.FirstOrDefault(
                    p => p.Id == session.Profile.RouterProviderId.Value);
                if (chosen != null) return chosen;
            }
            return ReviewProvider(session);
        }

        private async Task RunRouterAsync()
        {
            var session = _review;
            var draft = ReviewDraft;
            if (session == null || draft == null) { ReviewRoute = null; return; }
            var provider = RouterProvider(session);
            if (provider == null) { ReviewRoute = null; SetState(StateKind.Reviewing, ""); return; }
            var mm = session.Config.MultiMachine;
            var machineName = mm.ResolvedMachineName;
            var contexts = new List<MachineContext>
            {
                // Never capture screenshots here when the hotkey has screenshot
                // context off; use only what the dictation already gathered.
                PeerService.LocalContext(machineName, session.Windows, session.Screenshots, false),
            };
            var peers = mm.Peers.Where(p => p.Address.Length > 0).ToList();
            if (peers.Count > 0)
            {
                var fetches = peers.Select(p => PeerService.Shared.FetchContextAsync(p)).ToList();
                foreach (var fetch in fetches)
                {
                    var context = await fetch.ConfigureAwait(false);
                    if (context != null) contexts.Add(context);
                }
            }
            if (_review != session) return;   // review ended while gathering
            var message = CleanupEngine.RouterMessage(draft,
                contexts.Select(c => Tuple.Create(c.Machine, c.IsLocal, c.WindowLines)).ToList());
            var images = contexts.SelectMany(c => c.Screens).ToList();
            try
            {
                var client = new ProviderClient(provider, KeyStore.GetApiKey(provider.Id));
                string reply;
                try { reply = await client.ChatAsync(CleanupEngine.RouterPrompt, message, images).ConfigureAwait(false); }
                catch { reply = await client.ChatAsync(CleanupEngine.RouterPrompt, message).ConfigureAwait(false); }
                var verdict = CleanupEngine.ParseRouterVerdict(reply);
                // Apply on the UI thread so it can't interleave with AcceptReview;
                // the session check inside runs there too.
                _dispatcher.Invoke((Action)(() => {
                    if (_review == session) ApplyVerdict(verdict, contexts, machineName, session);
                }));
            }
            catch (Exception e)
            {
                Log.Error("Router failed: " + e.Message);
                ReviewRoute = null;
            }
            if (_review == session) SetState(StateKind.Reviewing, "");
        }

        private void ApplyVerdict(CleanupEngine.RouterVerdict verdict, List<MachineContext> contexts,
                                  string machineName, ReviewSession session)
        {
            ReviewRoute = null;
            session.Route = null;
            if (verdict == null) return;
            var context = contexts.FirstOrDefault(c => c.Machine == verdict.Machine);
            if (context == null) return;
            var window = context.Windows.FirstOrDefault(w => w.Id == verdict.Window);
            var windowLabel = window == null ? null
                : (window.Title.Length == 0 ? window.App : window.App + " — " + window.Title);
            if (context.IsLocal)
            {
                if (window == null) return;   // focused window — the normal paste
                session.Route = new RouteTarget { Machine = machineName, Window = window.Id, Label = windowLabel };
                ReviewRoute = "→ " + windowLabel;
            }
            else
            {
                // Resolve the peer by pinned fingerprint, not display name.
                var peer = session.Config.MultiMachine.Peers.FirstOrDefault(p => p.Fingerprint == context.Fingerprint);
                if (peer == null) return;
                var label = (windowLabel != null ? windowLabel + " on " : "") + context.Machine;
                session.Route = new RouteTarget
                    { Machine = context.Machine, Window = window != null ? window.Id : 0, Peer = peer, Label = label };
                ReviewRoute = "→ " + label;
            }
        }

        private static ProviderProfile ReviewProvider(ReviewSession session)
        {
            if (session.Profile != null && session.Profile.ReviewProviderId.HasValue)
            {
                var chosen = session.Config.Providers.FirstOrDefault(
                    p => p.Id == session.Profile.ReviewProviderId.Value);
                if (chosen != null) return chosen;
            }
            var cleanup = session.Policy.Provider;
            if (cleanup != null && cleanup.Kind.SupportsChat() && cleanup.ChatModel.Length > 0) return cleanup;
            return session.Config.Providers.FirstOrDefault(
                p => p.Kind.SupportsChat() && p.ChatModel.Length > 0);
        }

        private void StartCommandRecording()
        {
            if (_review == null || State != StateKind.Reviewing) return;
            var path = Path.Combine(Path.GetTempPath(), "vv-command-" + Guid.NewGuid().ToString("N") + ".wav");
            _commandPath = path;
            Recorder.Chunking = false;
            Recorder.OnSegment = null;
            try
            {
                Recorder.Start(path);
                SetState(StateKind.Recording, "Listening for a change…");
                _hook.RecordingActive = true;
                Chime.PlayStart(_config().PlaySounds);
            }
            catch (Exception e)
            {
                _commandPath = null;
                RaiseNotice("Could not record the change: " + e.Message);
                SetState(StateKind.Reviewing, "");
            }
        }

        private void FinishCommandRecording()
        {
            var session = _review;
            var path = _commandPath;
            if (session == null || path == null) return;
            _commandPath = null;
            _hook.RecordingActive = false;
            var duration = Recorder.Stop();
            Chime.PlayStop(_config().PlaySounds);
            if (duration < 0.5)
            {
                try { File.Delete(path); } catch { }
                SetState(StateKind.Reviewing, "");
                return;
            }
            SetState(StateKind.Processing, "Hearing the change…");
            var _ = ApplyCommandAsync(path, session);
        }

        private async Task ApplyCommandAsync(string path, ReviewSession session)
        {
            try
            {
                var stt = session.Policy.Stt;
                var reviewer = ReviewProvider(session);
                if (stt == null) { RaiseNotice("No transcription provider — pick one in Settings."); return; }
                if (reviewer == null) { RaiseNotice("No review model — pick a cleanup or review model for this hotkey."); return; }
                var vocabulary = CleanupEngine.ParseVocabulary(session.Policy.Config.Vocabulary);
                var audio = File.ReadAllBytes(path);
                var instruction = (await new ProviderClient(stt, KeyStore.GetApiKey(stt.Id))
                    .TranscribeAsync(audio, "command.wav", vocabulary).ConfigureAwait(false)).Text.Trim();
                var draft = ReviewDraft;
                if (instruction.Length == 0 || draft == null) return;
                SetState(StateKind.Processing, "Revising…");
                var client = new ProviderClient(reviewer, KeyStore.GetApiKey(reviewer.Id));
                var system = CleanupEngine.ReviewSystemPrompt(session.Policy.Config.Vocabulary);
                var user = CleanupEngine.ReviewMessage(draft, instruction);
                string reply;
                if (session.Screenshots != null)
                {
                    try { reply = await client.ChatAsync(system, user, session.Screenshots.Attachments()).ConfigureAwait(false); }
                    catch { reply = await client.ChatAsync(system, user).ConfigureAwait(false); }
                }
                else
                {
                    reply = await client.ChatAsync(system, user).ConfigureAwait(false);
                }
                ReviewDraft = CleanupEngine.PostProcess(reply, draft);
                session.Revisions++;
                session.Entry.CleanupLabel = session.Entry.CleanupLabel.Split(new[] { " · review " }, StringSplitOptions.None)[0]
                    + " · review " + reviewer.Name + "/" + reviewer.ChatModel + " ×" + session.Revisions;
            }
            catch (Exception e)
            {
                Chime.PlayError();
                RaiseNotice("Revision failed: " + e.Message);
            }
            finally
            {
                try { File.Delete(path); } catch { }
                SetState(StateKind.Reviewing, "");
            }
        }

        /// <summary>Enter while reviewing: save the draft and paste it.</summary>
        public void AcceptReview()
        {
            var session = _review;
            if (State != StateKind.Reviewing || session == null || ReviewDraft == null) return;
            _review = null;
            session.Entry.Cleaned = ReviewDraft;
            ReviewDraft = null;
            ReviewRoute = null;
            if (session.Route != null)
                session.Entry.CleanupLabel += " · routed to " + session.Route.Label;
            _library().Save(session.Entry);
            RaiseLibraryChanged();
            var _ = DeliverRoutedAsync(session);
        }

        private async Task DeliverRoutedAsync(ReviewSession session)
        {
            var target = session.Route;
            if (target == null)
            {
                await DeliverAsync(session.Entry, session.AudioPath, session.Folder, session.Config)
                    .ConfigureAwait(false);
                return;
            }
            if (target.Peer != null)
            {
                SetState(StateKind.Processing, "Sending to " + target.Machine + "…");
                var error = await PeerService.Shared.DeliverAsync(session.Entry.Cleaned, target.Window, target.Peer)
                    .ConfigureAwait(false);
                if (error != null)
                {
                    RaiseNotice("Could not deliver to " + target.Machine + ": " + error + " — pasting here.");
                    await DeliverAsync(session.Entry, session.AudioPath, session.Folder, session.Config)
                        .ConfigureAwait(false);
                    return;
                }
                SetState(StateKind.Idle, "");
                WebhookConfig webhook;
                if (session.Config.FolderWebhooks.TryGetValue(session.Folder, out webhook) && webhook.Enabled)
                {
                    var finished = session.Entry;
                    var path = session.AudioPath;
                    var _ = Task.Run(() => WebhookSender.SendAsync(finished, path, webhook));
                }
            }
            else
            {
                SetState(StateKind.Processing, "Pasting…");
                if (!WindowInventory.Activate(target.Window))
                    Log.Error("Routed window is gone; pasting into the focused window.");
                await Task.Delay(350).ConfigureAwait(false);
                await DeliverAsync(session.Entry, session.AudioPath, session.Folder, session.Config)
                    .ConfigureAwait(false);
            }
        }

        /// <summary>Inbound routed text from a paired machine: activate the
        /// window (when known), paste, save a routed entry.</summary>
        public void ReceiveRoutedText(string text, uint window, string machine,
                                      Action<bool, string> done)
        {
            // Not while recording OR mid-review — a paste would steal focus.
            if (IsBusy || State == StateKind.Reviewing) { done(false, "busy dictating"); return; }
            var _ = ReceiveRoutedAsync(text, window, machine, done);
        }

        private async Task ReceiveRoutedAsync(string text, uint window, string machine,
                                              Action<bool, string> done)
        {
            try
            {
                if (window != 0 && !WindowInventory.Activate(window))
                    Log.Error("Routed window " + window + " could not be focused; pasting into the foreground window.");
                await Task.Delay(350).ConfigureAwait(false);
                var config = _config();
                var library = _library();
                var outcome = await PasteService.InsertAsync(text, config.AutoPaste).ConfigureAwait(false);
                var slot = library.NewEntrySlot(config.ActiveFolder);
                var entry = new Entry
                {
                    Id = slot.Key, Folder = config.ActiveFolder, Date = DateTimeOffset.Now,
                    Duration = 0, SttLabel = "routed from " + machine, Status = "complete",
                    Cleaned = text, Raw = text,
                };
                if (outcome == PasteService.Outcome.CopiedOnly)
                {
                    entry.Status = "complete (copied only)";
                    RaiseNotice("Text from " + machine + " copied — press Ctrl+V to insert it.");
                }
                library.Save(entry);
                RaiseLibraryChanged();
                // Tell the sender the truth: only "pasted" is a real delivery.
                if (outcome == PasteService.Outcome.CopiedOnly)
                    done(false, "the receiving machine copied the text to its clipboard instead of pasting");
                else
                    done(true, "");
            }
            catch (Exception e)
            {
                done(false, e.Message);
            }
        }

        /// <summary>Esc while reviewing: keep the entry (with the draft) but don't paste.</summary>
        public void DiscardReview()
        {
            var session = _review;
            if (State != StateKind.Reviewing || session == null) return;
            _review = null;
            if (ReviewDraft != null) session.Entry.Cleaned = ReviewDraft;
            ReviewDraft = null;
            ReviewRoute = null;
            session.Entry.Status = "complete (not pasted)";
            _library().Save(session.Entry);
            RaiseLibraryChanged();
            SetState(StateKind.Idle, "");
        }

        public void Retry(Entry entry)
        {
            if (IsBusy) return;
            var audioPath = _library().AudioPath(entry);
            if (!File.Exists(audioPath)) return;
            SetState(StateKind.Processing, "Transcribing…");
            // Reuse the screenshots saved with the entry; the screen has moved on.
            _pendingScreenshots = _library().LoadScreenshots(entry);
            var _ = ProcessAsync(entry.Id, audioPath, entry.Folder, entry.Duration, 0, Guid.Empty);
        }

        private async Task ProcessAsync(string id, string audioPath, string folder,
                                        double duration, uint tailStartByte, Guid profileId)
        {
            var config = _config();
            var library = _library();
            var entry = new Entry
            {
                Id = id, Folder = folder, Date = DateTimeOffset.Now, Duration = duration,
            };
            entry.Attach(_pendingScreenshots);

            var dictationProfile = config.DictationProfiles.FirstOrDefault(p => p.Id == profileId);
            var policy = CleanupEngine.Effective(dictationProfile, config);
            var sttProfile = policy.Stt;
            if (sttProfile == null)
            {
                entry.Status = "error: no transcription provider configured";
                library.Save(entry);
                Fail("No transcription provider configured — open Settings.");
                RaiseLibraryChanged();
                return;
            }
            entry.SttLabel = sttProfile.Name + "/" + sttProfile.SttModel;

            // 0. Single-pass (same provider + same model fuses the stages).
            bool singlePassed = false;
            if (policy.Enabled
                && CleanupEngine.SinglePassEligible(sttProfile, policy.Provider, policy.Config.Mode))
            {
                try
                {
                    var client = new ProviderClient(sttProfile, KeyStore.GetApiKey(sttProfile.Id));
                    var audio = File.ReadAllBytes(audioPath);
                    var reply = await client.ChatWithAudioAsync(
                        CleanupEngine.SystemPrompt(policy.Config), audio).ConfigureAwait(false);
                    var cleaned = CleanupEngine.PostProcess(reply, "");
                    if (cleaned.Length == 0)
                        throw new InvalidOperationException("empty single-pass result");
                    entry.Raw = cleaned;
                    entry.Cleaned = cleaned;
                    entry.SttLabel = sttProfile.Name + "/" + sttProfile.ChatModel + " (single-pass)";
                    entry.CleanupLabel = "single-pass";
                    singlePassed = true;
                }
                catch (Exception e)
                {
                    Log.Error("Single-pass failed, falling back to two-pass: " + e.Message);
                }
            }

            if (!singlePassed)
            {
                var vocabulary = CleanupEngine.ParseVocabulary(policy.Config.Vocabulary);

                // 1. Transcribe — streamed segments first, full file as fallback.
                List<Task<string>> pending;
                lock (_segmentLock)
                {
                    pending = _segmentTasks.ToList();
                    _segmentTasks.Clear();
                }
                if (pending.Count > 0)
                {
                    try
                    {
                        var client = new ProviderClient(sttProfile, KeyStore.GetApiKey(sttProfile.Id));
                        var parts = new List<string>();
                        foreach (var task in pending)
                            parts.Add(await task.ConfigureAwait(false));
                        uint totalBytes = (uint)Math.Max(0, new FileInfo(audioPath).Length - 44);
                        if (totalBytes > tailStartByte)
                        {
                            var tail = WavWriter.SliceWav(audioPath, tailStartByte, totalBytes);
                            parts.Add((await client.TranscribeAsync(tail, "tail.wav", vocabulary)
                                .ConfigureAwait(false)).Text);
                        }
                        var joined = string.Join(" ",
                            parts.Select(p => p.Trim()).Where(p => p.Length > 0));
                        if (joined.Length > 0)
                        {
                            entry.Raw = joined;
                            entry.SttLabel += " (streamed " + (pending.Count + 1) + " parts)";
                        }
                    }
                    catch (Exception e)
                    {
                        Log.Error("Streamed transcription failed; falling back to full file: " + e.Message);
                    }
                }
                if (entry.Raw.Length == 0)
                {
                    try
                    {
                        var client = new ProviderClient(sttProfile, KeyStore.GetApiKey(sttProfile.Id));
                        var audio = File.ReadAllBytes(audioPath);
                        var result = await client.TranscribeAsync(audio, entry.AudioFilename, vocabulary)
                            .ConfigureAwait(false);
                        entry.Raw = result.Text.Trim();
                    }
                    catch (Exception e)
                    {
                        entry.Status = "error: transcription failed — " + e.Message;
                        library.Save(entry);
                        Fail("Transcription failed: " + e.Message);
                        RaiseLibraryChanged();
                        return;
                    }
                }

                if (entry.Raw.Length == 0)
                {
                    entry.Status = "error: empty transcript";
                    library.Save(entry);
                    Fail("The recording produced an empty transcript.");
                    RaiseLibraryChanged();
                    return;
                }

                // 2. Clean up (loud fallback to raw).
                entry.Cleaned = entry.Raw;
                if (!policy.Enabled)
                {
                    if (dictationProfile != null)
                        entry.CleanupLabel = "skipped — " + dictationProfile.Name + " hotkey";
                }
                else
                {
                    var cleanupProfile = policy.Provider;
                    if (cleanupProfile != null && cleanupProfile.Kind.SupportsChat()
                        && cleanupProfile.ChatModel.Length > 0)
                    {
                        SetState(StateKind.Processing, "Cleaning up…");
                        entry.CleanupLabel = cleanupProfile.Name + "/" + cleanupProfile.ChatModel;
                        try
                        {
                            var client = new ProviderClient(cleanupProfile,
                                                            KeyStore.GetApiKey(cleanupProfile.Id));
                            var system = CleanupEngine.SystemPrompt(policy.Config);
                            var user = CleanupEngine.WrapTranscript(entry.Raw);
                            string reply;
                            if (_pendingScreenshots != null)
                            {
                                try
                                {
                                    reply = await client.ChatAsync(system + "\n" + CleanupEngine.ScreenshotNote,
                                                                   user, _pendingScreenshots.Attachments()).ConfigureAwait(false);
                                }
                                catch { reply = await client.ChatAsync(system, user).ConfigureAwait(false); }
                            }
                            else
                            {
                                reply = await client.ChatAsync(system, user).ConfigureAwait(false);
                            }
                            entry.Cleaned = CleanupEngine.PostProcess(reply, entry.Raw);
                        }
                        catch (Exception e)
                        {
                            entry.CleanupLabel += " (failed — raw used)";
                            Log.Error("Cleanup failed, using raw transcript: " + e.Message);
                            RaiseNotice("Cleanup failed — raw transcript used: " + e.Message);
                        }
                    }
                    else
                    {
                        entry.CleanupLabel = "not run — no cleanup provider selected";
                        RaiseNotice("Cleanup skipped: no cleanup provider selected (Settings).");
                    }
                }
            }

            library.Save(entry);
            RaiseLibraryChanged();

            // Staged review: hold the draft in the HUD until Enter / Esc.
            if (dictationProfile != null && dictationProfile.ReviewBeforePaste && profileId != Guid.Empty)
            {
                BeginReview(entry, audioPath, folder, config, dictationProfile, policy);
                return;
            }
            await DeliverAsync(entry, audioPath, folder, config).ConfigureAwait(false);
        }

        /// <summary>3. Paste into the foreground app, then 4. webhook (fire and forget).</summary>
        private async Task DeliverAsync(Entry entry, string audioPath, string folder, AppConfig config)
        {
            SetState(StateKind.Processing, "Pasting…");
            Diag.Breadcrumb("state Processing Pasting…");
            var outcome = await PasteService.InsertAsync(entry.Cleaned, config.AutoPaste)
                .ConfigureAwait(false);
            if (outcome == PasteService.Outcome.CopiedOnly)
                RaiseNotice("Transcript copied — press Ctrl+V to insert it.");
            SetState(StateKind.Idle, "");

            WebhookConfig webhook;
            if (config.FolderWebhooks.TryGetValue(folder, out webhook) && webhook.Enabled)
            {
                var finished = entry;
                var path = audioPath;
                var _ = Task.Run(() => WebhookSender.SendAsync(finished, path, webhook));
            }
        }

        private void Fail(string message)
        {
            Chime.PlayError();
            SetState(StateKind.Failed, message);
            var _ = ClearFailureLaterAsync();
        }

        private async Task ClearFailureLaterAsync()
        {
            await Task.Delay(5000).ConfigureAwait(false);
            if (State == StateKind.Failed) SetState(StateKind.Idle, "");
        }

        private void SetState(StateKind state, string detail)
        {
            Diag.Breadcrumb("state " + state + " " + detail);
            _dispatcher.BeginInvoke((Action)(() =>
            {
                State = state;
                StateDetail = detail;
                var handler = StateChanged;
                if (handler != null) handler();
            }));
        }

        private void RaiseLibraryChanged()
        {
            _dispatcher.BeginInvoke((Action)(() =>
            {
                var handler = LibraryChanged;
                if (handler != null) handler();
            }));
        }

        private void RaiseNotice(string message)
        {
            _dispatcher.BeginInvoke((Action)(() =>
            {
                var handler = Notice;
                if (handler != null) handler(message);
            }));
        }
    }
}
