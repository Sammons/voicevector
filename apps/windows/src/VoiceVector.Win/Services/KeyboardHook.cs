using System;
using System.Runtime.InteropServices;
using System.Windows.Threading;
using VoiceVector.Shared;

namespace VoiceVector.Win.Services
{
    /// <summary>
    /// Global hotkey via a low-level keyboard hook (userland, no admin).
    /// Matched non-modifier hotkeys are swallowed; modifier-only ones pass
    /// through. Requires a message loop on the installing thread (WPF UI).
    /// </summary>
    public sealed class KeyboardHook
    {
        /// <summary>(action, profileId) — the initiating profile decides cleanup.</summary>
        public event Action<TapStateMachine.Act, Guid> OnAction;
        public Action<HotkeySpec> CaptureHandler;
        public bool RecordingActive;

        private readonly Func<AppConfig> _config;
        private readonly Dispatcher _dispatcher;
        private IntPtr _hook;
        private Native.LowLevelKeyboardProc _proc; // kept alive for the GC
        private TapStateMachine _machine;
        private DispatcherTimer _expiryTimer;
        private readonly System.Collections.Generic.Dictionary<Guid, bool> _hotkeyIsDown =
            new System.Collections.Generic.Dictionary<Guid, bool>();
        private Guid _activeProfileId;

        private static readonly int[] ModifierVks =
            { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0x5B, 0x5C, 0x14 };

        public KeyboardHook(Func<AppConfig> config, Dispatcher dispatcher)
        {
            _config = config;
            _dispatcher = dispatcher;
        }

        private static double Now { get { return Environment.TickCount / 1000.0; } }

        public void Start()
        {
            Reconfigure();
            _proc = HookProc;
            _hook = Native.SetWindowsHookExW(Native.WH_KEYBOARD_LL, _proc, IntPtr.Zero, 0);
            if (_hook == IntPtr.Zero)
                Log.Error("Keyboard hook installation failed (win32 "
                          + Marshal.GetLastWin32Error() + ")");
        }

        public void Stop()
        {
            if (_hook != IntPtr.Zero) Native.UnhookWindowsHookEx(_hook);
            _hook = IntPtr.Zero;
        }

        public void Reconfigure()
        {
            _machine = new TapStateMachine(_config().TapStartMode);
            _hotkeyIsDown.Clear();
            _activeProfileId = Guid.Empty;
        }

        private IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam)
        {
            if (nCode < 0) return Native.CallNextHookEx(_hook, nCode, wParam, lParam);

            var info = (Native.KBDLLHOOKSTRUCT)Marshal.PtrToStructure(
                lParam, typeof(Native.KBDLLHOOKSTRUCT));
            int message = (int)wParam;
            bool isDown = message == Native.WM_KEYDOWN || message == Native.WM_SYSKEYDOWN;
            bool isUp = message == Native.WM_KEYUP || message == Native.WM_SYSKEYUP;
            if (!isDown && !isUp) return Native.CallNextHookEx(_hook, nCode, wParam, lParam);
            int vk = (int)info.vkCode;

            var capture = CaptureHandler;
            if (capture != null)
            {
                if (isDown && vk != Native.VK_ESCAPE)
                {
                    bool isModifier = Array.IndexOf(ModifierVks, vk) >= 0;
                    var spec = new HotkeySpec
                    {
                        KeyCode = vk,
                        Modifiers = isModifier ? 0 : CurrentModifierMask(),
                        IsModifierOnly = isModifier,
                    };
                    _dispatcher.BeginInvoke((Action)(() => capture(spec)));
                    return (IntPtr)1;
                }
                return Native.CallNextHookEx(_hook, nCode, wParam, lParam);
            }

            if (RecordingActive && isDown && vk == Native.VK_ESCAPE)
            {
                Emit(_machine.Cancel());
                return (IntPtr)1;
            }

            // Try each profile's hotkey; an in-flight gesture only accepts
            // events from its initiating profile. First matching spec wins.
            foreach (var profile in _config().DictationProfiles)
            {
                if (_machine.IsActive && _activeProfileId != profile.Id) continue;
                var hotkey = profile.Hotkey;
                if (vk != hotkey.KeyCode) continue;
                if (hotkey.KeyCode == 0) continue; // unset profile hotkey
                if (!hotkey.IsModifierOnly && !_machine.IsActive && isDown
                    && CurrentModifierMask() != hotkey.Modifiers)
                    continue;

                bool wasDown;
                _hotkeyIsDown.TryGetValue(profile.Id, out wasDown);
                if (isDown)
                {
                    if (wasDown)
                        return hotkey.IsModifierOnly
                            ? Native.CallNextHookEx(_hook, nCode, wParam, lParam) : (IntPtr)1;
                    _hotkeyIsDown[profile.Id] = true;
                    if (!_machine.IsActive) _activeProfileId = profile.Id;
                    Emit(_machine.KeyDown(Now));
                }
                else
                {
                    _hotkeyIsDown[profile.Id] = false;
                    Emit(_machine.KeyUp(Now));
                }
                return hotkey.IsModifierOnly
                    ? Native.CallNextHookEx(_hook, nCode, wParam, lParam) : (IntPtr)1;
            }
            return Native.CallNextHookEx(_hook, nCode, wParam, lParam);
        }

