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
    /// <summary>
    /// The one window: header, notice bar, and a content area showing the
    /// wizard (first run), the library, or settings. Closing hides to the
    /// tray. Code-built WPF, Fluent-by-hand via Theme.
    /// </summary>
    public sealed class MainWindow : Window
    {
        private readonly TextBlock _statusLine;
        private readonly Button _recordButton;
        private readonly Border _noticeBar;
        private readonly TextBlock _noticeText;
        private readonly ContentControl _content;
        private HudWindow _hud;
        private System.Windows.Forms.NotifyIcon _tray;
        private bool _showingSettings;
        private int _wizardStep;
        private readonly Dictionary<string, int> _shownCounts = new Dictionary<string, int>();
        private readonly HashSet<string> _collapsedFolders = new HashSet<string>();
        private string _expandedEntryId;
        private const int PageSize = 25;

        public MainWindow()
        {
            Title = "VoiceVector";
            Width = 760;
            Height = 680;
            MinWidth = 560;
            MinHeight = 480;
            Background = Theme.WindowBackground;
            FontFamily = Theme.UiFont;
            Theme.ApplyChrome(this);

            _statusLine = Theme.Text("", 12, secondary: true);
            _recordButton = Theme.MakeButton("Record", prominent: true);
            _recordButton.Click += (s, e) => ToggleDictation();
            _noticeText = Theme.Text("", 12.5);
            _noticeBar = new Border { Visibility = Visibility.Collapsed };
            _content = new ContentControl
            {
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
                VerticalContentAlignment = VerticalAlignment.Stretch,
            };

            // CI bisect ladder: VV_UI_LEVEL 1=bare text, 2=chrome, 3=+content,
            // 4=+tray, 5/unset=full app.
            int uiLevel;
            if (!int.TryParse(Environment.GetEnvironmentVariable("VV_UI_LEVEL"), out uiLevel))
                uiLevel = 99;
            int step;
            if (int.TryParse(Environment.GetEnvironmentVariable("VV_WIZARD_STEP"), out step))
                _wizardStep = step;

            if (uiLevel <= 1)
            {
                var probe = Theme.Text("minimal", 14);
                probe.Margin = new Thickness(40);
                probe.Loaded += (s, e) => Diag.Breadcrumb("minimal content loaded");
                Content = probe;
                return;
            }

            var root = BuildRoot();
            root.Loaded += (s, e) => Diag.Breadcrumb("content loaded");
            Content = root;

            if (uiLevel >= 3) RefreshContent();
            else _content.Content = Theme.Text("level 2", 14);

            if (uiLevel >= 4) SetUpTray();

            if (uiLevel >= 5)
            {
                Program.Dictation.StateChanged += OnDictationState;
                Program.Dictation.LibraryChanged += RefreshContent;
                Program.Dictation.Notice += m => ShowNotice(m, warning: true);
                Closing += (s, e) =>
                {
                    // Close = hide to tray; quit lives in the tray menu.
                    e.Cancel = true;
                    Hide();
                };
            }
            OnDictationState();
        }

        // -- skeleton ----------------------------------------------------------

        private Grid BuildRoot()
        {
            var root = new Grid();
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            // Header
            var header = new Grid { Margin = new Thickness(20, 16, 20, 12) };
            header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var titleStack = new StackPanel();
            var titleRow = new StackPanel { Orientation = Orientation.Horizontal };
            titleRow.Children.Add(Theme.Icon("", 22, Theme.AccentBrush)); // microphone
            var title = new TextBlock
            {
                Text = "VoiceVector",
                FontFamily = Theme.DisplayFont,
                FontSize = 20,
                FontWeight = FontWeights.Bold,
                Foreground = Theme.TextPrimary,
                Margin = new Thickness(10, 0, 0, 0),
                VerticalAlignment = VerticalAlignment.Center,
            };
            titleRow.Children.Add(title);
            titleStack.Children.Add(titleRow);
            _statusLine.Margin = new Thickness(32, 2, 0, 0);
            titleStack.Children.Add(_statusLine);
            header.Children.Add(titleStack);

            var buttons = new StackPanel { Orientation = Orientation.Horizontal };
            _recordButton.Margin = new Thickness(0, 0, 8, 0);
            buttons.Children.Add(_recordButton);
            var settings = Theme.MakeButton("", icon: ""); // gear
            settings.Click += (s, e) => { _showingSettings = !_showingSettings; RefreshContent(); };
            buttons.Children.Add(settings);
            Grid.SetColumn(buttons, 1);
            header.Children.Add(buttons);
            root.Children.Add(header);

            // Notice bar
            var noticeGrid = new Grid { Margin = new Thickness(14, 8, 10, 8) };
            noticeGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            noticeGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            noticeGrid.Children.Add(_noticeText);
            var dismiss = Theme.MakeButton("", icon: ""); // ✕
            dismiss.Padding = new Thickness(6, 2, 6, 2);
            dismiss.Click += (s, e) => _noticeBar.Visibility = Visibility.Collapsed;
            Grid.SetColumn(dismiss, 1);
            noticeGrid.Children.Add(dismiss);
            _noticeBar.Child = noticeGrid;
            Grid.SetRow(_noticeBar, 1);
            root.Children.Add(_noticeBar);

            Grid.SetRow(_content, 2);
            root.Children.Add(_content);

            // Footer
            var footer = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Margin = new Thickness(20, 8, 20, 14),
            };
            footer.Children.Add(Theme.Text("New dictations go to", 12.5, secondary: true));
            var folderBox = new ComboBox
            {
                Margin = new Thickness(10, 0, 10, 0),
                MinWidth = 120,
                FontFamily = Theme.UiFont,
                VerticalAlignment = VerticalAlignment.Center,
            };
            foreach (var name in Program.Lib.FolderNames()) folderBox.Items.Add(name);
            folderBox.SelectedItem = Program.Config.ActiveFolder;
            folderBox.SelectionChanged += (s, e) =>
            {
                var name = folderBox.SelectedItem as string;
                if (name != null)
                {
                    Program.Config.ActiveFolder = name;
                    Program.Config.Save();
                }
            };
            footer.Children.Add(folderBox);
            var newFolder = Theme.MakeButton("New Folder", icon: "");
            newFolder.Click += (s, e) => PromptNewFolder();
            footer.Children.Add(newFolder);
            Grid.SetRow(footer, 3);
            root.Children.Add(footer);

            return root;
        }

        private void SetUpTray()
        {
            _tray = new System.Windows.Forms.NotifyIcon
            {
                Icon = System.Drawing.SystemIcons.Application,
                Text = "VoiceVector",
                Visible = true,
            };
            var menu = new System.Windows.Forms.ContextMenuStrip();
            menu.Items.Add("Start Dictation", null, (s, e) => Dispatcher.BeginInvoke(
                (Action)ToggleDictation));
            menu.Items.Add(new System.Windows.Forms.ToolStripSeparator());
            menu.Items.Add("Open VoiceVector", null, (s, e) => Dispatcher.BeginInvoke(
                (Action)(() => { Show(); Activate(); })));
            menu.Items.Add(new System.Windows.Forms.ToolStripSeparator());
            menu.Items.Add("Quit VoiceVector", null, (s, e) => Dispatcher.BeginInvoke(
                (Action)(() =>
                {
                    _tray.Visible = false;
                    _tray.Dispose();
                    Program.Hook.Stop();
                    Application.Current.Shutdown();
                })));
            menu.Opening += (s, e) =>
            {
                menu.Items[0].Text = Program.Dictation.IsRecording
                    ? "Stop Dictation" : "Start Dictation";
            };
            _tray.ContextMenuStrip = menu;
            _tray.DoubleClick += (s, e) => Dispatcher.BeginInvoke(
                (Action)(() => { Show(); Activate(); }));
            Diag.Breadcrumb("tray up");
        }

        private void ToggleDictation()
        {
            if (Program.Dictation.IsRecording) Program.Dictation.FinishRecording();
            else Program.Dictation.StartRecording();
        }

        private void OnDictationState()
        {
            var dictation = Program.Dictation;
            _recordButton.Content = dictation.IsRecording ? "Stop" : "Record";
            _recordButton.Background = dictation.IsRecording ? Theme.Danger : (Brush)Theme.AccentBrush;
            _recordButton.IsEnabled = dictation.State != DictationController.StateKind.Processing;
            switch (dictation.State)
            {
                case DictationController.StateKind.Recording:
                    _statusLine.Text = "Recording…";
                    EnsureHud().ShowHud("Listening…", dictation.ReviewDraft, dictation.ReviewRoute);
                    break;
                case DictationController.StateKind.Processing:
                    _statusLine.Text = dictation.StateDetail;
                    EnsureHud().ShowHud(dictation.StateDetail, dictation.ReviewDraft, dictation.ReviewRoute);
                    break;
                case DictationController.StateKind.Reviewing:
                    _statusLine.Text = "Reviewing — press the hotkey to say a change, Enter to paste, Esc to discard";
                    EnsureHud().ShowHud("Reviewing", dictation.ReviewDraft, dictation.ReviewRoute);
                    break;
                case DictationController.StateKind.Failed:
                    _statusLine.Text = dictation.StateDetail;
                    ShowNotice(dictation.StateDetail, warning: true);
                    if (_hud != null) _hud.HideHud();
                    break;
                default:
                    _statusLine.Text = "Press " + KeyboardHook.Describe(Program.Config.PrimaryHotkey)
                                       + " anywhere to dictate";
                    if (_hud != null) _hud.HideHud();
                    break;
            }
            Program.Hook.ReviewActive = dictation.State == DictationController.StateKind.Reviewing;
            if (_tray != null)
                _tray.Text = dictation.IsRecording ? "VoiceVector — recording" : "VoiceVector";
        }

        private HudWindow EnsureHud()
        {
            if (_hud == null) _hud = new HudWindow();
            return _hud;
        }

        public void ShowNotice(string message, bool warning)
        {
            _noticeText.Text = message;
            _noticeBar.Background = new SolidColorBrush(warning
                ? Color.FromArgb(36, 224, 158, 42)
                : Color.FromArgb(30, Theme.Accent.R, Theme.Accent.G, Theme.Accent.B));
            _noticeBar.Visibility = Visibility.Visible;
        }

        public void RefreshContent()
        {
            OnDictationState();
            if (!Program.Config.WizardCompleted) _content.Content = WizardUi.Build(this, _wizardStep);
            else if (_showingSettings) _content.Content = SettingsUi.Build(this);
            else _content.Content = BuildLibrary();
        }

        public void CloseSettings()
        {
            _showingSettings = false;
            RefreshContent();
        }

        public void SetWizardStep(int step)
        {
            _wizardStep = step;
            RefreshContent();
        }

        // -- library -----------------------------------------------------------

        private UIElement BuildLibrary()
        {
            var list = new StackPanel { Margin = new Thickness(16, 4, 16, 16) };
            foreach (var folder in Program.Lib.FolderNames())
                list.Children.Add(BuildFolderSection(folder));
            return new ScrollViewer
            {
                Content = list,
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            };
        }

        private UIElement BuildFolderSection(string folder)
        {
            int total = Program.Lib.EntryCount(folder);
            int shown = _shownCounts.ContainsKey(folder) ? _shownCounts[folder] : PageSize;
            bool collapsed = _collapsedFolders.Contains(folder);

            var section = new StackPanel { Margin = new Thickness(0, 0, 0, 6) };
            var headerButton = Theme.MakeButton(
                (collapsed ? "▸  " : "▾  ") + folder.ToUpperInvariant() + "   ·   " + total);
            headerButton.HorizontalAlignment = HorizontalAlignment.Stretch;
            headerButton.HorizontalContentAlignment = HorizontalAlignment.Left;
            headerButton.Background = Brushes.Transparent;
            headerButton.BorderBrush = Brushes.Transparent;
            headerButton.Foreground = Theme.TextTertiary;
            headerButton.FontSize = 12;
            headerButton.FontWeight = FontWeights.SemiBold;
            headerButton.Click += (s, e) =>
            {
                if (!_collapsedFolders.Remove(folder)) _collapsedFolders.Add(folder);
                RefreshContent();
            };
            section.Children.Add(headerButton);

            if (!collapsed)
            {
                if (total == 0)
                {
                    var empty = Theme.Text("No dictations yet", 13, secondary: true);
                    empty.Margin = new Thickness(12, 2, 0, 6);
                    section.Children.Add(empty);
                }
                else
                {
                    foreach (var entry in Program.Lib.Entries(folder, 0, shown))
                        section.Children.Add(BuildEntryRow(entry));
                    if (shown < total)
                    {
                        var more = Theme.MakeButton("Show more (" + (total - shown) + " older)");
                        more.Background = Brushes.Transparent;
                        more.BorderBrush = Brushes.Transparent;
                        more.Foreground = Theme.AccentBrush;
                        more.Click += (s, e) => { _shownCounts[folder] = shown + PageSize; RefreshContent(); };
                        section.Children.Add(more);
                    }
                }
            }
            return section;
        }

        private UIElement BuildEntryRow(Entry entry)
        {
            bool expanded = _expandedEntryId == entry.Id;
            var container = new StackPanel { Margin = new Thickness(4, 2, 4, 2) };

            var row = new Grid { Margin = new Thickness(8, 6, 8, 6) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var time = new TextBlock
            {
                Text = entry.Date.LocalDateTime.ToString("MMM d  HH:mm"),
                FontFamily = Theme.MonoFont,
                FontSize = 11.5,
                Foreground = Theme.TextTertiary,
                Margin = new Thickness(0, 0, 10, 0),
                VerticalAlignment = VerticalAlignment.Center,
            };
            row.Children.Add(time);

            string badge = entry.IsError ? "failed"
                : entry.CleanupLabel.Contains("failed") ? "cleanup failed"
                : entry.CleanupLabel.StartsWith("not run") ? "raw" : null;
            if (badge != null)
            {
                var pill = Theme.Pill(badge, entry.IsError ? Theme.Danger : Theme.TextSecondary);
                pill.Margin = new Thickness(0, 0, 8, 0);
                Grid.SetColumn(pill, 1);
                row.Children.Add(pill);
            }

            var preview = new TextBlock
            {
                Text = (entry.Cleaned.Length > 0 ? entry.Cleaned : entry.Status).Replace('\n', ' '),
                FontFamily = Theme.UiFont,
                FontSize = 13,
                Foreground = Theme.TextPrimary,
                TextTrimming = TextTrimming.CharacterEllipsis,
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetColumn(preview, 2);
            row.Children.Add(preview);

            var duration = new TextBlock
            {
                Text = entry.Duration >= 60
                    ? (int)entry.Duration / 60 + ":" + ((int)entry.Duration % 60).ToString("D2")
                    : entry.Duration.ToString("F0") + "s",
                FontSize = 11,
                Foreground = Theme.TextTertiary,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(8, 0, 0, 0),
            };
            Grid.SetColumn(duration, 3);
            row.Children.Add(duration);

            var rowButton = new Button
            {
                Content = row,
                HorizontalAlignment = HorizontalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
                Background = Brushes.Transparent,
                BorderBrush = Brushes.Transparent,
                Cursor = System.Windows.Input.Cursors.Hand,
                Padding = new Thickness(0),
                Template = TransparentButtonTemplate(),
            };
            rowButton.Click += (s, e) =>
            {
                _expandedEntryId = expanded ? null : entry.Id;
                RefreshContent();
            };
            container.Children.Add(rowButton);

            if (expanded)
            {
                var detail = new StackPanel { Margin = new Thickness(8, 0, 8, 8) };
                if (entry.IsError)
                {
                    var error = Theme.Text(entry.Status, 13);
                    error.Foreground = Theme.Danger;
                    detail.Children.Add(error);
                }
                if (entry.Cleaned.Length > 0)
                    detail.Children.Add(TranscriptBlock("Cleaned", entry.Cleaned));
                if (entry.Raw.Length > 0 && entry.Raw != entry.Cleaned)
                    detail.Children.Add(TranscriptBlock("Raw", entry.Raw));

                var meta = new List<string>();
                if (entry.SttLabel.Length > 0) meta.Add("STT: " + entry.SttLabel);
                if (entry.CleanupLabel.Length > 0) meta.Add("Cleanup: " + entry.CleanupLabel);
                if (meta.Count > 0)
                {
                    var metaText = Theme.Text(string.Join("   ·   ", meta), 11, secondary: true);
                    metaText.Margin = new Thickness(0, 4, 0, 6);
                    detail.Children.Add(metaText);
                }

                var actions = new StackPanel { Orientation = Orientation.Horizontal };
                if (entry.IsError || entry.CleanupLabel.Contains("failed"))
                {
                    var retry = Theme.MakeButton("Retry", icon: "");
                    retry.Margin = new Thickness(0, 0, 8, 0);
                    var captured = entry;
                    retry.Click += (s, e) => Program.Dictation.Retry(captured);
                    actions.Children.Add(retry);
                }
                var reveal = Theme.MakeButton("Show Files", icon: "");
                reveal.Margin = new Thickness(0, 0, 8, 0);
                var target = Program.Lib.AudioPath(entry);
                reveal.Click += (s, e) =>
                {
                    try
                    {
                        System.Diagnostics.Process.Start("explorer.exe", "/select,\"" + target + "\"");
                    }
                    catch (Exception ex) { Log.Error("Explorer open failed: " + ex.Message); }
                };
                actions.Children.Add(reveal);
                var delete = Theme.MakeButton("Delete", icon: "");
                var toDelete = entry;
                delete.Click += (s, e) =>
                {
                    Program.Lib.Delete(toDelete);
                    _expandedEntryId = null;
                    RefreshContent();
                };
                actions.Children.Add(delete);
                detail.Children.Add(actions);
                container.Children.Add(detail);
            }

            return new Border
            {
                Background = expanded ? (Brush)Theme.AccentSoft : Brushes.Transparent,
                CornerRadius = new CornerRadius(8),
                Child = container,
            };
        }

        private UIElement TranscriptBlock(string title, string text)
        {
            var stack = new StackPanel();
            var head = new Grid { Margin = new Thickness(0, 0, 0, 4) };
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            head.Children.Add(Theme.SectionTitle(title));
            var copy = Theme.MakeButton("Copy", icon: "");
            copy.Padding = new Thickness(8, 3, 8, 3);
            copy.FontSize = 11;
            var payload = text;
            copy.Click += (s, e) =>
            {
                try { Clipboard.SetText(payload); } catch { }
            };
            Grid.SetColumn(copy, 1);
            head.Children.Add(copy);
            stack.Children.Add(head);
            var body = Theme.Text(text, 13);
            stack.Children.Add(body);
            var card = Theme.Card(stack);
            card.Margin = new Thickness(0, 4, 0, 4);
            return card;
        }

        private static ControlTemplate TransparentButtonTemplate()
        {
            var presenter = new FrameworkElementFactory(typeof(ContentPresenter));
            return new ControlTemplate(typeof(Button)) { VisualTree = presenter };
        }

        private void PromptNewFolder()
        {
            var input = Theme.MakeTextBox();
            var create = Theme.MakeButton("Create", prominent: true);
            var panel = new StackPanel { Margin = new Thickness(16) };
            panel.Children.Add(Theme.Text("New folder", 15));
            input.Margin = new Thickness(0, 10, 0, 10);
            input.MinWidth = 220;
            panel.Children.Add(input);
            panel.Children.Add(create);

            var dialog = new Window
            {
                Title = "New folder",
                Content = panel,
                SizeToContent = SizeToContent.WidthAndHeight,
                WindowStartupLocation = WindowStartupLocation.CenterOwner,
                Owner = this,
                Background = Theme.WindowBackground,
                ResizeMode = ResizeMode.NoResize,
            };
            Theme.ApplyChrome(dialog);
            create.Click += (s, e) =>
            {
                Program.Lib.CreateFolder(input.Text);
                dialog.Close();
                RefreshContent();
            };
            input.KeyDown += (s, e) =>
            {
                if (e.Key == System.Windows.Input.Key.Enter)
                {
                    Program.Lib.CreateFolder(input.Text);
                    dialog.Close();
                    RefreshContent();
                }
            };
            dialog.ShowDialog();
        }
    }
}
