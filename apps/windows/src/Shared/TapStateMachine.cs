using System;

namespace VoiceVector.Shared
{
    /// <summary>
    /// Pure hotkey-gesture logic — behavior-identical to the macOS app (see
    /// docs/config-schema.md "Hotkey semantics"). Recording starts on the very
    /// first key-down; a stray single tap is discarded after the tap window.
    /// </summary>
    public class TapStateMachine
    {
        public enum Act { StartRecording, Commit, Discard }

        public enum PhaseKind { Idle, Pressed, AwaitingSecondTap, Latched, Draining }

        public TapStartMode StartMode { get; private set; }
        public double HoldThreshold = 0.35;
        public double TapWindow = 0.40;

        public PhaseKind Phase { get; private set; }
        private double _pressedSince;
        private int _tap;
        private double _deadline;

        public TapStateMachine(TapStartMode startMode)
        {
            StartMode = startMode;
            Phase = PhaseKind.Idle;
        }

        public double? PendingDeadline
        {
            get { return Phase == PhaseKind.AwaitingSecondTap ? _deadline : (double?)null; }
        }

        public bool IsActive { get { return Phase != PhaseKind.Idle; } }

        private int TapsRequired { get { return StartMode == TapStartMode.DoubleTap ? 2 : 1; } }

        public Act[] KeyDown(double now)
        {
            switch (Phase)
            {
                case PhaseKind.Idle:
                    Phase = PhaseKind.Pressed;
                    _pressedSince = now;
                    _tap = 1;
                    return new[] { Act.StartRecording };
                case PhaseKind.AwaitingSecondTap:
                    Phase = PhaseKind.Pressed;
                    _pressedSince = now;
                    _tap = 2;
                    return Array.Empty<Act>();
                case PhaseKind.Latched:
                    Phase = PhaseKind.Draining;
                    return new[] { Act.Commit };
                default:
                    return Array.Empty<Act>();
            }
        }

        public Act[] KeyUp(double now)
        {
            switch (Phase)
            {
                case PhaseKind.Pressed:
                    if (now - _pressedSince >= HoldThreshold)
                    {
                        Phase = PhaseKind.Idle;
                        return new[] { Act.Commit }; // hold-to-talk
                    }
                    if (_tap >= TapsRequired)
                    {
                        Phase = PhaseKind.Latched;
                        return Array.Empty<Act>();
                    }
                    Phase = PhaseKind.AwaitingSecondTap;
                    _deadline = now + TapWindow;
                    return Array.Empty<Act>();
                case PhaseKind.Draining:
                    Phase = PhaseKind.Idle;
                    return Array.Empty<Act>();
                default:
                    return Array.Empty<Act>();
            }
        }

        public Act[] Expire(double now)
        {
            if (Phase == PhaseKind.AwaitingSecondTap && now >= _deadline)
            {
                Phase = PhaseKind.Idle;
                return new[] { Act.Discard };
            }
            return Array.Empty<Act>();
        }

        public Act[] Cancel()
        {
            bool wasActive = IsActive;
            Phase = PhaseKind.Idle;
            return wasActive ? new[] { Act.Discard } : Array.Empty<Act>();
        }
    }
}
