using System;
using System.Runtime.InteropServices;
using System.Threading;
using VoiceVector.Shared;

namespace VoiceVector.Win.Services
{
    /// <summary>
    /// Microphone capture via raw WASAPI COM interop (built into Windows —
    /// no WinRT, no NAudio): shared-mode capture at the device's mix format
    /// (typically 32-bit float 44.1/48 kHz), converted to 16 kHz mono 16-bit
    /// with a linear resampler, streamed to a WAV file. Publishes a smoothed
    /// level for the HUD and supports silence-gap segmentation.
    /// </summary>
    public sealed class WasapiRecorder
    {
        public float Level { get; private set; }
        public bool IsRecording { get { return _writer != null; } }
        public TimeSpan Elapsed
        {
            get { return _startedAt == default(DateTime) ? TimeSpan.Zero : DateTime.UtcNow - _startedAt; }
        }

        // Silence-gap segmentation (streamed transcription).
        public bool Chunking;
        public Action<byte[], int> OnSegment;
        public uint TailStartByte { get; private set; }
        private int _segmentIndex;
        private double _lastVoicedAt;
        private bool _voicedInSegment;
        private const double SilenceCutAfter = 2.0;
        private const double MinSegmentSeconds = 5.0;
        // Voice detection is relative to a tracked noise floor so quiet,
        // un-boosted inputs (audio interfaces at conservative gain) still
        // segment; anything above the absolute ceiling always counts.
        private const float VoiceRmsCeiling = 0.015f;
        private const float VoiceRmsFloor = 0.001f;
        private float _noiseFloor = 0.001f;
        /// <summary>HUD level on a fixed, quantized scale (mirrors macOS):
        /// −60 dBFS is silence, −15 dBFS is full, crushed to 3 steps.</summary>
        public static float DisplayLevel(float rms)
        {
            var db = 20 * Math.Log10(Math.Max(rms, 1e-6));
            var normalized = Math.Min(1, Math.Max(0, (db + 60) / 45));
            return (float)(Math.Ceiling(normalized * 3) / 3);
        }

        private float Meter(float rms) { return DisplayLevel(rms); }

        private bool IsVoiced(float rms)
        {
            if (rms < _noiseFloor) _noiseFloor = rms;
            else _noiseFloor = Math.Min(rms, _noiseFloor * 1.02f);
            return rms > VoiceRmsCeiling || (rms > VoiceRmsFloor && rms > _noiseFloor * 3);
        }

        private WavWriter _writer;
        private readonly object _writerLock = new object();
        private DateTime _startedAt;
        private Thread _thread;
        private volatile bool _stopping;
        private double _resamplePos;
        private float _lastSample;

        /// <summary>Warm policy (see AppConfig.KeepMicWarm*): the capture
        /// thread can keep the device open between recordings, discarding
        /// audio until the next Start.</summary>
        public bool WarmAfterRecording = true;
        public bool AlwaysWarm = false;
        private const int WarmIdleMs = 15000;
        private System.Threading.Timer _idleStop;

        private static double Now { get { return Environment.TickCount / 1000.0; } }

        public void Start(string wavPath)
        {
            if (_writer != null) return;
            CancelIdleStop();
            var writer = new WavWriter(wavPath);
            lock (_writerLock)
            {
                _startedAt = DateTime.UtcNow;
                TailStartByte = 0;
                _segmentIndex = 0;
                _voicedInSegment = false;
                _lastVoicedAt = Now;
                _resamplePos = 0;
                _lastSample = 0;
                _writer = writer;
            }
            EnsureCaptureThread();
        }

        public double Stop()
        {
            WavWriter writer;
            lock (_writerLock)
            {
                writer = _writer;
                if (writer == null) return 0;
                _writer = null;
                Level = 0;
                _noiseFloor = 0.001f;
                _startedAt = default(DateTime);
            }
            if (AlwaysWarm)
            {
                // Device stays open.
            }
            else if (WarmAfterRecording)
            {
                CancelIdleStop();
                _idleStop = new System.Threading.Timer(_ =>
                {
                    if (_writer == null && !AlwaysWarm) StopCaptureThread();
                }, null, WarmIdleMs, System.Threading.Timeout.Infinite);
            }
            else
            {
                StopCaptureThread();
            }
            return writer.FinalizeFile();
        }

        /// <summary>Applies the warm policy while idle.</summary>
        public void ApplyWarmPolicy()
        {
            if (_writer != null) return;
            if (AlwaysWarm) { CancelIdleStop(); EnsureCaptureThread(); }
            else if (!WarmAfterRecording) { CancelIdleStop(); StopCaptureThread(); }
        }

        private void CancelIdleStop()
        {
            var timer = _idleStop;
            _idleStop = null;
            if (timer != null) timer.Dispose();
        }

        private void EnsureCaptureThread()
        {
            var thread = _thread;
            if (thread != null && thread.IsAlive && !_stopping) return;
            if (thread != null && thread.IsAlive) thread.Join(3000);
            _stopping = false;
            _thread = new Thread(CaptureLoop) { IsBackground = true, Name = "vv-capture" };
            _thread.SetApartmentState(ApartmentState.MTA);
            _thread.Start();
        }

