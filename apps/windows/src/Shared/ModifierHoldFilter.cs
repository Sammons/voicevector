namespace VoiceVector.Shared
{
    /// <summary>
    /// Decides what reaches the foreground app while an Alt or Win key is the
    /// dictation hotkey (Windows only; the macOS tap passes modifiers freely
    /// because a lone Option/Command tap does nothing there).
    ///
    /// On Windows a lone Alt press-and-release puts the foreground app into
    /// menu / access-key mode (a lone Win opens Start). Passing the hotkey
    /// through therefore swallowed the synthesized Ctrl+V that follows every
    /// dictation. Instead the modifier is held back: a pure tap never reaches
    /// the app, and if another key is pressed while it is down (a real combo
    /// such as Alt+Tab or AltGr+e), the modifier is replayed just ahead of that
    /// key so the combo still works.
    ///
    /// Pure logic so the self-test can cover it; the hook does the injecting.
    /// </summary>
    public sealed class ModifierHoldFilter
    {
        public enum Verdict
        {
            /// <summary>Let the event through unchanged.</summary>
            Pass,
            /// <summary>Drop the event.</summary>
            Swallow,
            /// <summary>Drop the event and inject the held modifier's key-down
            /// followed by this key-down, preserving their order.</summary>
            ReplayModifierThenKey,
        }

        private int _heldVk;
        private bool _replayed;

        /// <summary>Left/Right Alt and Left/Right Win: the keys whose lone tap
        /// triggers menu mode or Start. Ctrl/Shift/CapsLock taps are harmless
        /// and keep passing through.</summary>
        public static bool ShouldHold(int vk)
        {
            return vk == 0xA4 || vk == 0xA5 || vk == 0x5B || vk == 0x5C;
        }

        /// <summary>The modifier currently held back (0 when none).</summary>
        public int HeldVk { get { return _heldVk; } }

        /// <summary>True once the held modifier has been replayed for a combo.</summary>
        public bool Replayed { get { return _replayed; } }

        /// <summary>A key event for the hotkey modifier itself (including
        /// auto-repeat downs).</summary>
        public Verdict OnHotkey(int vk, bool down)
        {
            if (!ShouldHold(vk)) return Verdict.Pass;
            if (down)
            {
                if (_heldVk == vk) return _replayed ? Verdict.Pass : Verdict.Swallow; // auto-repeat
                _heldVk = vk;
                _replayed = false;
                return Verdict.Swallow;
            }
            // Up for a down we never held (e.g. the hook started mid-press):
            // the app saw that down, so it must see the up too.
            if (_heldVk != vk) return Verdict.Pass;
            bool replayed = _replayed;
            _heldVk = 0;
            _replayed = false;
            return replayed ? Verdict.Pass : Verdict.Swallow;
        }

        /// <summary>Any other key event while the filter may be holding.</summary>
        public Verdict OnOtherKey(int vk, bool down)
        {
            if (_heldVk == 0 || _replayed || !down || vk == _heldVk) return Verdict.Pass;
            _replayed = true;
            return Verdict.ReplayModifierThenKey;
        }
    }
}
