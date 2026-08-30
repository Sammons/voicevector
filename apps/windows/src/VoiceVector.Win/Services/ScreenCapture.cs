using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using VoiceVector.Shared;

namespace VoiceVector.Win.Services
{
    /// <summary>Screenshots of every display (JPEG, ≤1280 px wide each) for
    /// LLM context, the display holding the foreground window first with that
    /// window outlined in red. Mirrors the macOS ScreenCapture; no special
    /// permission is needed on Windows.</summary>
    public static class ScreenCapture
    {
        [StructLayout(LayoutKind.Sequential)]
        private struct RECT { public int Left, Top, Right, Bottom; }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        private class MONITORINFO
        {
            public int cbSize = Marshal.SizeOf(typeof(MONITORINFO));
            public RECT rcMonitor;
            public RECT rcWork;
            public uint dwFlags;
        }

        private delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdc, ref RECT rect, IntPtr data);

        [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
        [DllImport("user32.dll")] private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc proc, IntPtr data);
        [DllImport("user32.dll", CharSet = CharSet.Auto)] private static extern bool GetMonitorInfo(IntPtr hMonitor, [In, Out] MONITORINFO info);
        [DllImport("user32.dll")] private static extern IntPtr MonitorFromWindow(IntPtr hWnd, uint flags);
        private const uint MONITOR_DEFAULTTONEAREST = 2;

        public static ScreenshotSet AllScreens(int maxWidth = 1280)
        {
            var set = new ScreenshotSet();
            try
            {
                var target = IntPtr.Zero;
                RECT targetRect = new RECT();
                var hwnd = GetForegroundWindow();
                if (hwnd != IntPtr.Zero)
                {
                    uint pid;
                    GetWindowThreadProcessId(hwnd, out pid);
                    if (pid != (uint)System.Diagnostics.Process.GetCurrentProcess().Id && GetWindowRect(hwnd, out targetRect))
                        target = hwnd;
                }
                var activeMonitor = target != IntPtr.Zero ? MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST) : IntPtr.Zero;

                var monitors = new List<KeyValuePair<IntPtr, RECT>>();
                EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr hMonitor, IntPtr hdc, ref RECT rect, IntPtr data) =>
                {
                    var info = new MONITORINFO();
                    if (GetMonitorInfo(hMonitor, info)) monitors.Add(new KeyValuePair<IntPtr, RECT>(hMonitor, info.rcMonitor));
                    return true;
                }, IntPtr.Zero);
                // Active display first, then left to right.
                monitors.Sort((a, b) =>
                {
                    bool fa = a.Key == activeMonitor, fb = b.Key == activeMonitor;
                    if (fa != fb) return fa ? -1 : 1;
                    return a.Value.Left.CompareTo(b.Value.Left);
                });

                foreach (var monitor in monitors)
                {
                    var bounds = monitor.Value;
                    bool active = monitor.Key == activeMonitor;
                    Rectangle? highlight = null;
                    if (active && target != IntPtr.Zero)
                        highlight = new Rectangle(targetRect.Left - bounds.Left, targetRect.Top - bounds.Top,
                                                  targetRect.Right - targetRect.Left, targetRect.Bottom - targetRect.Top);
                    var jpeg = CaptureMonitor(bounds, highlight, maxWidth);
                    if (jpeg == null) continue;
                    if (active) { set.ActiveIndex = set.Images.Count; set.Outlined = highlight.HasValue; }
                    set.Images.Add(jpeg);
                }
            }
            catch (Exception e)
            {
                Log.Error("Screenshot failed: " + e.Message);
            }
            return set.IsEmpty ? null : set;
        }

        private static byte[] CaptureMonitor(RECT bounds, Rectangle? highlight, int maxWidth)
        {
            int width = bounds.Right - bounds.Left, height = bounds.Bottom - bounds.Top;
            if (width < 8 || height < 8) return null;
            using (var bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb))
            {
                using (var graphics = Graphics.FromImage(bitmap))
                    graphics.CopyFromScreen(bounds.Left, bounds.Top, 0, 0, new Size(width, height));
                var scale = Math.Min(1.0, maxWidth / (double)width);
                using (var scaled = new Bitmap(bitmap, new Size((int)(width * scale), (int)(height * scale))))
                using (var stream = new MemoryStream())
                {
                    if (highlight.HasValue)
                    {
                        var h = highlight.Value;
                        var r = new Rectangle((int)(h.X * scale), (int)(h.Y * scale), (int)(h.Width * scale), (int)(h.Height * scale));
                        r.Inflate(-3, -3);
                        using (var graphics = Graphics.FromImage(scaled))
                        using (var pen = new Pen(Color.FromArgb(255, 25, 25), 6))
                            graphics.DrawRectangle(pen, r);
                    }
                    var encoder = ImageCodecInfo.GetImageEncoders()[1]; // JPEG
                    foreach (var codec in ImageCodecInfo.GetImageEncoders())
                        if (codec.MimeType == "image/jpeg") encoder = codec;
                    var parameters = new EncoderParameters(1);
                    parameters.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, 60L);
                    scaled.Save(stream, encoder, parameters);
                    return stream.ToArray();
                }
            }
        }
    }
}
