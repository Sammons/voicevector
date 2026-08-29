using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using VoiceVector.Win.Services;

namespace VoiceVector.Win
{
    /// <summary>First-run walkthrough: welcome → provider → hotkey → ready.</summary>
    public static class WizardUi
    {
        public static UIElement Build(MainWindow owner, int step)
        {
            var stack = new StackPanel
            {
                Margin = new Thickness(40),
                MaxWidth = 560,
                VerticalAlignment = VerticalAlignment.Center,
            };

            // Progress dots.
            var dots = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                HorizontalAlignment = HorizontalAlignment.Center,
                Margin = new Thickness(0, 0, 0, 20),
            };
            for (int i = 0; i < 4; i++)
            {
                dots.Children.Add(new System.Windows.Shapes.Ellipse
                {
                    Width = 8,
                    Height = 8,
                    Margin = new Thickness(4, 0, 4, 0),
                    Fill = i <= step ? (Brush)Theme.AccentBrush : Theme.Divider,
                });
            }
            stack.Children.Add(dots);

            switch (step)
            {
                case 0:
                    stack.Children.Add(CenteredIcon("", 52)); // microphone
                    stack.Children.Add(Headline("Welcome to VoiceVector"));
                    stack.Children.Add(Sub(
                        "Press a hotkey anywhere, speak, and the cleaned-up text is typed right "
                        + "where your cursor is. Recordings and transcripts stay on this PC as "
                        + "plain files.\n\nIf Windows asks about microphone access, allow it in "
                        + "Settings → Privacy & security → Microphone."));
                    break;
                case 1:
                    stack.Children.Add(Headline("Connect a provider"));
                    stack.Children.Add(Sub(
                        "ElevenLabs or Vercel AI Gateway for transcription; the gateway, Cerebras, "
                        + "Fireworks, Azure AI Foundry, or a local Foundry Local server for "
                        + "cleanup. You can add more later in Settings."));
                    stack.Children.Add(SettingsUi.BuildProviders(owner));
                    break;
                case 2:
                    stack.Children.Add(Headline("Choose your hotkey"));
                    stack.Children.Add(SettingsUi.BuildDictation(owner));
                    break;
                default:
                    stack.Children.Add(CenteredIcon("", 48)); // checkmark
                    stack.Children.Add(Headline("You're set"));
                    stack.Children.Add(Sub(
                        "Click into any text field and press "
                        + KeyboardHook.Describe(Program.Config.PrimaryHotkey)
                        + ". Your dictations are saved in " + Program.Config.LibraryPath + "."));
                    break;
            }

            var nav = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                HorizontalAlignment = HorizontalAlignment.Center,
                Margin = new Thickness(0, 20, 0, 0),
            };
            int current = step;
            if (current > 0 && current < 3)
            {
                var back = Theme.MakeButton("Back");
                back.Margin = new Thickness(0, 0, 10, 0);
                back.Click += (s, e) => { owner.SetWizardStep(current - 1); };
                nav.Children.Add(back);
            }
            var next = Theme.MakeButton(
                current == 0 ? "Get Started" : current >= 3 ? "Start Dictating" : "Continue",
                prominent: true);
            next.Click += (s, e) =>
            {
                if (current >= 3)
                {
                    Program.Config.WizardCompleted = true;
                    Program.Config.Save();
                    owner.RefreshContent();
                }
                else
                {
                    owner.SetWizardStep(current + 1);
                }
            };
            nav.Children.Add(next);
            stack.Children.Add(nav);

            return new ScrollViewer
            {
                Content = stack,
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            };
        }

        private static TextBlock Headline(string text)
        {
            return new TextBlock
            {
                Text = text,
                FontFamily = Theme.DisplayFont,
                FontSize = 26,
                FontWeight = FontWeights.Bold,
                Foreground = Theme.TextPrimary,
                TextAlignment = TextAlignment.Center,
                Margin = new Thickness(0, 0, 0, 10),
            };
        }

        private static TextBlock Sub(string text)
        {
            return new TextBlock
            {
                Text = text,
                FontFamily = Theme.UiFont,
                FontSize = 13,
                Foreground = Theme.TextSecondary,
                TextWrapping = TextWrapping.Wrap,
                TextAlignment = TextAlignment.Center,
                Margin = new Thickness(0, 0, 0, 8),
            };
        }

        private static UIElement CenteredIcon(string glyph, double size)
        {
            var icon = Theme.Icon(glyph, size, Theme.AccentBrush);
            icon.HorizontalAlignment = HorizontalAlignment.Center;
            icon.Margin = new Thickness(0, 0, 0, 12);
            return icon;
        }
    }
}
