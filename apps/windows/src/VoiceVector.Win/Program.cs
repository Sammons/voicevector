using System;
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
                    Diag.Breadcrumb("services up");

                    MainWin = new MainWindow();
                    Diag.Breadcrumb("MainWindow constructed");
                    MainWin.Show();
                    Diag.Breadcrumb("activated");

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
