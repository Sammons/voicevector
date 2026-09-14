using System;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using VoiceVector.Shared;
using VoiceVector.Win.Services;

namespace VoiceVector.Win
{
    public static class Program
    {
        public static AppConfig Config;
        public static Library Lib;
        public static KeyboardHook Hook;
        public static DictationController Dictation;
        public static MainWindow MainWin;

        [STAThread]
        public static void Main()
        {
            Run();
        }

        private static int Run()
        {
            AppDomain.CurrentDomain.UnhandledException += (s, e) =>
                Diag.WriteCrashLog(e.ExceptionObject as Exception ?? new Exception("unknown"));
            TaskScheduler.UnobservedTaskException += (s, e) =>
            {
                Diag.WriteCrashLog(e.Exception);
                e.SetObserved();
            };
            // .NET Framework defaults can exclude TLS 1.2 on older Win10.
            System.Net.ServicePointManager.SecurityProtocol |=
                System.Net.SecurityProtocolType.Tls12;

            Diag.Breadcrumb("Main start");
            try
            {
                var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
                app.DispatcherUnhandledException += (s, e) =>
                {
                    Diag.WriteCrashLog(e.Exception);
                    Log.Error("Unhandled: " + e.Exception.Message);
                    e.Handled = true;
                };
                app.Startup += (s, e) =>
                {
                    Diag.Breadcrumb("Startup");
                    Theme.ApplyAppStyles(app);
                    Config = AppConfig.Load();
                    Config.Save(); // materialize config.json so it's discoverable
                    Lib = new Library(Config.ExpandedLibraryPath);
                    Hook = new KeyboardHook(() => Config, app.Dispatcher);
                    Dictation = new DictationController(() => Config, () => Lib, Hook,
                                                        app.Dispatcher);
                    Hook.OnAction += Dictation.Handle;
                    Hook.OnReviewAccept += Dictation.AcceptReview;
                    Hook.OnReviewDiscard += Dictation.DiscardReview;
                    Hook.Start();
                    if (Environment.GetEnvironmentVariable("VV_FAKE_AUDIO") == null)
                        Dictation.ApplyWarmPolicy();
                    var peers = PeerService.Shared;
                    peers.ConfigProvider = () => Config.MultiMachine;
                    peers.RunOnUi = a => app.Dispatcher.BeginInvoke(a);
                    peers.AddPeer = peer =>
                    {
                        if (!Config.MultiMachine.Peers.Any(p => p.Fingerprint == peer.Fingerprint))
                        {
                            Config.MultiMachine.Peers.Add(peer);
                            Config.Save();
                        }
                    };
                    peers.OnDeliver = (text, window, submit, done) =>
                        Dictation.ReceiveRoutedText(text, window, submit, "a paired machine", done);
                    peers.OnIncomingPair = (name, code, answer) =>
                    {
                        var result = System.Windows.MessageBox.Show(
                            "Pair with \"" + name + "\"?\n\nConfirm only if the other machine shows this code:\n\n"
                            + code.Substring(0, 3) + " " + code.Substring(3),
                            "VoiceVector pairing", System.Windows.MessageBoxButton.YesNo,
                            System.Windows.MessageBoxImage.Question);
                        answer(result == System.Windows.MessageBoxResult.Yes);
                    };
                    peers.ApplyConfig();
                    Diag.Breadcrumb("services up");

                    MainWin = new MainWindow();
                    Diag.Breadcrumb("MainWindow constructed");
                    MainWin.Show();
                    Diag.Breadcrumb("activated");

                    MaybeOfferInstall();

                    // E2E seam: self-trigger one dictation for runners where
                    // synthesized keyboard input doesn't reach hooks.
                    int delayMs;
                    if (int.TryParse(Environment.GetEnvironmentVariable("VV_E2E_AUTODICTATE"),
                                     out delayMs))
                    {
                        Diag.Breadcrumb("autodictate armed");
                        var _ = AutoDictateAsync(app, delayMs);
                    }
                };
                return app.Run();
            }
            catch (Exception e)
            {
                Diag.WriteCrashLog(e);
                throw;
            }
        }

        /// <summary>First run from outside the install folder: offer to copy
        /// the single exe into %LOCALAPPDATA%\\Programs with a Start Menu
        /// shortcut. User-land only; declining keeps the portable run.</summary>
        private static void MaybeOfferInstall()
        {
            if (!InstallService.ShouldOfferInstall) return;
            var result = System.Windows.MessageBox.Show(MainWin,
                "Install VoiceVector for your user?\n\n" +
                "\u2022 Copies the app to " + InstallService.InstallDir + "\n" +
                "\u2022 Adds a Start Menu shortcut\n" +
                "\u2022 Starts with Windows (changeable in Settings)\n\n" +
                "No admin rights needed. Choose No to keep running this copy as-is.",
                "Install VoiceVector", System.Windows.MessageBoxButton.YesNoCancel,
                System.Windows.MessageBoxImage.Question);
            if (result == System.Windows.MessageBoxResult.Yes)
            {
                if (InstallService.Install())
                {
                    Program.Hook.Stop();
                    Application.Current.Shutdown();
                    Environment.Exit(0);
                }
                // Install failed — keep running the portable copy.
            }
            else if (result == System.Windows.MessageBoxResult.Cancel)
            {
                InstallService.DismissOffer();
            }
        }

        private static async Task AutoDictateAsync(Application app, int delayMs)
        {
            await Task.Delay(delayMs);
            var op1 = app.Dispatcher.BeginInvoke((Action)(() => Dictation.StartRecording()));
            await Task.Delay(900);
            var op2 = app.Dispatcher.BeginInvoke((Action)(() => Dictation.FinishRecording()));
            GC.KeepAlive(op1);
            GC.KeepAlive(op2);
        }
    }
}
