using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
using VoiceVector.Win.Services;

namespace VoiceVector.Win
{
    /// <summary>
    /// Floating recording pill near the bottom of the screen: borderless,
    /// topmost, WS_EX_NOACTIVATE so focus never leaves the target app.
    /// </summary>
    public sealed class HudWindow : Window
    {
        private readonly TextBlock _label;
        private readonly TextBlock _clock;
        private readonly Border _staging;
        private readonly TextBlock _draft;
        private readonly TextBlock _reviewHint;
        private readonly StackPanel _bars;
        private readonly Rectangle[] _barShapes = new Rectangle[14];
        private readonly double[] _levels = new double[14];
        private readonly DispatcherTimer _timer;

        public HudWindow()
        {
            WindowStyle = WindowStyle.None;
            AllowsTransparency = true;
            Background = Brushes.Transparent;
            Topmost = true;
            ShowInTaskbar = false;
            ShowActivated = false;
            Width = 520;
            Height = 300; // room for the review staging card; unused area is transparent
            ResizeMode = ResizeMode.NoResize;

            _bars = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                VerticalAlignment = VerticalAlignment.Center,
                Height = 30,
            };
            for (int i = 0; i < _barShapes.Length; i++)
            {
                var bar = new Rectangle
                {
                    Width = 3,
                    Height = 6,
                    RadiusX = 1.5,
                    RadiusY = 1.5,
                    Fill = Theme.AccentBrush,
                    Margin = new Thickness(0, 0, 3, 0),
                    VerticalAlignment = VerticalAlignment.Center,
                };
                _barShapes[i] = bar;
                _bars.Children.Add(bar);
            }
            _label = new TextBlock
            {
                Text = "Listening…",
                FontFamily = Theme.UiFont,
                FontSize = 13,
                FontWeight = FontWeights.Medium,
                Foreground = new SolidColorBrush(Color.FromRgb(240, 238, 248)),
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(10, 0, 10, 0),
            };
            _clock = new TextBlock
            {
                FontFamily = Theme.MonoFont,
                FontSize = 13,
                Foreground = new SolidColorBrush(Color.FromArgb(180, 240, 238, 248)),
                VerticalAlignment = VerticalAlignment.Center,
            };

            var row = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            };
            row.Children.Add(_bars);
            row.Children.Add(_label);
            row.Children.Add(_clock);

            var pill = new Border
            {
                Background = new SolidColorBrush(Color.FromArgb(235, 32, 30, 42)),
                BorderBrush = new SolidColorBrush(Color.FromArgb(90, Theme.Accent.R,
                                                                 Theme.Accent.G, Theme.Accent.B)),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(24),
                Child = row,
                Width = 300,
                Height = 60,
                HorizontalAlignment = HorizontalAlignment.Center,
            };

            _draft = new TextBlock
            {
                FontFamily = Theme.UiFont,
                FontSize = 14,
                Foreground = new SolidColorBrush(Color.FromRgb(240, 238, 248)),
                TextWrapping = TextWrapping.Wrap,
            };
            _reviewHint = new TextBlock
            {
                FontFamily = Theme.UiFont,
                FontSize = 11.5,
                Foreground = new SolidColorBrush(Color.FromArgb(190, 240, 238, 248)),
                Margin = new Thickness(0, 8, 0, 0),
            };
            var stagingBody = new StackPanel();
            stagingBody.Children.Add(new ScrollViewer
            {
                Content = _draft,
                MaxHeight = 170,
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            });
            stagingBody.Children.Add(_reviewHint);
            _staging = new Border
            {
                Background = new SolidColorBrush(Color.FromArgb(240, 32, 30, 42)),
                BorderBrush = new SolidColorBrush(Color.FromArgb(90, Theme.Accent.R,
                                                                 Theme.Accent.G, Theme.Accent.B)),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(14),
                Padding = new Thickness(14),
                Margin = new Thickness(0, 0, 0, 10),
                Child = stagingBody,
                Visibility = Visibility.Collapsed,
            };

            var stack = new StackPanel { VerticalAlignment = VerticalAlignment.Bottom };
            stack.Children.Add(_staging);
            stack.Children.Add(pill);
            Content = stack;

            SourceInitialized += (s, e) =>
            {
                var hwnd = new WindowInteropHelper(this).Handle;
                var exStyle = Native.GetWindowLongPtrW(hwnd, Native.GWL_EXSTYLE).ToInt64();
                Native.SetWindowLongPtrW(hwnd, Native.GWL_EXSTYLE,
                    new IntPtr(exStyle | Native.WS_EX_NOACTIVATE | Native.WS_EX_TOOLWINDOW));
            };

            _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(50) };
            _timer.Tick += (s, e) => Tick();
        }

        public void ShowHud(string label)
        {
            ShowHud(label, null);
        }

        public void ShowHud(string label, string draft)
        {
            _label.Text = label;
            if (draft != null)
            {
                _draft.Text = draft;
                _reviewHint.Text = (label == "Reviewing"
                    ? "Press the hotkey and say a change"
                    : label) + "      Enter: paste   Esc: discard";
                _staging.Visibility = Visibility.Visible;
            }
            else
            {
                _staging.Visibility = Visibility.Collapsed;
            }
            bool recording = label == "Listening…";
            _bars.Visibility = recording ? Visibility.Visible : Visibility.Collapsed;
            _clock.Visibility = recording ? Visibility.Visible : Visibility.Collapsed;
            var area = SystemParameters.WorkArea;
            Left = area.Left + (area.Width - Width) / 2;
            Top = area.Bottom - Height - 84;
            Show();
            _timer.Start();
        }

        public void HideHud()
        {
            _timer.Stop();
            Hide();
        }

        private void Tick()
        {
            if (!Program.Dictation.Recorder.IsRecording) return;
            Array.Copy(_levels, 1, _levels, 0, _levels.Length - 1);
            _levels[_levels.Length - 1] = Math.Max(0.12, Program.Dictation.Recorder.Level);
            for (int i = 0; i < _barShapes.Length; i++)
                _barShapes[i].Height = 6 + _levels[i] * 22;
            var elapsed = Program.Dictation.Recorder.Elapsed;
            _clock.Text = (int)elapsed.TotalMinutes + ":" + elapsed.Seconds.ToString("D2");
        }
    }
}
