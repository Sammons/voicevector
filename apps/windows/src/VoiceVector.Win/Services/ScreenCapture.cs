using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using VoiceVector.Shared;

namespace VoiceVector.Win.Services
{
    /// <summary>Screenshot of the foreground window (JPEG, ≤1280 px wide)
    /// for LLM context. Mirrors the macOS ScreenCapture; no special
    /// permission is needed on Windows.</summary>
    public static class ScreenCapture
    {
        [StructLayout(LayoutKind.Sequential)]
        private struct RECT { public int Left, Top, Right, Bottom; }

        [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
        [DllImport("user32.dll")] private static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
        private const uint PW_RENDERFULLCONTENT = 2;

        public static byte[] ForegroundWindowJpeg(int maxWidth = 1280)
        {
            try
            {
                var hwnd = GetForegroundWindow();
                if (hwnd == IntPtr.Zero) return null;
                uint pid;
                GetWindowThreadProcessId(hwnd, out pid);
                if (pid == (uint)System.Diagnostics.Process.GetCurrentProcess().Id) return null;
                RECT rect;
                if (!GetWindowRect(hwnd, out rect)) return null;
                int width = rect.Right - rect.Left, height = rect.Bottom - rect.Top;
                if (width < 8 || height < 8) return null;

                using (var bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb))
                {
                    using (var graphics = Graphics.FromImage(bitmap))
                    {
                        var hdc = graphics.GetHdc();
                        try
                        {
                            if (!PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT))
                            {
                                graphics.ReleaseHdc(hdc);
                                graphics.CopyFromScreen(rect.Left, rect.Top, 0, 0, new Size(width, height));
                                hdc = IntPtr.Zero;
                            }
                        }
                        finally
                        {
                            if (hdc != IntPtr.Zero) graphics.ReleaseHdc(hdc);
                        }
                    }
                    var scale = Math.Min(1.0, maxWidth / (double)width);
                    using (var scaled = new Bitmap(bitmap, new Size((int)(width * scale), (int)(height * scale))))
                    using (var stream = new MemoryStream())
                    {
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
            catch (Exception e)
            {
                Log.Error("Screenshot failed: " + e.Message);
                return null;
            }
        }
    }
}
