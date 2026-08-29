using System;
using System.Diagnostics;
using System.IO;
using System.Media;
using System.Security.Cryptography;
using System.Text;
using VoiceVector.Shared;

namespace VoiceVector.Win.Services
{
    /// <summary>Chimes via System.Media.SoundPlayer (built-in) from generated WAVs.</summary>
    public static class Chime
    {
        private static SoundPlayer _start, _stop, _error;

        private static SoundPlayer Make(byte[] wav)
        {
            var player = new SoundPlayer(new MemoryStream(wav));
            player.Load();
            return player;
        }

        private static void Play(ref SoundPlayer player, Func<byte[]> generate)
        {
            try
            {
                if (player == null) player = Make(generate());
                player.Play();
            }
            catch (Exception e)
            {
                Log.Error("Chime failed: " + e.Message);
            }
        }

        public static void PlayStart(bool enabled)
        {
            if (enabled) Play(ref _start, WavWriter.StartChime);
        }

        public static void PlayStop(bool enabled)
        {
            if (enabled) Play(ref _stop, WavWriter.StopChime);
        }

        public static void PlayError()
        {
            Play(ref _error, WavWriter.ErrorChime);
        }
    }

    /// <summary>API keys: DPAPI (current user) files — ProtectedData is built
    /// into .NET Framework, no interop needed.</summary>
    public static class KeyStore
    {
        private static string Dir
        {
            get
            {
                return Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "VoiceVector", "keys");
            }
        }

        private static string PathFor(Guid id)
        {
            return Path.Combine(Dir, id.ToString("D"));
        }

        public static void SetApiKey(Guid id, string key)
        {
            try
            {
                if (string.IsNullOrEmpty(key))
                {
                    File.Delete(PathFor(id));
                    return;
                }
                Directory.CreateDirectory(Dir);
                var cipher = ProtectedData.Protect(Encoding.UTF8.GetBytes(key), null,
                                                   DataProtectionScope.CurrentUser);
                File.WriteAllBytes(PathFor(id), cipher);
            }
            catch (Exception e)
            {
                Log.Error("Key store write failed: " + e.Message);
            }
        }

        public static string GetApiKey(Guid id)
        {
            try
            {
                var path = PathFor(id);
                if (!File.Exists(path)) return "";
                var plain = ProtectedData.Unprotect(File.ReadAllBytes(path), null,
                                                    DataProtectionScope.CurrentUser);
                return Encoding.UTF8.GetString(plain);
            }
            catch (Exception e)
            {
                Log.Error("Key store read failed: " + e.Message);
                return "";
            }
        }

