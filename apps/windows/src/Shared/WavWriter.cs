using System;
using System.IO;

namespace VoiceVector.Shared
{
    /// <summary>
    /// Streaming RIFF WAV (16-bit PCM) writer; header sizes patched on
    /// finalize. Also generates in-memory WAV (silent test clips, chimes) and
    /// byte-range slices for streamed segment transcription.
    /// </summary>
    public sealed class WavWriter : IDisposable
    {
        public const int TargetSampleRate = 16000;

        private readonly FileStream _stream;
        private readonly int _sampleRate;
        private readonly int _channels;
        private uint _dataBytes;

        public uint DataBytes { get { return _dataBytes; } }
        public string Path { get; private set; }

        public WavWriter(string path, int sampleRate = TargetSampleRate, int channels = 1)
        {
            Path = path;
            _sampleRate = sampleRate;
            _channels = channels;
            _stream = new FileStream(path, FileMode.Create, FileAccess.Write);
            var header = Header(sampleRate, channels, 0);
            _stream.Write(header, 0, header.Length);
        }

        public void Append(short[] samples, int count)
        {
            var bytes = new byte[count * 2];
            Buffer.BlockCopy(samples, 0, bytes, 0, count * 2);
            _stream.Write(bytes, 0, bytes.Length);
            _dataBytes += (uint)bytes.Length;
        }

        /// <summary>Patches sizes and closes; returns duration in seconds.</summary>
        public double FinalizeFile()
        {
            _stream.Seek(0, SeekOrigin.Begin);
            var header = Header(_sampleRate, _channels, _dataBytes);
            _stream.Write(header, 0, header.Length);
            _stream.Dispose();
            return _dataBytes / (double)(2 * _channels * _sampleRate);
        }

        public void Dispose()
        {
            _stream.Dispose();
        }

        /// <summary>Standalone WAV from a byte range of a (possibly growing) recording.</summary>
        public static byte[] SliceWav(string path, uint fromByte, uint toByte,
                                      int sampleRate = TargetSampleRate)
        {
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                stream.Seek(44 + fromByte, SeekOrigin.Begin);
                var payload = new byte[toByte - fromByte];
                int read = stream.Read(payload, 0, payload.Length);
                if (read < payload.Length) Array.Resize(ref payload, read);
                var wav = new byte[44 + payload.Length];
                Header(sampleRate, 1, (uint)payload.Length).CopyTo(wav, 0);
                payload.CopyTo(wav, 44);
                return wav;
            }
        }

        public static byte[] SilentWav(double seconds, int sampleRate = TargetSampleRate)
        {
            uint dataBytes = (uint)(sampleRate * seconds) * 2;
            var wav = new byte[44 + dataBytes];
            Header(sampleRate, 1, dataBytes).CopyTo(wav, 0);
            return wav;
        }

        /// <summary>Two overlapping decaying sine notes — the start/stop chimes.</summary>
        public static byte[] ChimeWav(double[][] notes, int sampleRate = 44100)
        {
            const double duration = 0.45;
            int frames = (int)(duration * sampleRate);
            var wav = new byte[44 + frames * 2];
            Header(sampleRate, 1, (uint)(frames * 2)).CopyTo(wav, 0);
            for (int i = 0; i < frames; i++)
            {
                double t = i / (double)sampleRate;
                double value = 0;
                foreach (var note in notes)
                {
                    double freq = note[0], onset = note[1];
                    if (t < onset) continue;
                    double local = t - onset;
                    double attack = Math.Min(local / 0.008, 1.0);
                    double decay = Math.Exp(-local * 9.0);
                    value += Math.Sin(2 * Math.PI * freq * local) * attack * decay * 0.35;
                }
                short sample = (short)(Math.Max(-1.0, Math.Min(1.0, value)) * short.MaxValue * 0.5);
                wav[44 + i * 2] = (byte)(sample & 0xFF);
                wav[44 + i * 2 + 1] = (byte)((sample >> 8) & 0xFF);
            }
            return wav;
        }

        public static byte[] StartChime()
        {
            return ChimeWav(new[] { new[] { 660.0, 0.0 }, new[] { 990.0, 0.09 } });
        }

        public static byte[] StopChime()
        {
            return ChimeWav(new[] { new[] { 990.0, 0.0 }, new[] { 660.0, 0.09 } });
        }

        public static byte[] ErrorChime()
        {
            return ChimeWav(new[] { new[] { 440.0, 0.0 }, new[] { 330.0, 0.12 } });
        }

        private static byte[] Header(int sampleRate, int channels, uint dataBytes)
        {
            var data = new byte[44];
            using (var writer = new BinaryWriter(new MemoryStream(data)))
            {
                writer.Write(new[] { (byte)'R', (byte)'I', (byte)'F', (byte)'F' });
                writer.Write(36 + dataBytes);
                writer.Write(new[] { (byte)'W', (byte)'A', (byte)'V', (byte)'E' });
                writer.Write(new[] { (byte)'f', (byte)'m', (byte)'t', (byte)' ' });
                writer.Write(16);
                writer.Write((ushort)1); // PCM
                writer.Write((ushort)channels);
                writer.Write((uint)sampleRate);
                writer.Write((uint)(sampleRate * channels * 2));
                writer.Write((ushort)(channels * 2));
                writer.Write((ushort)16);
                writer.Write(new[] { (byte)'d', (byte)'a', (byte)'t', (byte)'a' });
                writer.Write(dataBytes);
            }
            return data;
        }
    }
}
