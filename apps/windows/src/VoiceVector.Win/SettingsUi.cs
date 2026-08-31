using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using VoiceVector.Shared;
using VoiceVector.Win.Services;

namespace VoiceVector.Win
{
    /// <summary>Settings page (providers / dictation / folders / general),
    /// shared with the wizard's provider and hotkey steps.</summary>
    public static class SettingsUi
    {
        public static UIElement Build(MainWindow owner)
        {
            var stack = new StackPanel { Margin = new Thickness(20, 4, 20, 20) };

            var back = Theme.MakeButton("←  Back to library");
            back.Background = Brushes.Transparent;
            back.BorderBrush = Brushes.Transparent;
            back.Foreground = Theme.AccentBrush;
            back.HorizontalAlignment = HorizontalAlignment.Left;
            back.Margin = new Thickness(0, 0, 0, 10);
            back.Click += (s, e) => owner.CloseSettings();
            stack.Children.Add(back);

            stack.Children.Add(Section("Providers", BuildProviders(owner)));
            stack.Children.Add(Section("Dictation", BuildDictation(owner)));
            stack.Children.Add(Section("Shared vocabulary", BuildVocabulary()));
            stack.Children.Add(Section("Folders & Webhooks", BuildFolders()));
            stack.Children.Add(Section("Multi-machine", BuildMultiMachine()));
            stack.Children.Add(Section("General", BuildGeneral(owner)));
            stack.Children.Add(Section("About", BuildAbout()));

            return new ScrollViewer
            {
                Content = stack,
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            };
        }

        private static UIElement Section(string title, UIElement content)
        {
            var inner = new StackPanel();
            inner.Children.Add(Theme.SectionTitle(title));
            inner.Children.Add(content);
            return Theme.Card(inner);
        }

        // -- providers ---------------------------------------------------------

        public static UIElement BuildProviders(MainWindow owner)
        {
            var stack = new StackPanel();
            foreach (var profile in Program.Config.Providers.ToList())
                stack.Children.Add(BuildProviderEditor(owner, profile));

            var addBox = new ComboBox { MinWidth = 260, Margin = new Thickness(0, 6, 0, 0) };
            addBox.Items.Add("Add provider…");
            addBox.Items.Add("ElevenLabs (transcription)");
            addBox.Items.Add("Vercel AI Gateway (STT + cleanup)");
            addBox.Items.Add("Fireworks (cleanup LLM)");
            addBox.Items.Add("Cerebras (fast cleanup LLM)");
            addBox.Items.Add("OpenAI-compatible (Azure Foundry, Foundry Local, self-hosted)");
            addBox.SelectedIndex = 0;
            addBox.SelectionChanged += (s, e) =>
            {
                if (addBox.SelectedIndex <= 0) return;
                ProviderKind kind;
                switch (addBox.SelectedIndex)
                {
                    case 1: kind = ProviderKind.ElevenLabs; break;
                    case 2: kind = ProviderKind.VercelGateway; break;
                    case 3: kind = ProviderKind.Fireworks; break;
                    case 4: kind = ProviderKind.Cerebras; break;
                    default: kind = ProviderKind.OpenAICompatible; break;
                }
                var profile = ProviderProfile.Preset(kind);
                Program.Config.Providers.Add(profile);
                if (Program.Config.SttProviderId == null && kind.SupportsTranscription())
                    Program.Config.SttProviderId = profile.Id;
                if (Program.Config.Cleanup.ProviderId == null && kind.SupportsChat())
                    Program.Config.Cleanup.ProviderId = profile.Id;
                Program.Config.Save();
                owner.RefreshContent();
            };
            stack.Children.Add(addBox);
            return stack;
        }

        private static UIElement BuildProviderEditor(MainWindow owner, ProviderProfile profile)
        {
            var stack = new StackPanel();

            var head = new Grid { Margin = new Thickness(0, 0, 0, 6) };
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var title = Theme.Text(profile.Name + "  (" + profile.Kind.DisplayName() + ")", 13.5);
            title.FontWeight = FontWeights.SemiBold;
            head.Children.Add(title);
            var remove = Theme.MakeButton("Remove");
            remove.Padding = new Thickness(8, 3, 8, 3);
            remove.FontSize = 11.5;
            remove.Click += (s, e) =>
            {
                KeyStore.DeleteApiKey(profile.Id);
                Program.Config.Providers.RemoveAll(p => p.Id == profile.Id);
                if (Program.Config.SttProviderId == profile.Id) Program.Config.SttProviderId = null;
                if (Program.Config.Cleanup.ProviderId == profile.Id) Program.Config.Cleanup.ProviderId = null;
                Program.Config.Save();
                owner.RefreshContent();
            };
            Grid.SetColumn(remove, 1);
            head.Children.Add(remove);
            stack.Children.Add(head);

            if (profile.Kind == ProviderKind.OpenAICompatible)
            {
                stack.Children.Add(Labeled("Base URL", MakeBound(profile.BaseUrl, v =>
                {
                    profile.BaseUrl = v.Trim();
                    Program.Config.Save();
                }, mono: true)));
                var hint = Theme.Text(
                    "Azure Foundry: https://<resource>.openai.azure.com/openai/v1 (model = deployment name). "
                    + "Foundry Local: http://localhost:5273/v1.", 11, secondary: true);
                hint.Margin = new Thickness(0, 2, 0, 6);
                stack.Children.Add(hint);
            }

            var keyBox = new PasswordBox
            {
                Password = KeyStore.GetApiKey(profile.Id),
                FontFamily = Theme.UiFont,
                Padding = new Thickness(8, 6, 8, 6),
                Background = Theme.IsDark
                    ? new SolidColorBrush(Color.FromArgb(255, 30, 28, 40)) : Brushes.White,
                Foreground = Theme.TextPrimary,
                BorderBrush = Theme.Divider,
            };
            keyBox.PasswordChanged += (s, e) => KeyStore.SetApiKey(profile.Id, keyBox.Password);
            stack.Children.Add(Labeled("API key", keyBox));

            if (profile.Kind.SupportsTranscription())
                stack.Children.Add(Labeled("STT model", MakeBound(profile.SttModel, v =>
                {
                    profile.SttModel = v.Trim();
                    Program.Config.Save();
                }, mono: true)));
            if (profile.Kind.SupportsChat())
                stack.Children.Add(Labeled("Chat model", MakeBound(profile.ChatModel, v =>
                {
                    profile.ChatModel = v.Trim();
                    Program.Config.Save();
                }, mono: true)));

            var actions = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Margin = new Thickness(0, 6, 0, 10),
            };
            var status = Theme.Text("", 12, secondary: true);
            status.Margin = new Thickness(10, 0, 0, 0);
            status.VerticalAlignment = VerticalAlignment.Center;
            var test = Theme.MakeButton("Test");
            test.Click += async (s, e) =>
            {
                test.IsEnabled = false;
                status.Text = "Testing…";
                try
                {
                    var client = new ProviderClient(profile, KeyStore.GetApiKey(profile.Id));
                    status.Text = await client.TestAsync();
                }
                catch (Exception ex)
                {
                    status.Text = "Test failed: " + ex.Message;
                }
                finally { test.IsEnabled = true; }
            };
            actions.Children.Add(test);
            actions.Children.Add(status);
            stack.Children.Add(actions);