        private static int CurrentModifierMask()
        {
            int mask = 0;
            if ((Native.GetAsyncKeyState(0x11) & 0x8000) != 0) mask |= 1; // Ctrl
            if ((Native.GetAsyncKeyState(0x12) & 0x8000) != 0) mask |= 2; // Alt
            if ((Native.GetAsyncKeyState(0x10) & 0x8000) != 0) mask |= 4; // Shift
            if ((Native.GetAsyncKeyState(0x5B) & 0x8000) != 0
                || (Native.GetAsyncKeyState(0x5C) & 0x8000) != 0) mask |= 8; // Win
            return mask;
        }

        private void Emit(TapStateMachine.Act[] actions)
        {
            ScheduleExpiry();
            if (actions.Length == 0) return;
            var profileId = _activeProfileId;
            _dispatcher.BeginInvoke((Action)(() =>
            {
                var handler = OnAction;
                if (handler == null) return;
                foreach (var action in actions) handler(action, profileId);
            }));
        }

        private void ScheduleExpiry()
        {
            var deadline = _machine.PendingDeadline;
            _dispatcher.BeginInvoke((Action)(() =>
            {
                if (_expiryTimer != null) _expiryTimer.Stop();
                if (deadline == null) return;
                if (_expiryTimer == null)
                {
                    _expiryTimer = new DispatcherTimer(DispatcherPriority.Normal, _dispatcher);
                    _expiryTimer.Tick += (s, e) =>
                    {
                        _expiryTimer.Stop();
                        Emit(_machine.Expire(Now));
                    };
                }
                var delay = Math.Max(0.01, deadline.Value - Now);
                _expiryTimer.Interval = TimeSpan.FromSeconds(delay);
                _expiryTimer.Start();
            }));
        }

        // -- display names -----------------------------------------------------

        public static string Describe(HotkeySpec spec)
        {
            var parts = new System.Collections.Generic.List<string>();
            if (!spec.IsModifierOnly)
            {
                if ((spec.Modifiers & 1) != 0) parts.Add("Ctrl");
                if ((spec.Modifiers & 2) != 0) parts.Add("Alt");
                if ((spec.Modifiers & 4) != 0) parts.Add("Shift");
                if ((spec.Modifiers & 8) != 0) parts.Add("Win");
            }
            parts.Add(KeyName(spec.KeyCode));
            return string.Join("+", parts);
        }

        public static string KeyName(int vk)
        {
            switch (vk)
            {
                case 0xA0: return "Left Shift";
                case 0xA1: return "Right Shift";
                case 0xA2: return "Left Ctrl";
                case 0xA3: return "Right Ctrl";
                case 0xA4: return "Left Alt";
                case 0xA5: return "Right Alt";
                case 0x5B: return "Left Win";
                case 0x5C: return "Right Win";
                case 0x14: return "CapsLock";
                case 0x20: return "Space";
                case 0x0D: return "Enter";
                case 0x09: return "Tab";
                case 0x1B: return "Esc";
                case 0x08: return "Backspace";
            }
            if (vk >= 0x70 && vk <= 0x87) return "F" + (vk - 0x6F);
            if ((vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A))
                return ((char)vk).ToString();
            return "VK 0x" + vk.ToString("X2");
        }
    }
}
