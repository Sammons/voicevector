using System;
using System.IO;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using VoiceVector.Shared;

namespace VoiceVector.Win.Services
{
    /// <summary>The machine's peering identity: a P-256 key + self-signed
    /// certificate, stored as a DPAPI-protected PFX beside the config
    /// (mirrors macOS PeerCrypto identity; docs/multi-machine.md).</summary>
    public static class PeerIdentity
    {
        private static X509Certificate2 _certificate;

        // A stable thumbprint file records which cert in CurrentUser\My is
        // ours, so we install the key container exactly once (SChannel needs
        // an on-disk key for SslStream) instead of re-importing every launch.
        private static string ThumbprintPath
        {
            get { return Path.Combine(Path.GetDirectoryName(AppConfig.DefaultPath), "peer-thumbprint.txt"); }
        }

        public static X509Certificate2 Load(string machineName)
        {
            if (_certificate != null) return _certificate;
            var store = new X509Store(StoreName.My, StoreLocation.CurrentUser);
            try
            {
                store.Open(OpenFlags.ReadWrite);
                if (File.Exists(ThumbprintPath))
                {
                    var thumb = File.ReadAllText(ThumbprintPath).Trim();
                    var found = store.Certificates.Find(X509FindType.FindByThumbprint, thumb, false);
                    if (found.Count > 0 && found[0].HasPrivateKey) { _certificate = found[0]; return _certificate; }
                }
                var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
                var request = new CertificateRequest("CN=VoiceVector-" + machineName, key,
                                                     HashAlgorithmName.SHA256);
                using (var ephemeral = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1),
                                                    new DateTimeOffset(2126, 1, 1, 0, 0, 0, TimeSpan.Zero)))
                {
                    // Round-trip through PFX with a persisted key, then add to the
                    // store; the store owns the single key container from now on.
                    var pfx = ephemeral.Export(X509ContentType.Pfx);
                    var persisted = new X509Certificate2(pfx, (string)null,
                        X509KeyStorageFlags.PersistKeySet | X509KeyStorageFlags.Exportable);
                    store.Add(persisted);
                    Directory.CreateDirectory(Path.GetDirectoryName(ThumbprintPath));
                    File.WriteAllText(ThumbprintPath, persisted.Thumbprint);
                    _certificate = persisted;
                    return _certificate;
                }
            }
            catch (Exception e)
            {
                Log.Error("Peer identity: " + e.Message);
                return null;
            }
            finally
            {
                store.Close();
            }
        }

        public static byte[] Fingerprint(X509Certificate2 cert)
        {
            return PeerCrypto.Fingerprint(cert.RawData);
        }

        public static string FingerprintHex(X509Certificate2 cert)
        {
            return PeerCrypto.ToHex(Fingerprint(cert));
        }
    }
}
