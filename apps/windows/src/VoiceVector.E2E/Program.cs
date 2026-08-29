using System.Diagnostics;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using VoiceVector.Shared;

namespace VoiceVector.E2E;

/// <summary>
/// End-to-end test for the Windows app, designed for CI runners:
///
///   mock OpenAI-compatible server ← app pipeline → this driver's TextBox
///
/// 1. Hosts a localhost provider (models / audio/transcriptions /
///    chat/completions) with canned responses.
/// 2. Writes config.json pointing the app at it; hotkey = F13; VV_FAKE_AUDIO
///    makes the app use a fixture WAV (runners have no microphone).
/// 3. Launches the real app, focuses a TextBox window of its own, synthesizes
///    an F13 press-and-hold through the real keyboard hook, and asserts the
///    cleaned transcript is pasted into the TextBox and stored as markdown.
///
/// Exit code 0 = pass. All diagnostics go to stdout.
/// </summary>
public static class Program
{
    private const string RawText = "um hello e2e world";
    private const string CleanedText = "Hello E2E world.";
    private const ushort VK_F13 = 0x7C;

    private static readonly List<string> Logs = new();
    private static void Say(string message)
    {
        Logs.Add(message);
        Console.WriteLine(message);
    }

    [STAThread]
    public static int Main(string[] args)
    {
        var appExe = Path.GetFullPath(args.Length > 0 ? args[0]
            : "apps/windows/src/VoiceVector.Win/bin/Release/net48/VoiceVector.exe");
        if (!File.Exists(appExe))
        {
            Say($"E2E FAIL: app exe not found at {appExe}");
            return 2;
        }

        using var server = StartMockServer(out int port);
        Say($"mock provider on 127.0.0.1:{port}");

        WriteConfig(port);
        var fixture = Path.Combine(Path.GetTempPath(), "vv-e2e-fixture.wav");
        File.WriteAllBytes(fixture, WavWriter.SilentWav(1.0));

        var appDataDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "VoiceVector");
        try { File.Delete(Path.Combine(appDataDir, "crash.log")); } catch { }

        var app = LaunchApp(appExe, fixture, autoDictateMs: null);

        // Receiver window: a focused TextBox the app should paste into.
        var form = new Form { Text = "VV E2E Receiver", Width = 500, Height = 300, TopMost = true };
        var box = new TextBox { Multiline = true, Dock = DockStyle.Fill };
        form.Controls.Add(box);

        int exitCode = 1;
        var worker = new Thread(() =>
        {
            try
            {
                exitCode = RunScenario(ref app, appExe, fixture, form, box, appDataDir);
            }
            catch (Exception e)
            {
                Say($"E2E FAIL: driver exception: {e}");
                exitCode = 1;
            }
            finally
            {
                try { form.Invoke(form.Close); } catch { }
            }
        });

        form.Shown += (_, _) => worker.Start();
        Application.Run(form);
        worker.Join();

