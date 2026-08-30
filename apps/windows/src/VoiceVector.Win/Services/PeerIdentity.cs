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

        private static string PfxPath
        {
            get { return Path.Combine(Path.GetDirectoryName(AppConfig.DefaultPath), "peer-identity.bin"); }
        }

        public static X509Certificate2 Load(string machineName)
        {
            if (_certificate != null) return _certificate;
            try
            {
                if (File.Exists(PfxPath))
                {
                    var pfx = ProtectedData.Unprotect(File.ReadAllBytes(PfxPath), null,
                                                      DataProtectionScope.CurrentUser);
                    var cert = new X509Certificate2(pfx, (string)null,
                        X509KeyStorageFlags.Exportable | X509KeyStorageFlags.PersistKeySet);
                    if (cert.HasPrivateKey) { _certificate = cert; return cert; }
                }
            }
            catch (Exception e)
            {
                Log.Error("Peer identity load failed, recreating: " + e.Message);
            }
            try
            {
                var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
                var request = new CertificateRequest("CN=VoiceVector-" + machineName, key,
                                                     HashAlgorithmName.SHA256);
                var cert = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1),
                                                    new DateTimeOffset(2126, 1, 1, 0, 0, 0, TimeSpan.Zero));
                Directory.CreateDirectory(Path.GetDirectoryName(PfxPath));
                var pfx = cert.Export(X509ContentType.Pfx);
                File.WriteAllBytes(PfxPath, ProtectedData.Protect(pfx, null,
                                                                  DataProtectionScope.CurrentUser));
                // Re-import so SslStream can use the private key.
                _certificate = new X509Certificate2(pfx, (string)null,
                    X509KeyStorageFlags.Exportable | X509KeyStorageFlags.PersistKeySet);
                return _certificate;
            }
            catch (Exception e)
            {
                Log.Error("Peer identity creation failed: " + e.Message);
                return null;
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
