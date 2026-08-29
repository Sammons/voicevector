using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using Microsoft.Win32;
using VoiceVector.Win.Services;

namespace VoiceVector.Win
{
    /// <summary>
    /// Fluent-by-hand, from OS built-ins only: the VoiceVector violet accent,
    /// Segoe UI Variable / Segoe Fluent Icons (ship with Win11), DWM dark
    /// title bars, Mica backdrop and rounded corners where available.
    /// </summary>
    public static class Theme
    {
        public static readonly Color Accent = Color.FromRgb(115, 89, 242);
        public static bool IsDark { get; private set; }

        // Palette (set by Refresh()).
        public static SolidColorBrush AccentBrush;
        public static SolidColorBrush AccentSoft;
        public static SolidColorBrush WindowBackground;
        public static SolidColorBrush CardBackground;
        public static SolidColorBrush TextPrimary;
        public static SolidColorBrush TextSecondary;
        public static SolidColorBrush TextTertiary;
        public static SolidColorBrush Danger;
        public static SolidColorBrush Divider;

        public static readonly FontFamily UiFont =
            new FontFamily("Segoe UI Variable Text, Segoe UI");
        public static readonly FontFamily DisplayFont =
            new FontFamily("Segoe UI Variable Display, Segoe UI");
        public static readonly FontFamily IconFont =
            new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets");
        public static readonly FontFamily MonoFont =
            new FontFamily("Cascadia Mono, Consolas");

        static Theme()
        {
            Refresh();
        }

        public static void Refresh()
        {
            IsDark = DetectDark();
            AccentBrush = Freeze(new SolidColorBrush(Accent));
            AccentSoft = Freeze(new SolidColorBrush(Color.FromArgb(IsDark ? (byte)46 : (byte)30,
                Accent.R, Accent.G, Accent.B)));
            WindowBackground = Freeze(new SolidColorBrush(
                IsDark ? Color.FromRgb(26, 24, 34) : Color.FromRgb(248, 247, 252)));
            CardBackground = Freeze(new SolidColorBrush(
                IsDark ? Color.FromArgb(255, 38, 36, 48) : Colors.White));
            TextPrimary = Freeze(new SolidColorBrush(
                IsDark ? Color.FromRgb(240, 238, 248) : Color.FromRgb(28, 26, 36)));
            TextSecondary = Freeze(new SolidColorBrush(
                IsDark ? Color.FromArgb(190, 240, 238, 248) : Color.FromArgb(170, 28, 26, 36)));
            TextTertiary = Freeze(new SolidColorBrush(
                IsDark ? Color.FromArgb(120, 240, 238, 248) : Color.FromArgb(110, 28, 26, 36)));
            Danger = Freeze(new SolidColorBrush(Color.FromRgb(217, 76, 76)));
            Divider = Freeze(new SolidColorBrush(
                IsDark ? Color.FromArgb(30, 255, 255, 255) : Color.FromArgb(24, 0, 0, 0)));
        }

        private static bool DetectDark()
        {
            try
            {
                using (var key = Registry.CurrentUser.OpenSubKey(
                    @"SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize"))
                {
                    var value = key != null ? key.GetValue("AppsUseLightTheme") : null;
                    return value is int && (int)value == 0;
                }
            }
            catch
            {
                return false;
            }
        }

        private static SolidColorBrush Freeze(SolidColorBrush brush)
        {
            brush.Freeze();
            return brush;
        }

        /// <summary>Dark title bar + Mica + rounded corners, when the OS has them.</summary>
        public static void ApplyChrome(Window window)
        {
            window.SourceInitialized += (s, e) =>
            {
                var hwnd = new WindowInteropHelper(window).Handle;
                int dark = IsDark ? 1 : 0;
                Native.DwmSetWindowAttribute(hwnd, Native.DWMWA_USE_IMMERSIVE_DARK_MODE,
                                             ref dark, sizeof(int));
                int corner = Native.DWMWCP_ROUND;
                Native.DwmSetWindowAttribute(hwnd, Native.DWMWA_WINDOW_CORNER_PREFERENCE,
                                             ref corner, sizeof(int));
                int backdrop = Native.DWMSBT_MAINWINDOW;
                Native.DwmSetWindowAttribute(hwnd, Native.DWMWA_SYSTEMBACKDROP_TYPE,
                                             ref backdrop, sizeof(int));
            };
        }

        // -- element factories -------------------------------------------------

        public static TextBlock Text(string content, double size = 13, bool secondary = false)
        {
            return new TextBlock
            {
                Text = content,
                FontFamily = UiFont,
                FontSize = size,
                Foreground = secondary ? TextSecondary : TextPrimary,
                TextWrapping = TextWrapping.Wrap,
            };
        }

        public static TextBlock SectionTitle(string content)
        {
            return new TextBlock
            {
                Text = content.ToUpperInvariant(),
                FontFamily = UiFont,
                FontSize = 11.5,
                FontWeight = FontWeights.SemiBold,
                Foreground = TextTertiary,
                Margin = new Thickness(0, 0, 0, 2),
            };
        }