        private void StopCaptureThread()
        {
            var thread = _thread;
            if (thread == null) return;
            _stopping = true;
            if (thread.IsAlive) thread.Join(3000);
            _thread = null;
        }

        public void Discard()
        {
            var path = _writer != null ? _writer.Path : null;
            Stop();
            if (path != null)
            {
                try { System.IO.File.Delete(path); } catch { }
            }
        }

        // -- capture thread ----------------------------------------------------

        private void CaptureLoop()
        {
            IMMDeviceEnumerator enumerator = null;
            IMMDevice device = null;
            IAudioClient audioClient = null;
            IAudioCaptureClient captureClient = null;
            IntPtr formatPtr = IntPtr.Zero;
            var startError = (string)null;
            try
            {
                enumerator = (IMMDeviceEnumerator)new MMDeviceEnumeratorComObject();
                int hr = enumerator.GetDefaultAudioEndpoint(EDataFlow.eCapture, ERole.eConsole, out device);
                if (hr != 0 || device == null)
                {
                    startError = "No microphone found (0x" + hr.ToString("X8") + "). "
                        + "Check Windows Settings → Privacy → Microphone.";
                    return;
                }
                object clientObj;
                var iidAudioClient = typeof(IAudioClient).GUID;
                Marshal.ThrowExceptionForHR(device.Activate(ref iidAudioClient, ClsCtx.ALL,
                                                            IntPtr.Zero, out clientObj));
                audioClient = (IAudioClient)clientObj;
                Marshal.ThrowExceptionForHR(audioClient.GetMixFormat(out formatPtr));
                var format = (WaveFormatEx)Marshal.PtrToStructure(formatPtr, typeof(WaveFormatEx));

                bool isFloat = format.wFormatTag == 3
                    || (format.wFormatTag == 0xFFFE && HasFloatSubformat(formatPtr));
                if (!isFloat && format.wBitsPerSample != 16)
                {
                    startError = "Unsupported capture format (" + format.wBitsPerSample + " bits).";
                    return;
                }

                // 100 ms buffer, shared mode, event-free polling.
                Marshal.ThrowExceptionForHR(audioClient.Initialize(AudioClientShareMode.Shared, 0,
                    1000000, 0, formatPtr, Guid.Empty));
                object captureObj;
                var iidCapture = typeof(IAudioCaptureClient).GUID;
                Marshal.ThrowExceptionForHR(audioClient.GetService(ref iidCapture, out captureObj));
                captureClient = (IAudioCaptureClient)captureObj;
                Marshal.ThrowExceptionForHR(audioClient.Start());

                int channels = format.nChannels;
                double sourceRate = format.nSamplesPerSec;
                double step = sourceRate / WavWriter.TargetSampleRate;
                var outBuffer = new short[8192];

                while (!_stopping)
                {
                    uint packet;
                    if (captureClient.GetNextPacketSize(out packet) != 0) break;
                    if (packet == 0) { Thread.Sleep(10); continue; }

                    IntPtr data;
                    uint frames;
                    uint flags;
                    long pos1, pos2;
                    if (captureClient.GetBuffer(out data, out frames, out flags, out pos1, out pos2) != 0)
                        break;
                    try
                    {
                        ProcessPacket(data, (int)frames, channels, isFloat,
                                      format.wBitsPerSample, step, outBuffer,
                                      (flags & 1) != 0 /* AUDCLNT_BUFFERFLAGS_SILENT */);
                    }
                    finally
                    {
                        captureClient.ReleaseBuffer(frames);
                    }
                }
                audioClient.Stop();
            }
            catch (Exception e)
            {
                startError = e.Message;
            }
            finally
            {
                if (formatPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(formatPtr);
                ReleaseCom(captureClient);
                ReleaseCom(audioClient);
                ReleaseCom(device);
                ReleaseCom(enumerator);
                if (startError != null) Log.Error("Recorder: " + startError);
            }
        }

        private void ProcessPacket(IntPtr data, int frames, int channels, bool isFloat,
                                   int bits, double step, short[] outBuffer, bool silent)
        {
            if (frames == 0) return;
            lock (_writerLock)
            {
                var writer = _writer;
                if (writer == null) return; // warm idle: discard
                ProcessPacketLocked(writer, data, frames, channels, isFloat, bits, step, outBuffer, silent);
            }
        }

        private void ProcessPacketLocked(WavWriter writer, IntPtr data, int frames, int channels,
                                         bool isFloat, int bits, double step, short[] outBuffer,
                                         bool silent)
        {

            // Downmix to mono float.
            var mono = new float[frames];
            if (silent)
            {
                // leave zeros
            }
            else if (isFloat)
            {
                unsafe
                {
                    float* src = (float*)data;
                    for (int i = 0; i < frames; i++)
                    {
                        float sum = 0;
                        for (int c = 0; c < channels; c++) sum += src[i * channels + c];
                        mono[i] = sum / channels;
                    }
                }
            }
            else // 16-bit PCM
            {
                unsafe
                {
                    short* src = (short*)data;
                    for (int i = 0; i < frames; i++)
                    {
                        float sum = 0;
                        for (int c = 0; c < channels; c++) sum += src[i * channels + c] / 32768f;
                        mono[i] = sum / channels;
                    }
                }
            }

            // Level metering.
            float rms = 0;
            for (int i = 0; i < frames; i++) rms += mono[i] * mono[i];
            rms = (float)Math.Sqrt(rms / frames);
            bool voiced = IsVoiced(rms);
            Level = Math.Max(Meter(rms), Level * 0.7f);

            // Linear resample source-rate → 16 kHz.
            int outCount = 0;
            for (int i = 0; i < frames; i++)
            {
                float current = mono[i];
                while (_resamplePos < 1.0)
                {
                    float sample = _lastSample + (float)_resamplePos * (current - _lastSample);
                    float clamped = Math.Max(-1f, Math.Min(1f, sample));
                    if (outCount == outBuffer.Length)
                    {
                        writer.Append(outBuffer, outCount);
                        outCount = 0;
                    }
                    outBuffer[outCount++] = (short)(clamped * short.MaxValue);
                    _resamplePos += step;
                }
                _resamplePos -= 1.0;
                _lastSample = current;
            }
            if (outCount > 0) writer.Append(outBuffer, outCount);

            // Silence-gap segmentation.
            if (Chunking)
            {
                double now = Now;
                if (voiced)
                {
                    _lastVoicedAt = now;
                    _voicedInSegment = true;
                }
                else if (_voicedInSegment && now - _lastVoicedAt >= SilenceCutAfter)
                {
                    uint current = writer.DataBytes;
                    double seconds = (current - TailStartByte) / (double)(2 * WavWriter.TargetSampleRate);
                    if (seconds >= MinSegmentSeconds)
                    {
                        try
                        {
                            var slice = WavWriter.SliceWav(writer.Path, TailStartByte, current);
                            var handler = OnSegment;
                            if (handler != null) handler(slice, _segmentIndex++);
                            TailStartByte = current;
                            _voicedInSegment = false;
                        }
                        catch (Exception e)
                        {
                            Log.Error("Segment slice failed: " + e.Message);
                        }
                    }
                }
            }
        }

        private static bool HasFloatSubformat(IntPtr formatPtr)
        {
            // WAVEFORMATEXTENSIBLE: SubFormat GUID sits at offset 24; float PCM
            // subformat starts with 0x00000003.
            int first = Marshal.ReadInt32(formatPtr, 24);
            return first == 3;
        }

        private static void ReleaseCom(object obj)
        {
            if (obj != null && Marshal.IsComObject(obj)) Marshal.ReleaseComObject(obj);
        }

        // -- WASAPI COM definitions -------------------------------------------

        private enum EDataFlow { eRender = 0, eCapture = 1 }
        private enum ERole { eConsole = 0 }

        [Flags]
        private enum ClsCtx : uint { ALL = 0x17 }

        private enum AudioClientShareMode { Shared = 0 }

        [StructLayout(LayoutKind.Sequential, Pack = 2)]
        private struct WaveFormatEx
        {
            public ushort wFormatTag;
            public ushort nChannels;
            public uint nSamplesPerSec;
            public uint nAvgBytesPerSec;
            public ushort nBlockAlign;
            public ushort wBitsPerSample;
            public ushort cbSize;
        }

        [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
        private class MMDeviceEnumeratorComObject { }

        [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),
         InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMMDeviceEnumerator
        {
            int EnumAudioEndpoints(EDataFlow dataFlow, uint stateMask, out IntPtr devices);
            int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, out IMMDevice endpoint);
        }

        [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"),
         InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMMDevice
        {
            int Activate(ref Guid iid, ClsCtx clsCtx, IntPtr activationParams,
                         [MarshalAs(UnmanagedType.IUnknown)] out object result);
        }

        [ComImport, Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2"),
         InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAudioClient
        {
            int Initialize(AudioClientShareMode shareMode, uint streamFlags,
                           long bufferDuration, long periodicity, IntPtr format, Guid audioSessionGuid);
            int GetBufferSize(out uint bufferFrameCount);
            int GetStreamLatency(out long latency);
            int GetCurrentPadding(out uint paddingFrameCount);
            int IsFormatSupported(AudioClientShareMode shareMode, IntPtr format, out IntPtr closestMatch);
            int GetMixFormat(out IntPtr format);
            int GetDevicePeriod(out long defaultPeriod, out long minimumPeriod);
            int Start();
            int Stop();
            int Reset();
            int SetEventHandle(IntPtr eventHandle);
            int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object service);
        }

        [ComImport, Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317"),
         InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAudioCaptureClient
        {
            int GetBuffer(out IntPtr data, out uint frames, out uint flags,
                          out long devicePosition, out long qpcPosition);
            int ReleaseBuffer(uint framesRead);
            int GetNextPacketSize(out uint frames);
        }
    }
}