            var border = new Border
            {
                BorderBrush = Theme.Divider,
                BorderThickness = new Thickness(0, 0, 0, 1),
                Padding = new Thickness(0, 0, 0, 8),
                Margin = new Thickness(0, 0, 0, 10),
                Child = stack,
            };
            return border;
        }

        private static TextBox MakeBound(string initial, Action<string> onChange, bool mono = false)
        {
            var box = Theme.MakeTextBox(initial, mono);
            box.TextChanged += (s, e) => onChange(box.Text);
            return box;
        }

        private static UIElement Labeled(string label, UIElement control)
        {
            var stack = new StackPanel { Margin = new Thickness(0, 0, 0, 6) };
            stack.Children.Add(Theme.Text(label, 11.5, secondary: true));
            if (control is FrameworkElement fe) fe.Margin = new Thickness(0, 2, 0, 0);
            stack.Children.Add(control);
            return stack;
        }

        // -- dictation ---------------------------------------------------------

        public static UIElement BuildDictation(MainWindow owner)
        {
            var stack = new StackPanel();
            var config = Program.Config;

            // Transcription provider picker.
            var sttProviders = config.Providers
                .Where(p => p.Kind.SupportsTranscription() && p.SttModel.Length > 0).ToList();
            var sttBox = new ComboBox { MinWidth = 260, Margin = new Thickness(0, 2, 0, 10) };
            sttBox.Items.Add("None");
            foreach (var p in sttProviders) sttBox.Items.Add(p.Name + " — " + p.SttModel);
            int sttIndex = sttProviders.FindIndex(p => p.Id == config.SttProviderId);
            sttBox.SelectedIndex = sttIndex < 0 ? 0 : sttIndex + 1;
            sttBox.SelectionChanged += (s, e) =>
            {
                config.SttProviderId = sttBox.SelectedIndex <= 0
                    ? (Guid?)null : sttProviders[sttBox.SelectedIndex - 1].Id;
                config.Save();
            };
            stack.Children.Add(Labeled("Transcription provider", sttBox));

            // Hotkey profiles — each hotkey carries its own cleanup policy.
            stack.Children.Add(Theme.Text("Hotkeys", 11.5, secondary: true));
            var hotkeysHint = Theme.Text(
                "Click a hotkey button, then press a key or tap a modifier (e.g. Right Alt). "
                + "Each hotkey can clean up differently — or not at all.",
                11.5, secondary: true);
            hotkeysHint.TextWrapping = TextWrapping.Wrap;
            hotkeysHint.Margin = new Thickness(0, 2, 0, 4);
            stack.Children.Add(hotkeysHint);
            foreach (var profile in config.DictationProfiles.ToList())
                stack.Children.Add(BuildProfileRow(owner, profile));
            var addProfile = Theme.MakeButton("Add hotkey");
            addProfile.HorizontalAlignment = HorizontalAlignment.Left;
            addProfile.Margin = new Thickness(0, 4, 0, 10);
            addProfile.Click += (s, e) =>
            {
                config.DictationProfiles.Add(new DictationProfile
                {
                    Name = "Hotkey " + (config.DictationProfiles.Count + 1),
                    Hotkey = new HotkeySpec { KeyCode = 0, Modifiers = 0, IsModifierOnly = false },
                });
                config.Save();
                Program.Hook.Reconfigure();
                owner.RefreshContent();
            };
            stack.Children.Add(addProfile);

            stack.Children.Add(RadioGroup(null,
                new[] { "Double-tap to start (tap once to stop)",
                        "Single tap to start (tap again to stop)" },
                config.TapStartMode == TapStartMode.DoubleTap ? 0 : 1,
                index =>
                {
                    config.TapStartMode = index == 0 ? TapStartMode.DoubleTap : TapStartMode.SingleTap;
                    Program.Hook.Reconfigure();
                    config.Save();
                }));
            var holdHint = Theme.Text(
                "Hold-to-talk always works: press and hold, speak, release. Esc cancels.",
                11.5, secondary: true);
            holdHint.Margin = new Thickness(0, 2, 0, 10);
            stack.Children.Add(holdHint);

            var chunked = new CheckBox
            {
                Content = Theme.Text("Transcribe during pauses (long dictations finish faster)"),
                IsChecked = config.ChunkedTranscription,
                Margin = new Thickness(0, 8, 0, 0),
            };
            chunked.Click += (s, e) =>
            {
                config.ChunkedTranscription = chunked.IsChecked == true;
                config.Save();
            };
            stack.Children.Add(chunked);

            return stack;
        }