        public static void DeleteApiKey(Guid id)
        {
            SetApiKey(id, "");
        }
    }

    /// <summary>Startup breadcrumbs + crash log (%APPDATA%\VoiceVector\*.log) —
    /// same diagnostics contract the CI smoke tests rely on.</summary>
    public static class Diag
    {
        private static string _bootLogPath;

        private static string AppDataDir
        {
            get
            {
                return Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "VoiceVector");
            }
        }

        public static void Breadcrumb(string step)
        {
            try
            {
                if (_bootLogPath == null)
                {
                    Directory.CreateDirectory(AppDataDir);
                    _bootLogPath = Path.Combine(AppDataDir, "boot.log");
                    File.WriteAllText(_bootLogPath, "");
                }
                File.AppendAllText(_bootLogPath,
                    "[" + DateTime.Now.ToString("HH:mm:ss.fff") + "] " + step + "\n");
            }
            catch { }
        }

        public static void WriteCrashLog(Exception exception)
        {
            try
            {
                Directory.CreateDirectory(AppDataDir);
                File.AppendAllText(Path.Combine(AppDataDir, "crash.log"),
                    "[" + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "] " + exception + "\n\n");
            }
            catch { }
        }
    }

    /// <summary>One-click updates from GitHub Releases — downloads the single
    /// exe asset, swaps itself via a detached cmd script, relaunches.</summary>
    public static class UpdateService
    {
        private const string Repo = "Sammons/voicevector";
        private const string AssetName = "VoiceVector-windows-x64.zip";

        public static string CurrentVersion
        {
            get
            {
                var info = System.Reflection.Assembly.GetExecutingAssembly();
                var attrs = info.GetCustomAttributes(
                    typeof(System.Reflection.AssemblyInformationalVersionAttribute), false);
                var version = attrs.Length > 0
                    ? ((System.Reflection.AssemblyInformationalVersionAttribute)attrs[0]).InformationalVersion
                    : "0.0.0-dev";
                int plus = version.IndexOf('+');
                return plus > 0 ? version.Substring(0, plus) : version;
            }
        }

        public static bool IsDevBuild { get { return CurrentVersion.EndsWith("-dev"); } }

        public class UpdateInfo
        {
            public string Version;
            public string AssetUrl;
        }

        public static async System.Threading.Tasks.Task<UpdateInfo> FetchLatestAsync()
        {
            using (var request = new System.Net.Http.HttpRequestMessage(
                System.Net.Http.HttpMethod.Get,
                "https://api.github.com/repos/" + Repo + "/releases/latest"))
            {
                request.Headers.Accept.ParseAdd("application/vnd.github+json");
                request.Headers.UserAgent.ParseAdd("VoiceVector");
                using (var response = await ProviderClient.Http.SendAsync(request).ConfigureAwait(false))
                {
                    response.EnsureSuccessStatusCode();
                    var json = Json.ParseObject(
                        await response.Content.ReadAsStringAsync().ConfigureAwait(false));
                    var tag = Json.Str(json, "tag_name");
                    var version = tag.StartsWith("v") ? tag.Substring(1) : tag;
                    string assetUrl = null;
                    var assets = Json.Arr(json, "assets");
                    if (assets != null)
                        foreach (var item in assets)
                        {
                            var asset = item as System.Collections.Generic.Dictionary<string, object>;
                            if (asset != null && Json.Str(asset, "name") == AssetName)
                            {
                                assetUrl = Json.Str(asset, "browser_download_url");
                                break;
                            }
                        }
                    if (assetUrl == null || !IsNewer(version, CurrentVersion)) return null;
                    return new UpdateInfo { Version = version, AssetUrl = assetUrl };
                }
            }
        }

        public static bool IsNewer(string candidate, string current)
        {
            if (current.EndsWith("-dev")) return true;
            var a = candidate.Split('.');
            var b = current.Split('.');
            for (int i = 0; i < Math.Max(a.Length, b.Length); i++)
            {
                int x, y;
                int.TryParse(i < a.Length ? a[i] : "0", out x);
                int.TryParse(i < b.Length ? b[i] : "0", out y);
                if (x != y) return x > y;
            }
            return false;
        }

        public static async System.Threading.Tasks.Task DownloadAndInstallAsync(UpdateInfo info)
        {
            var exePath = Process.GetCurrentProcess().MainModule.FileName;
            var installDir = Path.GetDirectoryName(exePath);
            var workDir = Path.Combine(Path.GetTempPath(), "vv-update-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(workDir);

            var zipPath = Path.Combine(workDir, AssetName);
            using (var response = await ProviderClient.Http.GetAsync(info.AssetUrl).ConfigureAwait(false))
            {
                response.EnsureSuccessStatusCode();
                using (var file = File.Create(zipPath))
                {
                    await response.Content.CopyToAsync(file).ConfigureAwait(false);
                }
            }

            var newDir = Path.Combine(workDir, "new");
            System.IO.Compression.ZipFile.ExtractToDirectory(zipPath, newDir);
            if (!File.Exists(Path.Combine(newDir, "VoiceVector.exe")))
                throw new InvalidOperationException("Downloaded update did not contain VoiceVector.exe.");

            int pid = Process.GetCurrentProcess().Id;
            var script = Path.Combine(workDir, "update.cmd");
            File.WriteAllText(script,
                "@echo off\r\n" +
                ":wait\r\n" +
                "tasklist /FI \"PID eq " + pid + "\" 2>nul | find \"" + pid + "\" >nul && (timeout /t 1 /nobreak >nul & goto wait)\r\n" +
                "copy /y \"" + Path.Combine(newDir, "VoiceVector.exe") + "\" \"" + exePath + "\" >nul\r\n" +
                "start \"\" \"" + exePath + "\"\r\n" +
                "rd /s /q \"" + workDir + "\"\r\n");

            Process.Start(new ProcessStartInfo("cmd.exe", "/c \"" + script + "\"")
            {
                CreateNoWindow = true,
                UseShellExecute = false,
                WorkingDirectory = Path.GetTempPath(),
            });
            System.Windows.Application.Current.Shutdown();
        }
    }
}
