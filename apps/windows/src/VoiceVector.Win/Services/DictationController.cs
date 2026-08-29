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
        public enum StateKind { Idle, Recording, Processing, Failed }

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
            var config = _config();
            _activeProfileId = profileId
                ?? (config.DictationProfiles.Count > 0 ? config.DictationProfiles[0].Id : Guid.Empty);
            var folder = config.ActiveFolder;
            var slot = _library().NewEntrySlot(folder);
            _slot = slot;
            _slotFolder = folder;

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
            _slot = null;
            _hook.RecordingActive = false;
            SetState(StateKind.Idle, "");
        }

        public void Retry(Entry entry)
        {
            if (IsBusy) return;
            var audioPath = _library().AudioPath(entry);
            if (!File.Exists(audioPath)) return;
            SetState(StateKind.Processing, "Transcribing…");
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
                            var reply = await client.ChatAsync(
                                CleanupEngine.SystemPrompt(policy.Config),
                                CleanupEngine.WrapTranscript(entry.Raw)).ConfigureAwait(false);
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

            // 3. Paste.
            SetState(StateKind.Processing, "Pasting…");
            Diag.Breadcrumb("state Processing Pasting…");
            var outcome = await PasteService.InsertAsync(entry.Cleaned, config.AutoPaste)
                .ConfigureAwait(false);
            if (outcome == PasteService.Outcome.CopiedOnly)
                RaiseNotice("Transcript copied — press Ctrl+V to insert it.");
            SetState(StateKind.Idle, "");

            // 4. Webhook (fire and forget).
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