        public static UIElement BuildVocabulary()
        {
            var config = Program.Config;
            var stack = new StackPanel();
            var hint = Theme.Text(
                "Names and jargon every hotkey should get right — comma separated. Sent to the "
                + "transcriber when it supports it (ElevenLabs key terms, Whisper-style prompt) and "
                + "always added to the cleanup prompt.", 12, secondary: true);
            hint.TextWrapping = TextWrapping.Wrap;
            stack.Children.Add(hint);
            var vocab = Theme.MakeTextBox(config.Cleanup.Vocabulary, mono: true);
            vocab.AcceptsReturn = true;
            vocab.Height = 48;
            vocab.TextWrapping = TextWrapping.Wrap;
            vocab.Margin = new Thickness(0, 6, 0, 0);
            vocab.TextChanged += (s, e) => { config.Cleanup.Vocabulary = vocab.Text; config.Save(); };
            stack.Children.Add(vocab);

            var ids = new List<Guid>();
            if (config.SttProviderId.HasValue) ids.Add(config.SttProviderId.Value);
            foreach (var p in config.DictationProfiles)
                if (p.SttProviderId.HasValue) ids.Add(p.SttProviderId.Value);
            var unsupported = new List<string>();
            foreach (var id in ids)
            {
                var p = config.Providers.FirstOrDefault(x => x.Id == id);
                if (p != null && !p.Kind.SupportsVocabulary() && !unsupported.Contains(p.Name))
                    unsupported.Add(p.Name);
            }
            if (unsupported.Count > 0)
            {
                var note = Theme.Text(string.Join(", ", unsupported)
                    + " doesn't accept vocabulary hints — these terms will only guide cleanup "
                    + "for hotkeys using it.", 11.5, secondary: true);
                note.TextWrapping = TextWrapping.Wrap;
                note.Margin = new Thickness(0, 6, 0, 0);
                stack.Children.Add(note);
            }
            return stack;
        }

        private static UIElement BuildProfileRow(MainWindow owner, DictationProfile profile)
        {
            var config = Program.Config;
            var row = new StackPanel { Margin = new Thickness(0, 2, 0, 8) };

            var top = new StackPanel { Orientation = Orientation.Horizontal };
            var name = Theme.MakeTextBox(profile.Name);
            name.MinWidth = 130;
            name.TextChanged += (s, e) => { profile.Name = name.Text; config.Save(); };
            top.Children.Add(name);

            var hotkeyButton = Theme.MakeButton(profile.Hotkey.KeyCode == 0
                ? "Set hotkey…" : KeyboardHook.Describe(profile.Hotkey));
            hotkeyButton.Margin = new Thickness(8, 0, 0, 0);
            bool capturing = false;
            hotkeyButton.Click += (s, e) =>
            {
                capturing = !capturing;
                if (capturing)
                {
                    hotkeyButton.Content = "Press a key…";
                    Program.Hook.CaptureHandler = spec =>
                    {
                        profile.Hotkey = spec;
                        Program.Hook.CaptureHandler = null;
                        Program.Hook.Reconfigure();
                        config.Save();
                        capturing = false;
                        hotkeyButton.Content = KeyboardHook.Describe(spec);
                    };
                }
                else
                {
                    Program.Hook.CaptureHandler = null;
                    hotkeyButton.Content = profile.Hotkey.KeyCode == 0
                        ? "Set hotkey…" : KeyboardHook.Describe(profile.Hotkey);
                }
            };
            top.Children.Add(hotkeyButton);
            var modeBox = new ComboBox { MinWidth = 150, Margin = new Thickness(8, 0, 0, 0) };
            modeBox.Items.Add("Raw transcript");
            modeBox.Items.Add("Light cleanup");
            modeBox.Items.Add("Rich cleanup");
            modeBox.SelectedIndex = (int)CleanupEngine.EffectiveMode(profile, config);
            top.Children.Add(modeBox);

            if (config.DictationProfiles.Count > 1)
            {
                var remove = Theme.MakeButton("Remove");
                remove.Margin = new Thickness(8, 0, 0, 0);
                remove.Background = Brushes.Transparent;
                remove.BorderBrush = Brushes.Transparent;
                remove.Foreground = Theme.AccentBrush;
                remove.Click += (s, e) =>
                {
                    config.DictationProfiles.RemoveAll(p => p.Id == profile.Id);
                    if (config.DictationProfiles.Count == 0)
                        config.DictationProfiles.Add(new DictationProfile());
                    config.Save();
                    Program.Hook.Reconfigure();
                    owner.RefreshContent();
                };
                top.Children.Add(remove);
            }
            row.Children.Add(top);

            var sttProviders = config.Providers
                .Where(p => p.Kind.SupportsTranscription() && p.SttModel.Length > 0).ToList();
            var sttBox = new ComboBox { MinWidth = 220, Margin = new Thickness(22, 4, 0, 0),
                                        HorizontalAlignment = HorizontalAlignment.Left };
            var defaultStt = config.Providers.FirstOrDefault(p => p.Id == config.SttProviderId);
            sttBox.Items.Add(defaultStt == null ? "Default transcriber (none)"
                : "Default transcriber (" + defaultStt.Name + " — " + defaultStt.SttModel + ")");
            foreach (var p in sttProviders) sttBox.Items.Add(p.Name + " — " + p.SttModel);
            int sttIndex = profile.SttProviderId.HasValue
                ? sttProviders.FindIndex(p => p.Id == profile.SttProviderId.Value) : -1;
            sttBox.SelectedIndex = sttIndex < 0 ? 0 : sttIndex + 1;
            sttBox.SelectionChanged += (s, e) =>
            {
                profile.SttProviderId = sttBox.SelectedIndex <= 0
                    ? (Guid?)null : sttProviders[sttBox.SelectedIndex - 1].Id;
                config.Save();
            };
            row.Children.Add(sttBox);

            var reviewRow = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(22, 6, 0, 0) };
            var reviewToggle = new CheckBox
            {
                Content = Theme.Text("Review before pasting", 12),
                IsChecked = profile.ReviewBeforePaste,
            };
            var screenshotToggle = new CheckBox
            {
                Content = Theme.Text("Screenshot context", 12),
                IsChecked = profile.ScreenshotContext,
                Margin = new Thickness(14, 0, 0, 0),
            };
            var submitToggle = new CheckBox
            {
                Content = Theme.Text("Press Enter to submit", 12),
                IsChecked = profile.AutoSubmit,
                Margin = new Thickness(14, 0, 0, 0),
            };
            submitToggle.Click += (s, e) => { profile.AutoSubmit = submitToggle.IsChecked == true; config.Save(); };
            reviewRow.Children.Add(reviewToggle);
            reviewRow.Children.Add(screenshotToggle);
            reviewRow.Children.Add(submitToggle);
            row.Children.Add(reviewRow);

