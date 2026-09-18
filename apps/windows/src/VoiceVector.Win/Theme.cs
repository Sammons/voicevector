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
            FieldBackground = Freeze(new SolidColorBrush(
                IsDark ? Color.FromRgb(30, 28, 40) : Colors.White));
            PublishBrushes();
        }

        /// <summary>Input-field surface, shared by the implicit TextBox style.</summary>
        public static SolidColorBrush FieldBackground;

        /// <summary>Expose the palette as application-level resources so XAML
        /// styles can reference it with DynamicResource and follow Refresh().</summary>
        public static void PublishBrushes()
        {
            var res = Application.Current == null ? null : Application.Current.Resources;
            if (res == null) return;
            Action<string, Brush> set = (key, brush) =>
            {
                if (res.Contains(key)) res[key] = brush; else res.Add(key, brush);
            };
            set("VV.Accent", AccentBrush);
            set("VV.AccentSoft", AccentSoft);
            set("VV.Background", WindowBackground);
            set("VV.Card", CardBackground);
            set("VV.TextPrimary", TextPrimary);
            set("VV.TextSecondary", TextSecondary);
            set("VV.TextTertiary", TextTertiary);
            set("VV.Danger", Danger);
            set("VV.Divider", Divider);
            set("VV.Field", FieldBackground);
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

        // -- implicit styles + app icon -----------------------------------------

        private static System.Drawing.Icon _appIcon;

        /// <summary>The VoiceVector icon (exe, windows, tray). Loaded once from
        /// the embedded App.ico resource.</summary>
        public static System.Drawing.Icon AppIcon
        {
            get
            {
                if (_appIcon == null)
                {
                    var sri = Application.GetResourceStream(
                        new Uri("pack://application:,,,/App.ico"));
                    using (var stream = sri.Stream)
                    {
                        _appIcon = new System.Drawing.Icon(stream);
                    }
                }
                return _appIcon;
            }
        }

        /// <summary>Install implicit WPF styles for the controls the element
        /// factories don't cover (CheckBox, ComboBox, RadioButton, raw
        /// TextBox/Button, ToolTip). Defined as XAML because those control
        /// templates are too long for FrameworkElementFactory. Call once after
        /// the Application exists; brushes follow Refresh().</summary>
        public static void ApplyAppStyles(Application app)
        {
            PublishBrushes();
            app.Resources.MergedDictionaries.Add(
                (ResourceDictionary)System.Windows.Markup.XamlReader.Parse(StylesXaml));
        }

        private const string StylesXaml = @"
<ResourceDictionary
    xmlns=""http://schemas.microsoft.com/winfx/2006/xaml/presentation""
    xmlns:x=""http://schemas.microsoft.com/winfx/2006/xaml"">

  <Style TargetType=""ToolTip"">
    <Setter Property=""Background"" Value=""{DynamicResource VV.Card}"" />
    <Setter Property=""Foreground"" Value=""{DynamicResource VV.TextPrimary}"" />
    <Setter Property=""FontFamily"" Value=""Segoe UI Variable Text, Segoe UI"" />
    <Setter Property=""FontSize"" Value=""12"" />
    <Setter Property=""Template"">
      <Setter.Value>
        <ControlTemplate TargetType=""ToolTip"">
          <Border Background=""{TemplateBinding Background}"" CornerRadius=""6""
                  BorderBrush=""{DynamicResource VV.Divider}"" BorderThickness=""1""
                  Padding=""8,5"">
            <ContentPresenter />
          </Border>
        </ControlTemplate>
      </Setter.Value>
    </Setter>
  </Style>

  <Style TargetType=""Button"">
    <Setter Property=""FontFamily"" Value=""Segoe UI Variable Text, Segoe UI"" />
    <Setter Property=""FontSize"" Value=""13"" />
    <Setter Property=""Padding"" Value=""14,7"" />
    <Setter Property=""Cursor"" Value=""Hand"" />
    <Setter Property=""Foreground"" Value=""{DynamicResource VV.TextPrimary}"" />
    <Setter Property=""Background"" Value=""{DynamicResource VV.Card}"" />
    <Setter Property=""BorderBrush"" Value=""{DynamicResource VV.Divider}"" />
    <Setter Property=""Template"">
      <Setter.Value>
        <ControlTemplate TargetType=""Button"">
          <Border x:Name=""root"" CornerRadius=""8"" BorderThickness=""1""
                  Background=""{TemplateBinding Background}""
                  BorderBrush=""{TemplateBinding BorderBrush}""
                  Padding=""{TemplateBinding Padding}"">
            <ContentPresenter HorizontalAlignment=""Center"" VerticalAlignment=""Center"" />
          </Border>
          <ControlTemplate.Triggers>
            <Trigger Property=""IsMouseOver"" Value=""True"">
              <Setter Property=""Opacity"" Value=""0.86"" />
            </Trigger>
            <Trigger Property=""IsPressed"" Value=""True"">
              <Setter Property=""Opacity"" Value=""0.7"" />
            </Trigger>
            <Trigger Property=""IsEnabled"" Value=""False"">
              <Setter Property=""Opacity"" Value=""0.45"" />
            </Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value>
    </Setter>
  </Style>

  <Style TargetType=""TextBox"">
    <Setter Property=""FontFamily"" Value=""Segoe UI Variable Text, Segoe UI"" />
    <Setter Property=""FontSize"" Value=""13"" />
    <Setter Property=""Padding"" Value=""8,6"" />
    <Setter Property=""Background"" Value=""{DynamicResource VV.Field}"" />
    <Setter Property=""Foreground"" Value=""{DynamicResource VV.TextPrimary}"" />
    <Setter Property=""BorderBrush"" Value=""{DynamicResource VV.Divider}"" />
    <Setter Property=""CaretBrush"" Value=""{DynamicResource VV.TextPrimary}"" />
    <Setter Property=""SelectionBrush"" Value=""{DynamicResource VV.Accent}"" />
    <Setter Property=""Template"">
      <Setter.Value>
        <ControlTemplate TargetType=""TextBox"">
          <Border x:Name=""Bd"" CornerRadius=""6"" BorderThickness=""1""
                  Background=""{TemplateBinding Background}""
                  BorderBrush=""{TemplateBinding BorderBrush}"">
            <ScrollViewer x:Name=""PART_ContentHost"" Margin=""{TemplateBinding Padding}"" />
          </Border>
          <ControlTemplate.Triggers>
            <Trigger Property=""IsKeyboardFocusWithin"" Value=""True"">
              <Setter Property=""BorderBrush"" Value=""{DynamicResource VV.Accent}"" />
            </Trigger>
            <Trigger Property=""IsEnabled"" Value=""False"">
              <Setter Property=""Opacity"" Value=""0.45"" />
            </Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value>
    </Setter>
  </Style>

  <Style TargetType=""CheckBox"">
    <Setter Property=""FontFamily"" Value=""Segoe UI Variable Text, Segoe UI"" />
    <Setter Property=""FontSize"" Value=""13"" />
    <Setter Property=""Foreground"" Value=""{DynamicResource VV.TextPrimary}"" />
    <Setter Property=""Cursor"" Value=""Hand"" />
    <Setter Property=""VerticalContentAlignment"" Value=""Center"" />
    <Setter Property=""Template"">
      <Setter.Value>
        <ControlTemplate TargetType=""CheckBox"">
          <StackPanel Orientation=""Horizontal"" Background=""Transparent"">
            <Border x:Name=""box"" Width=""18"" Height=""18"" CornerRadius=""5""
                    BorderThickness=""1.4"" VerticalAlignment=""Center""
                    Background=""{DynamicResource VV.Field}""
                    BorderBrush=""{DynamicResource VV.Divider}"">
              <Path x:Name=""check"" Data=""M 4.5 9.5 L 8 13 L 14 5.5"" Stroke=""White""
                    StrokeThickness=""2"" StrokeStartLineCap=""Round""
                    StrokeEndLineCap=""Round"" Opacity=""0"" />
            </Border>
            <ContentPresenter Margin=""8,0,0,0"" VerticalAlignment=""Center""
                              RecognizesAccessKey=""True"" />
          </StackPanel>
          <ControlTemplate.Triggers>
            <Trigger Property=""IsChecked"" Value=""True"">
              <Setter TargetName=""box"" Property=""Background""
                      Value=""{DynamicResource VV.Accent}"" />
              <Setter TargetName=""box"" Property=""BorderBrush""
                      Value=""{DynamicResource VV.Accent}"" />
              <Setter TargetName=""check"" Property=""Opacity"" Value=""1"" />
            </Trigger>
            <Trigger Property=""IsMouseOver"" Value=""True"">
              <Setter TargetName=""box"" Property=""Opacity"" Value=""0.86"" />
            </Trigger>
            <Trigger Property=""IsEnabled"" Value=""False"">
              <Setter Property=""Opacity"" Value=""0.45"" />
            </Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value>
    </Setter>
  </Style>

  <Style TargetType=""RadioButton"">
    <Setter Property=""FontFamily"" Value=""Segoe UI Variable Text, Segoe UI"" />
    <Setter Property=""FontSize"" Value=""13"" />
    <Setter Property=""Foreground"" Value=""{DynamicResource VV.TextPrimary}"" />
    <Setter Property=""Cursor"" Value=""Hand"" />
    <Setter Property=""Template"">
      <Setter.Value>
        <ControlTemplate TargetType=""RadioButton"">
          <StackPanel Orientation=""Horizontal"" Background=""Transparent"">
            <Border x:Name=""ring"" Width=""18"" Height=""18"" CornerRadius=""9""
                    BorderThickness=""1.4"" VerticalAlignment=""Center""
                    Background=""{DynamicResource VV.Field}""
                    BorderBrush=""{DynamicResource VV.Divider}"">
              <Ellipse x:Name=""dot"" Width=""8"" Height=""8""
                       Fill=""{DynamicResource VV.Accent}"" Opacity=""0"" />
            </Border>
            <ContentPresenter Margin=""8,0,0,0"" VerticalAlignment=""Center""
                              RecognizesAccessKey=""True"" />
          </StackPanel>
          <ControlTemplate.Triggers>
            <Trigger Property=""IsChecked"" Value=""True"">
              <Setter TargetName=""ring"" Property=""BorderBrush""
                      Value=""{DynamicResource VV.Accent}"" />
              <Setter TargetName=""dot"" Property=""Opacity"" Value=""1"" />
            </Trigger>
            <Trigger Property=""IsMouseOver"" Value=""True"">
              <Setter TargetName=""ring"" Property=""Opacity"" Value=""0.86"" />
            </Trigger>
            <Trigger Property=""IsEnabled"" Value=""False"">
              <Setter Property=""Opacity"" Value=""0.45"" />
            </Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value>
    </Setter>
  </Style>

  <Style TargetType=""ComboBox"">
    <Setter Property=""FontFamily"" Value=""Segoe UI Variable Text, Segoe UI"" />
    <Setter Property=""FontSize"" Value=""13"" />
    <Setter Property=""Foreground"" Value=""{DynamicResource VV.TextPrimary}"" />
    <Setter Property=""Background"" Value=""{DynamicResource VV.Field}"" />
    <Setter Property=""BorderBrush"" Value=""{DynamicResource VV.Divider}"" />
    <Setter Property=""Padding"" Value=""10,6"" />
    <Setter Property=""Cursor"" Value=""Hand"" />
    <Setter Property=""Template"">
      <Setter.Value>
        <ControlTemplate TargetType=""ComboBox"">
          <Grid>
            <!-- Face first, then the transparent ToggleButton ON TOP of it:
                 later Grid children win hit-testing, so clicks anywhere on
                 the box reach the toggle and open the dropdown. With the
                 toggle underneath the opaque face every ComboBox is dead. -->
            <Border x:Name=""Bd"" CornerRadius=""6"" BorderThickness=""1""
                    Background=""{TemplateBinding Background}""
                    BorderBrush=""{TemplateBinding BorderBrush}"">
              <Grid IsHitTestVisible=""False"">
                <Grid.ColumnDefinitions>
                  <ColumnDefinition Width=""*"" />
                  <ColumnDefinition Width=""26"" />
                </Grid.ColumnDefinitions>
                <ContentPresenter Grid.Column=""0"" Margin=""{TemplateBinding Padding}""
                                  VerticalAlignment=""Center""
                                  Content=""{TemplateBinding SelectionBoxItem}""
                                  ContentTemplate=""{TemplateBinding SelectionBoxItemTemplate}""
                                  ContentTemplateSelector=""{TemplateBinding ItemTemplateSelector}"" />
                <TextBlock Grid.Column=""1"" Text=""""
                           FontFamily=""Segoe Fluent Icons, Segoe MDL2 Assets"" FontSize=""10""
                           Foreground=""{DynamicResource VV.TextSecondary}""
                           VerticalAlignment=""Center"" HorizontalAlignment=""Center"" />
              </Grid>
            </Border>
            <ToggleButton x:Name=""ToggleButton"" Focusable=""False""
                          ClickMode=""Press""
                          IsChecked=""{Binding IsDropDownOpen, RelativeSource={RelativeSource TemplatedParent}, Mode=TwoWay}""
                          Background=""Transparent"" BorderThickness=""0"">
              <ToggleButton.Template>
                <ControlTemplate TargetType=""ToggleButton"">
                  <Border Background=""Transparent"" />
                </ControlTemplate>
              </ToggleButton.Template>
            </ToggleButton>
            <Popup x:Name=""PART_Popup"" IsOpen=""{TemplateBinding IsDropDownOpen}""
                   AllowsTransparency=""True"" Placement=""Bottom""
                   PopupAnimation=""Fade"" StaysOpen=""False"">
              <Border Background=""{DynamicResource VV.Card}""
                      BorderBrush=""{DynamicResource VV.Divider}"" BorderThickness=""1""
                      CornerRadius=""8"" Margin=""0,4,0,0""
                      MinWidth=""{TemplateBinding ActualWidth}"">
                <Border.Effect>
                  <DropShadowEffect BlurRadius=""12"" ShadowDepth=""2"" Opacity=""0.35"" />
                </Border.Effect>
                <ScrollViewer MaxHeight=""320"" VerticalScrollBarVisibility=""Auto"">
                  <ItemsPresenter />
                </ScrollViewer>
              </Border>
            </Popup>
          </Grid>
          <ControlTemplate.Triggers>
            <Trigger Property=""IsDropDownOpen"" Value=""True"">
              <Setter TargetName=""Bd"" Property=""BorderBrush""
                      Value=""{DynamicResource VV.Accent}"" />
            </Trigger>
            <Trigger Property=""IsEnabled"" Value=""False"">
              <Setter Property=""Opacity"" Value=""0.45"" />
            </Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value>
    </Setter>
    <Setter Property=""ItemContainerStyle"">
      <Setter.Value>
        <Style TargetType=""ComboBoxItem"">
          <Setter Property=""FontFamily"" Value=""Segoe UI Variable Text, Segoe UI"" />
          <Setter Property=""FontSize"" Value=""13"" />
          <Setter Property=""Foreground"" Value=""{DynamicResource VV.TextPrimary}"" />
          <Setter Property=""Padding"" Value=""10,6"" />
          <Setter Property=""Cursor"" Value=""Hand"" />
          <Setter Property=""Template"">
            <Setter.Value>
              <ControlTemplate TargetType=""ComboBoxItem"">
                <Border x:Name=""row"" Background=""Transparent""
                        Padding=""{TemplateBinding Padding}"">
                  <ContentPresenter VerticalAlignment=""Center"" />
                </Border>
                <ControlTemplate.Triggers>
                  <Trigger Property=""IsHighlighted"" Value=""True"">
                    <Setter TargetName=""row"" Property=""Background""
                            Value=""{DynamicResource VV.AccentSoft}"" />
                  </Trigger>
                </ControlTemplate.Triggers>
              </ControlTemplate>
            </Setter.Value>
          </Setter>
        </Style>
      </Setter.Value>
    </Setter>
  </Style>
</ResourceDictionary>
";
    }
}
