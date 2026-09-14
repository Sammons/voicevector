using System;
using System.Diagnostics;
using System.IO;
using Microsoft.Win32;

namespace VoiceVector.Win.Services
{
    /// <summary>
    /// User-land install/uninstall: everything happens under HKCU and
    /// %LOCALAPPDATA% — never needs admin.
    ///
    /// The app ships as a single signed exe. First run from outside the
    /// install folder offers to "install": copy the exe to
    /// %LOCALAPPDATA%\Programs\VoiceVector, create a per-user Start Menu
    /// shortcut and (optionally) a HKCU Run entry for autostart. The same
    /// single file keeps working portably if the user declines.
    /// </summary>
    public static class InstallService
    {
        private const string RunKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Run";
        private const string RunValue = "VoiceVector";

        public static string ExePath
        {
            get { return Process.GetCurrentProcess().MainModule.FileName; }
        }

        public static string InstallDir
        {
            get
            {
                return Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "Programs", "VoiceVector");
            }
        }

        public static string InstalledExePath
        {
            get { return Path.Combine(InstallDir, "VoiceVector.exe"); }
        }

        public static bool IsRunningFromInstall
        {
            get
            {
                return string.Equals(Path.GetDirectoryName(ExePath),
                                     InstallDir, StringComparison.OrdinalIgnoreCase);
            }
        }

        /// <summary>CI and dev builds never see the offer: any VV_* env var
        /// marks an automation/dev launch; -dev versions must not self-copy.</summary>
        public static bool ShouldOfferInstall
        {
            get
            {
                if (IsRunningFromInstall) return false;
                if (UpdateService.IsDevBuild) return false;
                foreach (string key in Environment.GetEnvironmentVariables().Keys)
                {
                    if (key.StartsWith("VV_", StringComparison.Ordinal)) return false;
                }
                if (OfferDismissed) return false;
                return !File.Exists(InstalledExePath);
            }
        }

        private static bool OfferDismissed
        {
            get
            {
                try
                {
                    using (var key = Registry.CurrentUser.CreateSubKey(
                        @"SOFTWARE\VoiceVector\Installer"))
                    {
                        return (int)(key.GetValue("OfferDismissed", 0)) != 0;
                    }
                }
                catch { return false; }
            }
        }

        public static void DismissOffer()
        {
            try
            {
                using (var key = Registry.CurrentUser.CreateSubKey(
                    @"SOFTWARE\VoiceVector\Installer"))
                {
                    key.SetValue("OfferDismissed", 1, RegistryValueKind.DWord);
                }
            }
            catch { }
        }

        /// <summary>Copy this exe into the install folder, wire the Start Menu
        /// shortcut, launch the installed copy, and let this one exit. Returns
        /// false if anything failed (caller keeps running portably).</summary>
        public static bool Install()
        {
            try
            {
                Directory.CreateDirectory(InstallDir);
                var target = InstalledExePath;
                File.Copy(ExePath, target, overwrite: true);

                // Per-user Start Menu shortcut (no admin needed).
                var startMenu = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.StartMenu),
                    "Programs");
                var lnk = Path.Combine(startMenu, "VoiceVector.lnk");
                CreateShortcut(lnk, target);

                SetAutoStart(true);
                Process.Start(new ProcessStartInfo(target) { UseShellExecute = true });
                return true;
            }
            catch (Exception e)
            {
                Diag.WriteCrashLog(e);
                return false;
            }
        }

        /// <summary>Remove shortcut + autostart; the exe itself is a single
        /// file the user can delete whenever.</summary>
        public static void Uninstall()
        {
            SetAutoStart(false);
            try
            {
                var lnk = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.StartMenu),
                    "Programs", "VoiceVector.lnk");
                if (File.Exists(lnk)) File.Delete(lnk);
            }
            catch { }
        }

        private static void CreateShortcut(string lnkPath, string targetExe)
        {
            // WScript.Shell via reflection — avoids a Microsoft.CSharp dep for dynamic.
            object shell = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell"));
            object shortcut = shell.GetType().InvokeMember("CreateShortcut",
                System.Reflection.BindingFlags.InvokeMethod, null, shell,
                new object[] { lnkPath });
            var st = shortcut.GetType();
            st.InvokeMember("TargetPath", System.Reflection.BindingFlags.SetProperty,
                null, shortcut, new object[] { targetExe });
            st.InvokeMember("WorkingDirectory", System.Reflection.BindingFlags.SetProperty,
                null, shortcut, new object[] { InstallDir });
            st.InvokeMember("Description", System.Reflection.BindingFlags.SetProperty,
                null, shortcut, new object[] { "Minimalist native dictation" });
            st.InvokeMember("Save", System.Reflection.BindingFlags.InvokeMethod,
                null, shortcut, new object[0]);
        }

        public static bool IsAutoStart
        {
            get
            {
                try
                {
                    using (var key = Registry.CurrentUser.OpenSubKey(RunKey))
                    {
                        return key != null && key.GetValue(RunValue) != null;
                    }
                }
                catch { return false; }
            }
        }

        public static void SetAutoStart(bool enabled)
        {
            try
            {
                using (var key = Registry.CurrentUser.CreateSubKey(RunKey))
                {
                    if (enabled) key.SetValue(RunValue, InstalledExePath);
                    else key.DeleteValue(RunValue, throwOnMissingValue: false);
                }
            }
            catch (Exception e)
            {
                Diag.WriteCrashLog(e);
            }
        }
    }
}