            var reviewBox = new ComboBox { MinWidth = 220, Margin = new Thickness(22, 4, 0, 0),
                                           HorizontalAlignment = HorizontalAlignment.Left };
            reviewBox.Items.Add("Review model: same as cleanup");
            var reviewProviders = config.Providers
                .Where(p => p.Kind.SupportsChat() && p.ChatModel.Length > 0).ToList();
            foreach (var p in reviewProviders) reviewBox.Items.Add(p.Name + " — " + p.ChatModel);
            int reviewIndex = profile.ReviewProviderId.HasValue
                ? reviewProviders.FindIndex(p => p.Id == profile.ReviewProviderId.Value) : -1;
            reviewBox.SelectedIndex = reviewIndex < 0 ? 0 : reviewIndex + 1;
            reviewBox.SelectionChanged += (s, e) =>
            {
                profile.ReviewProviderId = reviewBox.SelectedIndex <= 0
                    ? (Guid?)null : reviewProviders[reviewBox.SelectedIndex - 1].Id;
                config.Save();
            };
            var reviewHint = Theme.Text(
                "The cleaned text is staged above the recording pill instead of pasted. Press the hotkey "
                + "and say a change as many times as you like; Enter pastes, Esc discards.",
                11.5, secondary: true);
            reviewHint.TextWrapping = TextWrapping.Wrap;
            reviewHint.Margin = new Thickness(22, 2, 0, 0);
            var screenshotHint = Theme.Text(
                "A screenshot of every display is attached to cleanup and review calls so the model "
                + "knows what you're looking at; the window receiving the text is outlined. Models without vision ignore it.",
                11.5, secondary: true);
            screenshotHint.TextWrapping = TextWrapping.Wrap;
            screenshotHint.Margin = new Thickness(22, 2, 0, 0);
            var routerToggle = new CheckBox
            {
                Content = Theme.Text("Route with AI", 12),
                IsChecked = profile.RouterEnabled,
                Margin = new Thickness(22, 4, 0, 0),
            };
            routerToggle.Click += (s, e) => { profile.RouterEnabled = routerToggle.IsChecked == true; config.Save(); };
            var routerBox = new ComboBox { MinWidth = 220, Margin = new Thickness(22, 4, 0, 0),
                                           HorizontalAlignment = HorizontalAlignment.Left };
            routerBox.Items.Add("Router model: same as review");
            foreach (var p in reviewProviders) routerBox.Items.Add(p.Name + " — " + p.ChatModel);
            int routerIndex = profile.RouterProviderId.HasValue
                ? reviewProviders.FindIndex(p => p.Id == profile.RouterProviderId.Value) : -1;
            routerBox.SelectedIndex = routerIndex < 0 ? 0 : routerIndex + 1;
            routerBox.SelectionChanged += (s, e) =>
            {
                profile.RouterProviderId = routerBox.SelectedIndex <= 0
                    ? (Guid?)null : reviewProviders[routerBox.SelectedIndex - 1].Id;
                config.Save();
            };
            var routerHint = Theme.Text(
                "A router model looks at your windows (and paired machines' windows) and picks where "
                + "the draft should go; the staging card shows its choice and Enter sends it there. "
                + "Set up machines in the Multi-machine section.", 11.5, secondary: true);
            routerHint.TextWrapping = TextWrapping.Wrap;
            routerHint.Margin = new Thickness(22, 2, 0, 0);
            row.Children.Add(reviewBox);
            row.Children.Add(reviewHint);
            row.Children.Add(routerToggle);
            row.Children.Add(routerBox);
            row.Children.Add(routerHint);
            row.Children.Add(screenshotHint);
            routerToggle.Click += (s, e) =>
            {
                var routing = routerToggle.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
                routerBox.Visibility = routerHint.Visibility = routing;
            };
            routerBox.Visibility = routerHint.Visibility =
                profile.RouterEnabled ? Visibility.Visible : Visibility.Collapsed;
            Action syncReview = () =>
            {
                var review = reviewToggle.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
                reviewBox.Visibility = reviewHint.Visibility = review;
                screenshotHint.Visibility = screenshotToggle.IsChecked == true
                    ? Visibility.Visible : Visibility.Collapsed;
            };
            reviewToggle.Click += (s, e) =>
            {
                profile.ReviewBeforePaste = reviewToggle.IsChecked == true;
                config.Save();
                syncReview();
            };
            screenshotToggle.Click += (s, e) =>
            {
                profile.ScreenshotContext = screenshotToggle.IsChecked == true;
                config.Save();
                syncReview();
            };
            syncReview();

