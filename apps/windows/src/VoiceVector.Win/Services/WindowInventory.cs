using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace VoiceVector.Win.Services
{
    /// <summary>A window another machine (or the router) can target.</summary>
    public sealed class WindowInfo
    {
        public uint Id;          // HWND value
        public string App = "";
        public string Title = "";
    }

    /// <summary>Lists targetable windows and raises one. Mirrors the macOS
    /// WindowInventory.</summary>
    public static class WindowInventory
    {
        private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

        [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lParam);
        [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hWnd);
        [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr hWnd);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowTextW(IntPtr hWnd, StringBuilder text, int max);
        [DllImport("user32.dll")] private static extern int GetWindowTextLengthW(IntPtr hWnd);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
        [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr hWnd, uint cmd);
        [DllImport("user32.dll")] private static extern int GetWindowLongW(IntPtr hWnd, int index);
        [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr hWnd);
        [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr hWnd, int cmd);
        [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] private static extern bool BringWindowToTop(IntPtr hWnd);
        [DllImport("user32.dll")] private static extern bool AttachThreadInput(uint idAttach, uint idTo, bool attach);
        [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();

        private const int GWL_EXSTYLE = -20;
        private const int WS_EX_TOOLWINDOW = 0x80;
        private const int SW_RESTORE = 9;

        /// <summary>Visible, titled, non-toolwindow top-level windows of other processes.</summary>
        public static List<WindowInfo> List()
        {
            var result = new List<WindowInfo>();
            uint ownPid = (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
            EnumWindows((hwnd, _) =>
            {
                if (!IsWindowVisible(hwnd)) return true;
                if ((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return true;
                int length = GetWindowTextLengthW(hwnd);
                if (length == 0) return true;
                uint pid;
                GetWindowThreadProcessId(hwnd, out pid);
                if (pid == ownPid) return true;
                var text = new StringBuilder(length + 1);
                GetWindowTextW(hwnd, text, text.Capacity);
                string app;
                try { app = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; }
                catch { app = "?"; }
                result.Add(new WindowInfo { Id = (uint)hwnd.ToInt64(), App = app, Title = text.ToString() });
                return true;
            }, IntPtr.Zero);
            return result;
        }

        /// <summary>Brings the window to the foreground and confirms it took;
        /// false when the window is gone OR Windows refused the focus change,
        /// so the caller must NOT paste (it would land in the focused window).</summary>
        public static bool Activate(uint id)
        {
            // HWNDs carry 32 significant bits and widen by SIGN-extension on
            // Win64; new IntPtr(uint) would zero-extend, so cast through int.
            var hwnd = new IntPtr(unchecked((int)id));
            if (!IsWindowVisible(hwnd)) return false;
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);

            // A background process is normally denied SetForegroundWindow.
            // Attaching our input thread to the target's foreground thread
            // lifts that restriction (the standard focus-stealing workaround).
            var foreground = GetForegroundWindow();
            uint targetThread = GetWindowThreadProcessId(hwnd, out _);
            uint foreThread = foreground == IntPtr.Zero ? 0 : GetWindowThreadProcessId(foreground, out _);
            uint ourThread = GetCurrentThreadId();
            bool attachedFore = foreThread != 0 && foreThread != targetThread
                                && AttachThreadInput(ourThread, foreThread, true);
            bool attachedTarget = targetThread != ourThread
                                  && AttachThreadInput(ourThread, targetThread, true);
            try
            {
                BringWindowToTop(hwnd);
                SetForegroundWindow(hwnd);
            }
            finally
            {
                if (attachedTarget) AttachThreadInput(ourThread, targetThread, false);
                if (attachedFore) AttachThreadInput(ourThread, foreThread, false);
            }
            for (int i = 0; i < 16; i++)
            {
                if (GetForegroundWindow() == hwnd) return true;
                System.Threading.Thread.Sleep(50);
            }
            return GetForegroundWindow() == hwnd;
        }

        /// <summary>Router input: one numbered line per window.</summary>
        public static string Describe(List<WindowInfo> windows)
        {
            var sb = new StringBuilder();
            foreach (var w in windows)
            {
                if (sb.Length > 0) sb.Append('\n');
                sb.Append(w.Id).Append(": ").Append(w.App).Append(" — ")
                  .Append(w.Title.Length == 0 ? "(untitled)" : w.Title);
            }
            return sb.ToString();
        }
    }
}
