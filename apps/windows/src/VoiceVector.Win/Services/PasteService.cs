using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using VoiceVector.Shared;

namespace VoiceVector.Win.Services
{
    /// <summary>
    /// Inserts a transcript into the foreground app: snapshot the clipboard
    /// (all HGLOBAL formats), write the text marked to be excluded from Win+V
    /// history/monitors, wait for physical modifier release, synthesize Ctrl+V,
    /// then restore the snapshot if we still own the clipboard.
    /// </summary>
    public static class PasteService
    {
        public enum Outcome { Pasted, CopiedOnly }

        private const int PrePasteDelayMs = 100;
        private const int RestoreDelayMs = 900;

        private static readonly uint[] SkipFormats =
            { 2 /*CF_BITMAP*/, 3 /*CF_METAFILEPICT*/, 14 /*CF_ENHMETAFILE*/,
              0x0082, 0x008E, 0x0080 /*owner display*/ };

        public static async Task<Outcome> InsertAsync(string text, bool autoPaste)
        {
            text = text.TrimEnd('\r', '\n'); // trailing newline auto-submits in terminals

            if (!autoPaste)
            {
                WritePlainText(text, false);
                return Outcome.CopiedOnly;
            }

            var snapshot = Snapshot();
            WritePlainText(text, true);
            uint ourSequence = Native.GetClipboardSequenceNumber();

            await Task.Delay(PrePasteDelayMs).ConfigureAwait(false);
            await WaitForModifierReleaseAsync().ConfigureAwait(false);

            if (!SendCtrlV())
            {
                WritePlainText(text, false);
                return Outcome.CopiedOnly;
            }

            var saved = snapshot;
            var seq = ourSequence;
            var _ = Task.Run(async () =>
            {
                await Task.Delay(RestoreDelayMs).ConfigureAwait(false);
                if (Native.GetClipboardSequenceNumber() == seq && saved.Count > 0)
                    Restore(saved);
            });
            return Outcome.Pasted;
        }

        private static async Task WaitForModifierReleaseAsync()
        {
            for (int i = 0; i < 100; i++)
            {
                bool held = ((Native.GetAsyncKeyState(0x12) & 0x8000) != 0)
                            || ((Native.GetAsyncKeyState(0x10) & 0x8000) != 0)
                            || ((Native.GetAsyncKeyState(0x5B) & 0x8000) != 0)
                            || ((Native.GetAsyncKeyState(0x5C) & 0x8000) != 0);
                if (!held) return;
                await Task.Delay(10).ConfigureAwait(false);
            }
        }

        private static bool SendCtrlV()
        {
            var inputs = new Native.INPUT[4];
            inputs[0] = Key(Native.VK_CONTROL, true);
            inputs[1] = Key(Native.VK_V, true);
            inputs[2] = Key(Native.VK_V, false);
            inputs[3] = Key(Native.VK_CONTROL, false);
            uint sent = Native.SendInput((uint)inputs.Length, inputs,
                                         Marshal.SizeOf(typeof(Native.INPUT)));
            if (sent != inputs.Length)
            {
                Log.Error("SendInput sent " + sent + "/4 events");
                return false;
            }
            return true;
        }

        private static Native.INPUT Key(ushort vk, bool down)
        {
            return new Native.INPUT
            {
                type = Native.INPUT_KEYBOARD,
                ki = new Native.KEYBDINPUT { wVk = vk, dwFlags = down ? 0u : Native.KEYEVENTF_KEYUP },
            };
        }

        // -- clipboard plumbing ------------------------------------------------

        private static bool TryOpenClipboard()
        {
            for (int i = 0; i < 10; i++)
            {
                if (Native.OpenClipboard(IntPtr.Zero)) return true;
                Thread.Sleep(15);
            }
            return false;
        }

        private static System.Collections.Generic.Dictionary<uint, byte[]> Snapshot()
        {
            var items = new System.Collections.Generic.Dictionary<uint, byte[]>();
            if (!TryOpenClipboard()) return items;
            try
            {
                uint format = 0;
                while ((format = Native.EnumClipboardFormats(format)) != 0)
                {
                    if (Array.IndexOf(SkipFormats, format) >= 0) continue;
                    IntPtr handle = Native.GetClipboardData(format);
                    if (handle == IntPtr.Zero) continue;
                    IntPtr pointer = Native.GlobalLock(handle);
                    if (pointer == IntPtr.Zero) continue;
                    try
                    {
                        int size = (int)Native.GlobalSize(handle);
                        if (size <= 0 || size > 64 * 1024 * 1024) continue;
                        var data = new byte[size];
                        Marshal.Copy(pointer, data, 0, size);
                        items[format] = data;
                    }
                    finally { Native.GlobalUnlock(handle); }
                }
            }
            finally { Native.CloseClipboard(); }
            return items;
        }

        private static void Restore(System.Collections.Generic.Dictionary<uint, byte[]> snapshot)
        {
            if (!TryOpenClipboard()) return;
            try
            {
                Native.EmptyClipboard();
                foreach (var pair in snapshot) SetData(pair.Key, pair.Value);
                MarkTransient();
            }
            finally { Native.CloseClipboard(); }
        }

        private static void WritePlainText(string text, bool markTransient)
        {
            if (!TryOpenClipboard()) return;
            try
            {
                Native.EmptyClipboard();
                var bytes = new byte[(text.Length + 1) * 2];
                Encoding.Unicode.GetBytes(text, 0, text.Length, bytes, 0);
                SetData(Native.CF_UNICODETEXT, bytes);
                if (markTransient) MarkTransient();
            }
            finally { Native.CloseClipboard(); }
        }

        private static void MarkTransient()
        {
            SetData(Native.RegisterClipboardFormatW("ExcludeClipboardContentFromMonitorProcessing"),
                    new byte[] { 1, 0, 0, 0 });
            SetData(Native.RegisterClipboardFormatW("CanIncludeInClipboardHistory"),
                    new byte[] { 0, 0, 0, 0 });
            SetData(Native.RegisterClipboardFormatW("CanUploadToCloudClipboard"),
                    new byte[] { 0, 0, 0, 0 });
        }

        private static void SetData(uint format, byte[] data)
        {
            IntPtr handle = Native.GlobalAlloc(Native.GMEM_MOVEABLE, (UIntPtr)data.Length);
            if (handle == IntPtr.Zero) return;
            IntPtr pointer = Native.GlobalLock(handle);
            if (pointer == IntPtr.Zero) { Native.GlobalFree(handle); return; }
            Marshal.Copy(data, 0, pointer, data.Length);
            Native.GlobalUnlock(handle);
            if (Native.SetClipboardData(format, handle) == IntPtr.Zero)
                Native.GlobalFree(handle);
        }
    }
}