            var vocabLabel = Theme.Text("Extra vocabulary for this hotkey (added to the shared list; used by the transcriber when supported)",
                                        11.5, secondary: true);
            vocabLabel.Margin = new Thickness(22, 4, 0, 0);
            var vocabBox = Theme.MakeTextBox(profile.Vocabulary, mono: true);
            vocabBox.Margin = new Thickness(22, 2, 0, 0);
            vocabBox.TextChanged += (s, e) => { profile.Vocabulary = vocabBox.Text; config.Save(); };
            row.Children.Add(vocabLabel);
            row.Children.Add(vocabBox);

            var chatProviders = config.Providers
                .Where(p => p.Kind.SupportsChat() && p.ChatModel.Length > 0).ToList();
            var providerBox = new ComboBox { MinWidth = 220, Margin = new Thickness(22, 4, 0, 0),
                                             HorizontalAlignment = HorizontalAlignment.Left };
            var defaultProvider = config.Providers.FirstOrDefault(p => p.Id == config.Cleanup.ProviderId);
            providerBox.Items.Add(defaultProvider == null ? "Default (none)"
                : "Default (" + defaultProvider.Name + " — " + defaultProvider.ChatModel + ")");
            foreach (var p in chatProviders) providerBox.Items.Add(p.Name + " — " + p.ChatModel);
            int providerIndex = profile.CleanupProviderId.HasValue
                ? chatProviders.FindIndex(p => p.Id == profile.CleanupProviderId.Value) : -1;
            providerBox.SelectedIndex = providerIndex < 0 ? 0 : providerIndex + 1;
            providerBox.SelectionChanged += (s, e) =>
            {
                profile.CleanupProviderId = providerBox.SelectedIndex <= 0
                    ? (Guid?)null : chatProviders[providerBox.SelectedIndex - 1].Id;
                config.Save();
            };
            row.Children.Add(providerBox);

            bool isCustom = profile.CustomPrompt.Length > 0;
            var promptLabel = Theme.Text(
                isCustom ? "Cleanup prompt (custom for this hotkey)"
                         : "Cleanup prompt (built-in — any edit saves a custom prompt for this hotkey)",
                11.5, secondary: true);
            promptLabel.Margin = new Thickness(22, 4, 0, 0);
            var promptBox = Theme.MakeTextBox(
                isCustom ? profile.CustomPrompt
                         : CleanupEngine.SystemPromptBase(CleanupEngine.Effective(profile, config).Config),
                mono: true);
            promptBox.AcceptsReturn = true;
            promptBox.Height = 96;
            promptBox.TextWrapping = TextWrapping.Wrap;
            promptBox.FontSize = 11.5;
            promptBox.Margin = new Thickness(22, 2, 0, 0);
            promptBox.VerticalScrollBarVisibility = ScrollBarVisibility.Auto;
            promptBox.TextChanged += (s, e) => { profile.CustomPrompt = promptBox.Text; config.Save(); };
            var resetPrompt = Theme.MakeButton("Reset to built-in prompt");
            resetPrompt.Background = Brushes.Transparent;
            resetPrompt.BorderBrush = Brushes.Transparent;
            resetPrompt.Foreground = Theme.AccentBrush;
            resetPrompt.HorizontalAlignment = HorizontalAlignment.Left;
            resetPrompt.Margin = new Thickness(18, 0, 0, 0);
            resetPrompt.Visibility = isCustom ? Visibility.Visible : Visibility.Collapsed;
            resetPrompt.Click += (s, e) =>
            {
                profile.CustomPrompt = "";
                config.Save();
                owner.RefreshContent();
            };
            row.Children.Add(promptLabel);
            row.Children.Add(promptBox);
            row.Children.Add(resetPrompt);

            Action syncCleanupVisibility = () =>
            {
                var visibility = CleanupEngine.EffectiveMode(profile, config) != CleanupMode.Off
                    ? Visibility.Visible : Visibility.Collapsed;
                providerBox.Visibility = promptLabel.Visibility = promptBox.Visibility = visibility;
                resetPrompt.Visibility = visibility == Visibility.Visible && profile.CustomPrompt.Length > 0
                    ? Visibility.Visible : Visibility.Collapsed;
            };
            modeBox.SelectionChanged += (s, e) =>
            {
                profile.CleanupMode = (CleanupMode)modeBox.SelectedIndex;
                profile.CleanupEnabled = profile.CleanupMode != CleanupMode.Off;
                config.Save();
                syncCleanupVisibility();
            };
            syncCleanupVisibility();