        public static TextBlock Icon(string glyph, double size = 16, Brush brush = null)
        {
            return new TextBlock
            {
                Text = glyph,
                FontFamily = IconFont,
                FontSize = size,
                Foreground = brush ?? TextPrimary,
                VerticalAlignment = VerticalAlignment.Center,
            };
        }

        public static Border Card(UIElement child)
        {
            return new Border
            {
                Background = CardBackground,
                CornerRadius = new CornerRadius(10),
                BorderBrush = Divider,
                BorderThickness = new Thickness(1),
                Padding = new Thickness(16),
                Child = child,
                Margin = new Thickness(0, 0, 0, 12),
            };
        }

        /// <summary>Rounded flat button (WPF's default chrome is Win95-adjacent;
        /// this template is the Fluent look, hover state included).</summary>
        public static Button MakeButton(string label, bool prominent = false, string icon = null)
        {
            var button = new Button
            {
                FontFamily = UiFont,
                FontSize = 13,
                Padding = new Thickness(14, 7, 14, 7),
                Cursor = System.Windows.Input.Cursors.Hand,
                Foreground = prominent ? Brushes.White : TextPrimary,
                Background = prominent ? (Brush)AccentBrush : CardBackground,
                BorderBrush = prominent ? (Brush)AccentBrush : Divider,
            };
            if (icon == null)
            {
                button.Content = label;
            }
            else
            {
                var row = new StackPanel { Orientation = Orientation.Horizontal };
                row.Children.Add(Icon(icon, 13, prominent ? Brushes.White : TextPrimary));
                var text = Text(label);
                text.Margin = new Thickness(7, 0, 0, 0);
                text.Foreground = prominent ? Brushes.White : TextPrimary;
                row.Children.Add(text);
                button.Content = row;
            }
            button.Template = RoundedButtonTemplate();
            return button;
        }

        private static ControlTemplate RoundedButtonTemplate()
        {
            var border = new FrameworkElementFactory(typeof(Border), "root");
            border.SetValue(Border.CornerRadiusProperty, new CornerRadius(8));
            border.SetBinding(Border.BackgroundProperty,
                new System.Windows.Data.Binding("Background")
                {
                    RelativeSource = new System.Windows.Data.RelativeSource(
                        System.Windows.Data.RelativeSourceMode.TemplatedParent),
                });
            border.SetBinding(Border.BorderBrushProperty,
                new System.Windows.Data.Binding("BorderBrush")
                {
                    RelativeSource = new System.Windows.Data.RelativeSource(
                        System.Windows.Data.RelativeSourceMode.TemplatedParent),
                });
            border.SetValue(Border.BorderThicknessProperty, new Thickness(1));
            border.SetBinding(Border.PaddingProperty,
                new System.Windows.Data.Binding("Padding")
                {
                    RelativeSource = new System.Windows.Data.RelativeSource(
                        System.Windows.Data.RelativeSourceMode.TemplatedParent),
                });

            var presenter = new FrameworkElementFactory(typeof(ContentPresenter));
            presenter.SetValue(ContentPresenter.HorizontalAlignmentProperty,
                               HorizontalAlignment.Center);
            presenter.SetValue(ContentPresenter.VerticalAlignmentProperty,
                               VerticalAlignment.Center);
            border.AppendChild(presenter);

            var template = new ControlTemplate(typeof(Button)) { VisualTree = border };
            var hover = new Trigger { Property = UIElement.IsMouseOverProperty, Value = true };
            hover.Setters.Add(new Setter(UIElement.OpacityProperty, 0.86));
            var pressed = new Trigger { Property = Button.IsPressedProperty, Value = true };
            pressed.Setters.Add(new Setter(UIElement.OpacityProperty, 0.7));
            var disabled = new Trigger { Property = UIElement.IsEnabledProperty, Value = false };
            disabled.Setters.Add(new Setter(UIElement.OpacityProperty, 0.45));
            template.Triggers.Add(hover);
            template.Triggers.Add(pressed);
            template.Triggers.Add(disabled);
            return template;
        }

        public static TextBox MakeTextBox(string text = "", bool mono = false)
        {
            return new TextBox
            {
                Text = text,
                FontFamily = mono ? MonoFont : UiFont,
                FontSize = 13,
                Padding = new Thickness(8, 6, 8, 6),
                Background = IsDark ? new SolidColorBrush(Color.FromArgb(255, 30, 28, 40))
                                    : Brushes.White,
                Foreground = TextPrimary,
                BorderBrush = Divider,
                CaretBrush = TextPrimary,
            };
        }

        public static Border Pill(string label, Brush color)
        {
            return new Border
            {
                Background = new SolidColorBrush(Color.FromArgb(34,
                    ((SolidColorBrush)color).Color.R,
                    ((SolidColorBrush)color).Color.G,
                    ((SolidColorBrush)color).Color.B)),
                CornerRadius = new CornerRadius(8),
                Padding = new Thickness(8, 2, 8, 3),
                VerticalAlignment = VerticalAlignment.Center,
                Child = new TextBlock
                {
                    Text = label,
                    FontFamily = UiFont,
                    FontSize = 11,
                    FontWeight = FontWeights.SemiBold,
                    Foreground = color,
                },
            };
        }
    }
}
