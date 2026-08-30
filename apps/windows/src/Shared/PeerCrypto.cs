using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;

namespace VoiceVector.Shared
{
    /// <summary>Pure logic for multi-machine peering (docs/multi-machine.md):
    /// frame codec and the pairing code. Mirrors macOS PeerCrypto; sockets and
    /// the identity certificate live in the Windows app.</summary>
    public static class PeerCrypto
    {
        public const int MaxFrame = 32 * 1024 * 1024;

        // -- frames: 4-byte big-endian length + UTF-8 JSON --------------------

        public static byte[] Frame(Dictionary<string, object> obj)
        {
            var body = Encoding.UTF8.GetBytes(Json.Write(obj));
            var frame = new byte[4 + body.Length];
            frame[0] = (byte)(body.Length >> 24);
            frame[1] = (byte)(body.Length >> 16);
            frame[2] = (byte)(body.Length >> 8);
            frame[3] = (byte)body.Length;
            Buffer.BlockCopy(body, 0, frame, 4, body.Length);
            return frame;
        }

        /// <summary>Parses one complete frame from the front of the buffer;
        /// consumed = 0 while incomplete, -1 on a bad frame.</summary>
        public static Dictionary<string, object> ParseFrame(List<byte> buffer, out int consumed)
        {
            consumed = 0;
            if (buffer.Count < 4) return null;
            int length = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
            if (length < 0 || length > MaxFrame) { consumed = -1; return null; }
            if (buffer.Count < 4 + length) return null;
            var body = Encoding.UTF8.GetString(buffer.ToArray(), 4, length);
            consumed = 4 + length;
            var obj = Json.Parse(body) as Dictionary<string, object>;
            if (obj == null) { consumed = -1; return null; }
            return obj;
        }

        // -- pairing code (numeric comparison) -------------------------------

        /// <summary>6-digit code over both certificate digests and both
        /// revealed nonces. Test vector: docs/multi-machine.md.</summary>
        public static string PairingCode(byte[] fpClient, byte[] fpServer,
                                         byte[] nonceClient, byte[] nonceServer)
        {
            var input = new byte[fpClient.Length + fpServer.Length + nonceClient.Length + nonceServer.Length];
            int at = 0;
            Buffer.BlockCopy(fpClient, 0, input, at, fpClient.Length); at += fpClient.Length;
            Buffer.BlockCopy(fpServer, 0, input, at, fpServer.Length); at += fpServer.Length;
            Buffer.BlockCopy(nonceClient, 0, input, at, nonceClient.Length); at += nonceClient.Length;
            Buffer.BlockCopy(nonceServer, 0, input, at, nonceServer.Length);
            byte[] digest;
            using (var sha = SHA256.Create()) digest = sha.ComputeHash(input);
            uint value = (uint)((digest[0] << 24) | (digest[1] << 16) | (digest[2] << 8) | digest[3]);
            return (value % 1000000).ToString("D6");
        }

        public static string Commitment(byte[] nonce)
        {
            using (var sha = SHA256.Create()) return ToHex(sha.ComputeHash(nonce));
        }

        public static byte[] Fingerprint(byte[] certificateDer)
        {
            using (var sha = SHA256.Create()) return sha.ComputeHash(certificateDer);
        }

        public static string ToHex(byte[] bytes)
        {
            var sb = new StringBuilder(bytes.Length * 2);
            foreach (var b in bytes) sb.Append(b.ToString("x2"));
            return sb.ToString();
        }

        public static byte[] FromHex(string hex)
        {
            if (hex == null || hex.Length % 2 != 0) return null;
            var bytes = new byte[hex.Length / 2];
            for (int i = 0; i < bytes.Length; i++)
            {
                int hi = HexDigit(hex[i * 2]), lo = HexDigit(hex[i * 2 + 1]);
                if (hi < 0 || lo < 0) return null;
                bytes[i] = (byte)((hi << 4) | lo);
            }
            return bytes;
        }

        private static int HexDigit(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }
    }
}