            return row;
        }

        private static UIElement RadioGroup(string header, string[] options, int selected,
                                            Action<int> onSelect)
        {
            var stack = new StackPanel { Margin = new Thickness(0, 0, 0, 6) };
            if (header != null) stack.Children.Add(Theme.Text(header, 11.5, secondary: true));
            var group = Guid.NewGuid().ToString();
            for (int i = 0; i < options.Length; i++)
            {
                int index = i;
                var radio = new RadioButton
                {
                    Content = Theme.Text(options[i]),
                    GroupName = group,
                    IsChecked = i == selected,
                    Margin = new Thickness(0, 3, 0, 0),
                };
                radio.Checked += (s, e) => onSelect(index);
                stack.Children.Add(radio);
            }
            return stack;
        }

        // -- folders & webhooks ------------------------------------------------

        public static UIElement BuildFolders()
        {
            var stack = new StackPanel();
            stack.Children.Add(Theme.Text(
                "Each folder can forward finished dictations to a webhook.", 12, secondary: true));
            foreach (var folder in Program.Lib.FolderNames())
            {
                WebhookConfig config;
                if (!Program.Config.FolderWebhooks.TryGetValue(folder, out config))
                    config = new WebhookConfig();
                var captured = config;
                var name = folder;

                var row = new StackPanel { Margin = new Thickness(0, 8, 0, 0) };
                var toggle = new CheckBox
                {
                    Content = Theme.Text(folder + " — webhook", 13),
                    IsChecked = config.Enabled,
                };
                var url = Theme.MakeTextBox(config.Url, mono: true);
                url.Margin = new Thickness(22, 4, 0, 0);
                url.Visibility = config.Enabled ? Visibility.Visible : Visibility.Collapsed;
                var audio = new CheckBox
                {
                    Content = Theme.Text("Attach audio file (multipart)", 12),
                    IsChecked = config.IncludeAudio,
                    Margin = new Thickness(22, 4, 0, 0),
                    Visibility = config.Enabled ? Visibility.Visible : Visibility.Collapsed,
                };
                toggle.Click += (s, e) =>
                {
                    captured.Enabled = toggle.IsChecked == true;
                    url.Visibility = audio.Visibility =
                        captured.Enabled ? Visibility.Visible : Visibility.Collapsed;
                    Program.Config.FolderWebhooks[name] = captured;
                    Program.Config.Save();
                };
                url.TextChanged += (s, e) =>
                {
                    captured.Url = url.Text.Trim();
                    Program.Config.FolderWebhooks[name] = captured;
                    Program.Config.Save();
                };
                audio.Click += (s, e) =>
                {
                    captured.IncludeAudio = audio.IsChecked == true;
                    Program.Config.FolderWebhooks[name] = captured;
                    Program.Config.Save();
                };
                row.Children.Add(toggle);
                row.Children.Add(url);
                row.Children.Add(audio);
                stack.Children.Add(row);
            }
            return stack;
        }

        // -- general -----------------------------------------------------------

        // -- about ---------------------------------------------------------------

        private static Button LinkButton(string label, string url)
        {
            var button = Theme.MakeButton(label);
            button.Background = Brushes.Transparent;
            button.BorderBrush = Brushes.Transparent;
            button.Foreground = Theme.AccentBrush;
            button.Padding = new Thickness(0, 2, 12, 2);
            button.Click += (s, e) =>
            {
                try
                {
                    System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url)
                    {
                        UseShellExecute = true,
                    });
                }
                catch (Exception ex) { Log.Error("Could not open " + url + ": " + ex.Message); }
            };
            return button;
        }

        private static UIElement LicenseRow(string title, string detail)
        {
            var row = new StackPanel { Margin = new Thickness(0, 8, 0, 0) };
            row.Children.Add(Theme.Text(title, 13));
            var body = Theme.Text(detail, 12, secondary: true);
            body.TextWrapping = TextWrapping.Wrap;
            row.Children.Add(body);
            return row;
        }

        public static UIElement BuildMultiMachine()
        {
            var stack = new StackPanel();
            var config = Program.Config;
            var mm = config.MultiMachine;

            var enabled = new CheckBox
            {
                Content = Theme.Text("Allow paired machines to connect"),
                IsChecked = mm.Enabled,
                Margin = new Thickness(0, 2, 0, 4),
            };
            enabled.Click += (s, e) =>
            {
                mm.Enabled = enabled.IsChecked == true;
                config.Save();
                PeerService.Shared.ApplyConfig();
            };
            stack.Children.Add(enabled);

            var nameRow = new StackPanel { Orientation = Orientation.Horizontal };
            nameRow.Children.Add(Theme.Text("Machine name  ", 12));
            var nameBox = new TextBox { MinWidth = 160, Text = mm.MachineName };
            nameBox.LostFocus += (s, e) => { mm.MachineName = nameBox.Text.Trim(); config.Save(); };
            nameRow.Children.Add(nameBox);
            nameRow.Children.Add(Theme.Text("   Port  ", 12));
            var portBox = new TextBox { MinWidth = 60, Text = mm.Port.ToString() };
            portBox.LostFocus += (s, e) =>
            {
                int port;
                if (int.TryParse(portBox.Text.Trim(), out port) && port > 0 && port < 65536)
                { mm.Port = port; config.Save(); PeerService.Shared.ApplyConfig(); }
            };
            nameRow.Children.Add(portBox);
            stack.Children.Add(nameRow);

            var hint = Theme.Text(
                "Machines pair once with a 6-digit code confirmed on both screens, then talk over TLS "
                + "with pinned identities. Use tailnet or LAN addresses only.", 11.5, secondary: true);
            hint.TextWrapping = TextWrapping.Wrap;
            hint.Margin = new Thickness(0, 4, 0, 8);
            stack.Children.Add(hint);

            foreach (var peerRef in mm.Peers.ToList())
            {
                var peer = peerRef;
                var peerRow = new StackPanel { Margin = new Thickness(0, 2, 0, 6) };
                var head = new StackPanel { Orientation = Orientation.Horizontal };
                head.Children.Add(Theme.Text(peer.Name, 12.5));
                var fp = Theme.Text("  " + peer.Fingerprint.Substring(0, Math.Min(12, peer.Fingerprint.Length)) + "…",
                                    11, secondary: true);
                fp.VerticalAlignment = VerticalAlignment.Center;
                head.Children.Add(fp);
                var remove = Theme.MakeButton("Remove");
                remove.Margin = new Thickness(10, 0, 0, 0);
                head.Children.Add(remove);
                peerRow.Children.Add(head);
                var detail = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 3, 0, 0) };
                detail.Children.Add(Theme.Text("Address  ", 11.5, secondary: true));
                var addressBox = new TextBox { MinWidth = 160, Text = peer.Address };
                addressBox.LostFocus += (s, e) => { peer.Address = addressBox.Text.Trim(); config.Save(); };
                detail.Children.Add(addressBox);
                var allowScreens = new CheckBox
                {
                    Content = Theme.Text("May see my screens", 11.5),
                    IsChecked = peer.AllowScreens,
                    Margin = new Thickness(12, 0, 0, 0),
                };
                allowScreens.Click += (s, e) => { peer.AllowScreens = allowScreens.IsChecked == true; config.Save(); };
                detail.Children.Add(allowScreens);
                var allowDeliver = new CheckBox
                {
                    Content = Theme.Text("May paste into me", 11.5),
                    IsChecked = peer.AllowDeliver,
                    Margin = new Thickness(12, 0, 0, 0),
                };
                allowDeliver.Click += (s, e) => { peer.AllowDeliver = allowDeliver.IsChecked == true; config.Save(); };
                detail.Children.Add(allowDeliver);
                peerRow.Children.Add(detail);
                remove.Click += (s, e) =>
                {
                    mm.Peers.Remove(peer);
                    config.Save();
                    stack.Children.Remove(peerRow);
                };
                stack.Children.Add(peerRow);
            }

            var pairRow = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 6, 0, 0) };
            var addressField = new TextBox { MinWidth = 200 };
            pairRow.Children.Add(Theme.Text("Other machine's address  ", 12));
            pairRow.Children.Add(addressField);
            var pairButton = Theme.MakeButton("Pair…");
            pairButton.Margin = new Thickness(8, 0, 0, 0);
            pairRow.Children.Add(pairButton);
            stack.Children.Add(pairRow);
            var pairStatus = Theme.Text("", 11.5, secondary: true);
            pairStatus.Margin = new Thickness(0, 4, 0, 0);
            stack.Children.Add(pairStatus);
            pairButton.Click += async (s, e) =>
            {
                var address = addressField.Text.Trim();
                if (address.Length == 0) return;
                pairButton.IsEnabled = false;
                pairStatus.Text = "Connecting…";
                try
                {
                    var peer = await PeerService.Shared.PairAsync(address, (code, answer) =>
                    {
                        var result = MessageBox.Show(
                            "Does the other machine show this code?\n\n"
                            + code.Substring(0, 3) + " " + code.Substring(3),
                            "VoiceVector pairing", MessageBoxButton.YesNo, MessageBoxImage.Question);
                        answer(result == MessageBoxResult.Yes);
                    });
                    if (!mm.Peers.Any(p => p.Fingerprint == peer.Fingerprint))
                    {
                        mm.Peers.Add(peer);
                        config.Save();
                    }
                    pairStatus.Text = "Paired with " + peer.Name + ". Reopen Settings to manage it.";
                    addressField.Text = "";
                }
                catch (Exception ex)
                {
                    pairStatus.Text = "Pairing failed: " + ex.Message;
                }
                pairButton.IsEnabled = true;
            };

            var footer = Theme.Text(
                "VoiceVector must be running (with connections allowed) on the other machine. "
                + "Start pairing from either side.", 11.5, secondary: true);
            footer.TextWrapping = TextWrapping.Wrap;
            footer.Margin = new Thickness(0, 4, 0, 0);
            stack.Children.Add(footer);
            return stack;
        }

        public static UIElement BuildAbout()
        {
            var stack = new StackPanel();
            var head = new StackPanel { Orientation = Orientation.Horizontal };
            head.Children.Add(Theme.Text("VoiceVector", 18));
            var version = Theme.Text("Version " + UpdateService.CurrentVersion, 12, secondary: true);
            version.Margin = new Thickness(10, 0, 0, 0);
            version.VerticalAlignment = VerticalAlignment.Bottom;
            head.Children.Add(version);
            stack.Children.Add(head);
            stack.Children.Add(Theme.Text("A Sammons Software LLC product.", 13));
            stack.Children.Add(Theme.Text("© 2026 Sammons Software LLC. All rights reserved.", 12, secondary: true));
            var links = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 4, 0, 10) };
            links.Children.Add(LinkButton("Website", "https://voicevector.sammons.io"));
            links.Children.Add(LinkButton("What's new",
                "https://github.com/Sammons/voicevector/blob/main/CHANGELOG.md"));
            links.Children.Add(LinkButton("Source on GitHub", "https://github.com/Sammons/voicevector"));
            stack.Children.Add(links);

            stack.Children.Add(Theme.Text("Licensing", 11.5, secondary: true));
            var intro = Theme.Text(
                "VoiceVector is source-available under the VoiceVector Community License.", 13);
            intro.TextWrapping = TextWrapping.Wrap;
            stack.Children.Add(intro);
            stack.Children.Add(LicenseRow("Free",
                "For individuals, and for any company or organization with fewer than 1,000 "
                + "employees and contractors (counted together with affiliates). Commercial use included."));
            stack.Children.Add(LicenseRow("Commercial license — US $50 per seat per year",
                "Required for organizations with 1,000 or more employees and contractors. A seat is one "
                + "person who uses VoiceVector for their work. Organizations that cross the threshold get "
                + "a 90-day grace period; site licenses are available."));
            stack.Children.Add(LicenseRow("The same software for everyone",
                "No feature differences, no license keys, no telemetry. Compliance rests with the "
                + "organization, like any other license in its software inventory."));
            var licenseLinks = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 8, 0, 0) };
            licenseLinks.Children.Add(LinkButton("License text",
                "https://github.com/Sammons/voicevector/blob/main/LICENSE"));
            licenseLinks.Children.Add(LinkButton("Commercial terms",
                "https://github.com/Sammons/voicevector/blob/main/COMMERCIAL.md"));
            licenseLinks.Children.Add(LinkButton("Buy a commercial license",
                "mailto:sales@sammons.io?subject=VoiceVector%20commercial%20license"));
            stack.Children.Add(licenseLinks);
            return stack;
        }

        public static UIElement BuildGeneral(MainWindow owner)
        {
            var stack = new StackPanel();
            var config = Program.Config;

            var sounds = new CheckBox
            {
                Content = Theme.Text("Play chime when recording starts/stops"),
                IsChecked = config.PlaySounds,
                Margin = new Thickness(0, 2, 0, 4),
            };
            sounds.Click += (s, e) => { config.PlaySounds = sounds.IsChecked == true; config.Save(); };
            stack.Children.Add(sounds);

            var micHint = Theme.Text(
                "Opening an external audio interface can take half a second before recording starts. "
                + "Keeping the microphone open avoids that, but Windows shows its microphone-in-use "
                + "indicator while it's open.", 11.5, secondary: true);
            micHint.TextWrapping = TextWrapping.Wrap;
            micHint.Margin = new Thickness(0, 6, 0, 2);
            stack.Children.Add(micHint);
            var warmAfter = new CheckBox
            {
                Content = Theme.Text("Keep the microphone open for 15 seconds after a recording"),
                IsChecked = config.KeepMicWarmAfterRecording,
                Margin = new Thickness(0, 2, 0, 2),
            };
            warmAfter.Click += (s, e) =>
            {
                config.KeepMicWarmAfterRecording = warmAfter.IsChecked == true;
                config.Save();
                Program.Dictation.ApplyWarmPolicy();
            };
            stack.Children.Add(warmAfter);
            var warmAlways = new CheckBox
            {
                Content = Theme.Text("Always keep the microphone open while VoiceVector is running"),
                IsChecked = config.KeepMicAlwaysWarm,
                Margin = new Thickness(0, 2, 0, 6),
            };
            warmAlways.Click += (s, e) =>
            {
                config.KeepMicAlwaysWarm = warmAlways.IsChecked == true;
                config.Save();
                Program.Dictation.ApplyWarmPolicy();
            };
            stack.Children.Add(warmAlways);

            var paste = new CheckBox
            {
                Content = Theme.Text("Paste transcript into the active app"),
                IsChecked = config.AutoPaste,
                Margin = new Thickness(0, 0, 0, 10),
            };
            paste.Click += (s, e) => { config.AutoPaste = paste.IsChecked == true; config.Save(); };
            stack.Children.Add(paste);

            // Updates.
            var updateRow = new StackPanel { Orientation = Orientation.Horizontal };
            var version = Theme.Text("Version " + UpdateService.CurrentVersion, 12.5, secondary: true);
            version.VerticalAlignment = VerticalAlignment.Center;
            version.Margin = new Thickness(0, 0, 10, 0);
            updateRow.Children.Add(version);
            var updateStatus = Theme.Text("", 11.5, secondary: true);
            updateStatus.Margin = new Thickness(0, 4, 0, 0);
            var updateButton = Theme.MakeButton("Check for Updates");
            UpdateService.UpdateInfo pending = null;
            updateButton.Click += async (s, e) =>
            {
                updateButton.IsEnabled = false;
                try
                {
                    if (pending == null)
                    {
                        pending = await UpdateService.FetchLatestAsync();
                        if (pending == null)
                        {
                            updateStatus.Text = "You're up to date.";
                        }
                        else
                        {
                            updateStatus.Text = "Version " + pending.Version + " is available.";
                            updateButton.Content = "Update to " + pending.Version + " & Restart";
                        }
                    }
                    else
                    {
                        updateStatus.Text = "Downloading " + pending.Version + "…";
                        await UpdateService.DownloadAndInstallAsync(pending);
                    }
                }
                catch (Exception ex)
                {
                    updateStatus.Text = "Update failed: " + ex.Message;
                }
                finally { updateButton.IsEnabled = true; }
            };
            updateRow.Children.Add(updateButton);
            stack.Children.Add(updateRow);
            stack.Children.Add(updateStatus);

            // Storage.
            var pathRow = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Margin = new Thickness(0, 10, 0, 0),
            };
            var path = Theme.Text(config.ExpandedLibraryPath, 12, secondary: true);
            path.FontFamily = Theme.MonoFont;
            path.VerticalAlignment = VerticalAlignment.Center;
            path.Margin = new Thickness(0, 0, 10, 0);
            pathRow.Children.Add(path);
            var open = Theme.MakeButton("Open in Explorer");
            open.Click += (s, e) =>
            {
                try { System.Diagnostics.Process.Start("explorer.exe", config.ExpandedLibraryPath); }
                catch (Exception ex) { Log.Error("Explorer open failed: " + ex.Message); }
            };
            pathRow.Children.Add(open);
            stack.Children.Add(pathRow);

            var rerun = Theme.MakeButton("Run setup wizard again");
            rerun.Background = Brushes.Transparent;
            rerun.BorderBrush = Brushes.Transparent;
            rerun.Foreground = Theme.AccentBrush;
            rerun.HorizontalAlignment = HorizontalAlignment.Left;
            rerun.Margin = new Thickness(0, 8, 0, 0);
            rerun.Click += (s, e) =>
            {
                config.WizardCompleted = false;
                config.Save();
                owner.CloseSettings();
            };
            stack.Children.Add(rerun);

            if (Log.RecentErrors.Count > 0)
            {
                var errorsTitle = Theme.SectionTitle("Recent errors");
                errorsTitle.Margin = new Thickness(0, 12, 0, 2);
                stack.Children.Add(errorsTitle);
                foreach (var line in Log.RecentErrors.Reverse().Take(8))
                {
                    var text = Theme.Text(line, 11, secondary: true);
                    text.FontFamily = Theme.MonoFont;
                    stack.Children.Add(text);
                }
            }
            return stack;
        }
    }
}