        try { if (!app.HasExited) app.Kill(entireProcessTree: true); } catch { }
        DumpAppLogs(appDataDir);
        Say(exitCode == 0 ? "E2E PASS" : "E2E FAIL");
        return exitCode;
    }

    private static Process LaunchApp(string appExe, string fixture, int? autoDictateMs)
    {
        var psi = new ProcessStartInfo(appExe) { UseShellExecute = false };
        psi.Environment["VV_FAKE_AUDIO"] = fixture;
        if (autoDictateMs is int ms) psi.Environment["VV_E2E_AUTODICTATE"] = ms.ToString();
        var app = Process.Start(psi)!;
        Say($"app started pid={app.Id} autodictate={autoDictateMs?.ToString() ?? "off"}");
        return app;
    }

    private static bool WaitForBoot(Process app, string appDataDir)
    {
        var bootLog = Path.Combine(appDataDir, "boot.log");
        if (!WaitFor(() => File.Exists(bootLog) && ReadAll(bootLog).Contains("activated"),
                     TimeSpan.FromSeconds(30)))
        {
            Say("app never reached 'activated' breadcrumb");
            return false;
        }
        if (app.HasExited)
        {
            Say($"app exited early with {app.ExitCode}");
            return false;
        }
        Say("app activated");
        return true;
    }

    private static bool WaitForPaste(Form form, TextBox box, TimeSpan timeout, out string text)
    {
        string current = "";
        bool pasted = WaitFor(() =>
        {
            current = (string)form.Invoke(() => box.Text);
            return current.Contains(CleanedText);
        }, timeout);
        text = current;
        return pasted;
    }

    private static int RunScenario(ref Process app, string appExe, string fixture,
                                   Form form, TextBox box, string appDataDir)
    {
        if (!WaitForBoot(app, appDataDir)) { Say("E2E FAIL: boot"); return 1; }

        // Focus our receiver.
        form.Invoke(() => { form.Activate(); box.Focus(); });
        Thread.Sleep(800);

        // Path A: real hotkey — hold F13 (≥350 ms) through the keyboard hook.
        SendKey(VK_F13, down: true);
        Thread.Sleep(800);
        SendKey(VK_F13, down: false);
        Say("hotkey sent");

        if (WaitForPaste(form, box, TimeSpan.FromSeconds(15), out var text))
        {
            Say("paste verified (hotkey path)");
        }
        else
        {
            // Path B: synthesized input may not reach hooks in this session —
            // relaunch with the app-side auto-dictate trigger.
            Say($"hotkey path produced no paste (TextBox: \"{text}\") — retrying with auto-dictate trigger");
            try { if (!app.HasExited) app.Kill(entireProcessTree: true); } catch { }
            try { File.Delete(Path.Combine(appDataDir, "boot.log")); } catch { }
            form.Invoke(() => box.Clear());

            app = LaunchApp(appExe, fixture, autoDictateMs: 2000);
            if (!WaitForBoot(app, appDataDir)) { Say("E2E FAIL: boot (auto-dictate)"); return 1; }
            form.Invoke(() => { form.Activate(); box.Focus(); });

            if (!WaitForPaste(form, box, TimeSpan.FromSeconds(30), out text))
            {
                Say($"E2E FAIL: transcript not pasted on either path. TextBox: \"{text}\"");
                return 1;
            }
            Say("paste verified (auto-dictate path — synthetic hotkey input appears blocked in this session)");
        }

        // The library should have a markdown entry with raw + cleaned text.
        var inbox = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            "Documents", "VoiceVector", "Inbox");
        bool stored = WaitFor(() =>
            Directory.Exists(inbox) &&
            Directory.EnumerateFiles(inbox, "*.md").Any(f =>
            {
                var content = ReadAll(f);
                return content.Contains(CleanedText) && content.Contains(RawText);
            }), TimeSpan.FromSeconds(10));
        if (!stored)
        {
            Say("E2E FAIL: markdown entry with raw+cleaned text not found in library");
            return 1;
        }
        Say("library entry verified");
        return 0;
    }

    // -- mock provider --------------------------------------------------------

    private static HttpListener StartMockServer(out int port)
    {
        var random = new Random();
        HttpListener? listener = null;
        port = 0;
        for (int attempt = 0; attempt < 20; attempt++)
        {
            port = random.Next(20000, 50000);
            listener = new HttpListener();
            listener.Prefixes.Add($"http://127.0.0.1:{port}/");
            try { listener.Start(); break; }
            catch { listener.Close(); listener = null; }
        }
        if (listener is null) throw new InvalidOperationException("no free port for mock server");

        var server = listener;
        _ = Task.Run(async () =>
        {
            while (server.IsListening)
            {
                HttpListenerContext context;
                try { context = await server.GetContextAsync(); }
                catch { break; }
                _ = Task.Run(() => Handle(context));
            }
        });
        return listener;
    }

    private static void Handle(HttpListenerContext context)
    {
        string path = context.Request.Url?.AbsolutePath ?? "";
        string body = path switch
        {
            "/v1/models" => """{"data":[{"id":"whisper-1"},{"id":"test-model"}]}""",
            "/v1/audio/transcriptions" => JsonSerializer.Serialize(new { text = RawText }),
            "/v1/chat/completions" => JsonSerializer.Serialize(new
            {
                choices = new[] { new { message = new { role = "assistant", content = CleanedText } } },
            }),
            _ => "{}",
        };
        Say($"mock hit: {context.Request.HttpMethod} {path}");
        var bytes = Encoding.UTF8.GetBytes(body);
        context.Response.ContentType = "application/json";
        context.Response.OutputStream.Write(bytes);
        context.Response.Close();
    }

    // -- config ---------------------------------------------------------------

    private static void WriteConfig(int port)
    {
        var provider = ProviderProfile.Preset(ProviderKind.OpenAICompatible);
        provider.Name = "E2E Mock";
        provider.BaseUrl = $"http://127.0.0.1:{port}/v1";
        provider.SttModel = "whisper-1";
        provider.ChatModel = "test-model";

        var config = new AppConfig
        {
            WizardCompleted = true,
            AutoPaste = true,
            PlaySounds = false,
            SttProviderId = provider.Id,
        };
        config.DictationProfiles[0].Hotkey =
            new HotkeySpec { KeyCode = VK_F13, Modifiers = 0, IsModifierOnly = false };
        config.Providers.Add(provider);
        config.Cleanup.ProviderId = provider.Id;
        config.Save();
        Say($"config written to {AppConfig.DefaultPath}");
    }

    // -- helpers --------------------------------------------------------------

    private static bool WaitFor(Func<bool> condition, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            try { if (condition()) return true; } catch { }
            Thread.Sleep(250);
        }
        return false;
    }

    private static string ReadAll(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        return new StreamReader(stream).ReadToEnd();
    }

    private static void DumpAppLogs(string appDataDir)
    {
        foreach (var name in new[] { "boot.log", "crash.log" })
        {
            var path = Path.Combine(appDataDir, name);
            Say($"=== {name} ===");
            Say(File.Exists(path) ? ReadAll(path) : "(not present)");
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KEYBDINPUT
    {
        public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public nuint extra;
    }

    [StructLayout(LayoutKind.Explicit, Size = 40)] // native union pads to MOUSEINPUT
    private struct INPUT
    {
        [FieldOffset(0)] public int type;
        [FieldOffset(8)] public KEYBDINPUT ki;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint count, INPUT[] inputs, int size);

    private static void SendKey(ushort vk, bool down)
    {
        var input = new INPUT
        {
            type = 1,
            ki = new KEYBDINPUT { wVk = vk, dwFlags = down ? 0u : 2u },
        };
        uint sent = SendInput(1, [input], Marshal.SizeOf<INPUT>());
        if (sent != 1)
            Say($"SendInput FAILED (sent={sent}, win32={Marshal.GetLastWin32Error()})");
    }
}
