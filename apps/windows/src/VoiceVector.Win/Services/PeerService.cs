using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Threading.Tasks;
using VoiceVector.Shared;

namespace VoiceVector.Win.Services
{
    /// <summary>One machine's routing context.</summary>
    public sealed class MachineContext
    {
        public string Machine = "";
        public bool IsLocal;
        public List<WindowInfo> Windows = new List<WindowInfo>();
        public string WindowLines = "";
        public IList<ScreenshotAttachment> Screens = new List<ScreenshotAttachment>();
    }

    /// <summary>TLS listener + client for multi-machine peering
    /// (docs/multi-machine.md). Mirrors macOS PeerService.</summary>
    public sealed class PeerService
    {
        public static readonly PeerService Shared = new PeerService();

        public Func<MultiMachineConfig> ConfigProvider;
        public Action<PeerRef> AddPeer;
        /// <summary>(text, windowId, done(ok, message)) on the UI thread.</summary>
        public Action<string, uint, Action<bool, string>> OnDeliver;
        /// <summary>(peerName, code, answer) on the UI thread.</summary>
        public Action<string, string, Action<bool>> OnIncomingPair;
        public Action<Action> RunOnUi = a => a();

        private TcpListener _listener;
        private int _listenPort = -1;
        private X509Certificate2 _certificate;
        private bool _pairingBusy;

        private MultiMachineConfig Config
        {
            get { return ConfigProvider != null ? ConfigProvider() : new MultiMachineConfig(); }
        }

        public string FingerprintHex
        {
            get { return _certificate == null ? "" : PeerIdentity.FingerprintHex(_certificate); }
        }

        // -- lifecycle -------------------------------------------------------

        public void ApplyConfig()
        {
            var mm = Config;
            if (!mm.Enabled) { StopListener(); return; }
            if (_certificate == null) _certificate = PeerIdentity.Load(mm.ResolvedMachineName);
            if (_certificate == null) return;
            if (_listener != null && _listenPort == mm.Port) return;
            StopListener();
            try
            {
                _listener = new TcpListener(IPAddress.Any, mm.Port);
                _listener.Start();
                _listenPort = mm.Port;
                var _ = AcceptLoopAsync(_listener);
            }
            catch (Exception e)
            {
                Log.Error("Peer listener: " + e.Message);
                _listener = null;
            }
        }

        private void StopListener()
        {
            try { if (_listener != null) _listener.Stop(); } catch { }
            _listener = null;
            _listenPort = -1;
        }

        private async Task AcceptLoopAsync(TcpListener listener)
        {
            while (_listener == listener)
            {
                TcpClient client;
                try { client = await listener.AcceptTcpClientAsync().ConfigureAwait(false); }
                catch { break; }
                var _ = Task.Run(() => ServeAsync(client));
            }
        }

        // -- frame plumbing over SslStream -----------------------------------

        private static async Task<Dictionary<string, object>> ReadFrameAsync(SslStream stream, List<byte> buffer)
        {
            var chunk = new byte[65536];
            while (true)
            {
                int consumed;
                var obj = PeerCrypto.ParseFrame(buffer, out consumed);
                if (consumed > 0) { buffer.RemoveRange(0, consumed); return obj; }
                if (consumed < 0) return null;
                int n;
                try { n = await stream.ReadAsync(chunk, 0, chunk.Length).ConfigureAwait(false); }
                catch { return null; }
                if (n <= 0) return null;
                for (int i = 0; i < n; i++) buffer.Add(chunk[i]);
            }
        }

        private static Task SendAsync(SslStream stream, Dictionary<string, object> obj)
        {
            var frame = PeerCrypto.Frame(obj);
            return stream.WriteAsync(frame, 0, frame.Length);
        }

        // -- server ----------------------------------------------------------

        private async Task ServeAsync(TcpClient client)
        {
            try
            {
                client.ReceiveTimeout = client.SendTimeout = 60000;
                using (var ssl = new SslStream(client.GetStream(), false, (s, cert, chain, err) => cert != null))
                {
                    await ssl.AuthenticateAsServerAsync(_certificate, true,
                        System.Security.Authentication.SslProtocols.Tls12, false).ConfigureAwait(false);
                    var remote = ssl.RemoteCertificate == null
                        ? null : PeerCrypto.Fingerprint(ssl.RemoteCertificate.GetRawCertData());
                    if (remote == null) return;
                    var buffer = new List<byte>();
                    var hello = await ReadFrameAsync(ssl, buffer).ConfigureAwait(false);
                    if (hello == null || Json.Str(hello, "t") != "hello") return;
                    var peerName = Json.Str(hello, "name", "?");
                    switch (Json.Str(hello, "purpose"))
                    {
                        case "pair": await ServePairingAsync(ssl, buffer, peerName, remote).ConfigureAwait(false); break;
                        case "peer": await ServePeerAsync(ssl, buffer, remote).ConfigureAwait(false); break;
                    }
                }
            }
            catch (Exception e)
            {
                Log.Error("Peer connection: " + e.Message);
            }
            finally
            {
                try { client.Close(); } catch { }
            }
        }

        private async Task ServePairingAsync(SslStream ssl, List<byte> buffer, string peerName, byte[] clientFp)
        {
            if (_pairingBusy || OnIncomingPair == null)
            {
                await SendAsync(ssl, new Dictionary<string, object> { { "t", "err" }, { "err", "busy" } }).ConfigureAwait(false);
                return;
            }
            _pairingBusy = true;
            try
            {
                var nonce = new byte[32];
                using (var rng = RandomNumberGenerator.Create()) rng.GetBytes(nonce);
                await SendAsync(ssl, new Dictionary<string, object>
                    { { "t", "hello" }, { "ver", 1.0 }, { "name", Config.ResolvedMachineName } }).ConfigureAwait(false);
                var commit = await ReadFrameAsync(ssl, buffer).ConfigureAwait(false);
                if (commit == null || Json.Str(commit, "t") != "commit") return;
                var theirCommit = Json.Str(commit, "h");
                await SendAsync(ssl, new Dictionary<string, object>
                    { { "t", "commit" }, { "h", PeerCrypto.Commitment(nonce) } }).ConfigureAwait(false);
                var reveal = await ReadFrameAsync(ssl, buffer).ConfigureAwait(false);
                var theirNonce = reveal == null ? null : PeerCrypto.FromHex(Json.Str(reveal, "n"));
                if (theirNonce == null || PeerCrypto.Commitment(theirNonce) != theirCommit) return;
                await SendAsync(ssl, new Dictionary<string, object>
                    { { "t", "reveal" }, { "n", PeerCrypto.ToHex(nonce) } }).ConfigureAwait(false);
                var code = PeerCrypto.PairingCode(clientFp, PeerIdentity.Fingerprint(_certificate),
                                                  theirNonce, nonce);

                var localAnswer = new TaskCompletionSource<bool>();
                RunOnUi(() => OnIncomingPair(peerName, code, ok => localAnswer.TrySetResult(ok)));
                var remoteAnswer = ReadFrameAsync(ssl, buffer);
                if (!await localAnswer.Task.ConfigureAwait(false))
                {
                    await SendAsync(ssl, new Dictionary<string, object> { { "t", "deny" } }).ConfigureAwait(false);
                    return;
                }
                var answer = await remoteAnswer.ConfigureAwait(false);
                if (answer == null || Json.Str(answer, "t") != "confirm") return;
                var peer = new PeerRef { Name = peerName, Fingerprint = PeerCrypto.ToHex(clientFp) };
                RunOnUi(() => { if (AddPeer != null) AddPeer(peer); });
                await SendAsync(ssl, new Dictionary<string, object> { { "t", "confirm" } }).ConfigureAwait(false);
            }
            finally
            {
                _pairingBusy = false;
            }
        }

        private async Task ServePeerAsync(SslStream ssl, List<byte> buffer, byte[] remoteFp)
        {
            var fingerprint = PeerCrypto.ToHex(remoteFp);
            var peer = Config.Peers.FirstOrDefault(p => p.Fingerprint == fingerprint);
            if (peer == null)
            {
                await SendAsync(ssl, new Dictionary<string, object> { { "t", "err" }, { "err", "untrusted" } }).ConfigureAwait(false);
                return;
            }
            await SendAsync(ssl, new Dictionary<string, object>
                { { "t", "hello" }, { "ver", 1.0 }, { "name", Config.ResolvedMachineName } }).ConfigureAwait(false);
            var request = await ReadFrameAsync(ssl, buffer).ConfigureAwait(false);
            if (request == null) return;
            switch (Json.Str(request, "t"))
            {
                case "context":
                {
                    if (!peer.AllowScreens)
                    {
                        await SendAsync(ssl, new Dictionary<string, object> { { "t", "err" }, { "err", "screens not allowed" } }).ConfigureAwait(false);
                        return;
                    }
                    var context = LocalContext(Config.ResolvedMachineName, null, null);
                    var screens = context.Screens.Select(s => (object)new Dictionary<string, object>
                        { { "jpeg", Convert.ToBase64String(s.Jpeg) }, { "caption", s.Caption } }).ToList();
                    var windows = context.Windows.Select(w => (object)new Dictionary<string, object>
                        { { "id", (double)w.Id }, { "app", w.App }, { "title", w.Title } }).ToList();
                    await SendAsync(ssl, new Dictionary<string, object>
                        { { "t", "context" }, { "machine", context.Machine },
                          { "screens", screens }, { "windows", windows } }).ConfigureAwait(false);
                    break;
                }
                case "deliver":
                {
                    if (!peer.AllowDeliver)
                    {
                        await SendAsync(ssl, new Dictionary<string, object> { { "t", "err" }, { "err", "deliver not allowed" } }).ConfigureAwait(false);
                        return;
                    }
                    var text = Json.Str(request, "text");
                    var window = (uint)Json.Num(request, "window", 0);
                    if (text.Length == 0 || OnDeliver == null)
                    {
                        await SendAsync(ssl, new Dictionary<string, object> { { "t", "err" }, { "err", "empty" } }).ConfigureAwait(false);
                        return;
                    }
                    var done = new TaskCompletionSource<Tuple<bool, string>>();
                    RunOnUi(() => OnDeliver(text, window, (ok, message) => done.TrySetResult(Tuple.Create(ok, message))));
                    var result = await done.Task.ConfigureAwait(false);
                    await SendAsync(ssl, result.Item1
                        ? new Dictionary<string, object> { { "t", "ok" } }
                        : new Dictionary<string, object> { { "t", "err" }, { "err", result.Item2 } }).ConfigureAwait(false);
                    break;
                }
            }
        }

        // -- client ----------------------------------------------------------

        private async Task<Tuple<TcpClient, SslStream, List<byte>, string, string>> OpenSessionAsync(
            string address, string purpose)
        {
            var mm = Config;
            if (_certificate == null) _certificate = PeerIdentity.Load(mm.ResolvedMachineName);
            if (_certificate == null) throw new InvalidOperationException("no identity");
            var host = address;
            var port = mm.Port;
            var colon = address.LastIndexOf(':');
            if (colon > 0 && address.IndexOf(':') == colon)
            {
                host = address.Substring(0, colon);
                int parsed;
                if (int.TryParse(address.Substring(colon + 1), out parsed)) port = parsed;
            }
            var client = new TcpClient();
            try
            {
                var connect = client.ConnectAsync(host, port);
                if (await Task.WhenAny(connect, Task.Delay(8000)).ConfigureAwait(false) != connect)
                    throw new TimeoutException("timed out");
                await connect.ConfigureAwait(false);
                var ssl = new SslStream(client.GetStream(), false, (s, cert, chain, err) => cert != null,
                                        (s, h, certs, remote, issuers) => _certificate);
                await ssl.AuthenticateAsClientAsync(host, new X509CertificateCollection { _certificate },
                    System.Security.Authentication.SslProtocols.Tls12, false).ConfigureAwait(false);
                var serverFp = ssl.RemoteCertificate == null
                    ? "" : PeerCrypto.ToHex(PeerCrypto.Fingerprint(ssl.RemoteCertificate.GetRawCertData()));
                var buffer = new List<byte>();
                await SendAsync(ssl, new Dictionary<string, object>
                    { { "t", "hello" }, { "ver", 1.0 }, { "name", mm.ResolvedMachineName },
                      { "purpose", purpose } }).ConfigureAwait(false);
                var hello = await ReadFrameAsync(ssl, buffer).ConfigureAwait(false);
                if (hello == null || Json.Str(hello, "t") != "hello")
                    throw new InvalidOperationException(hello != null ? Json.Str(hello, "err", "handshake failed") : "handshake failed");
                return Tuple.Create(client, ssl, buffer, Json.Str(hello, "name", "?"), serverFp);
            }
            catch
            {
                try { client.Close(); } catch { }
                throw;
            }
        }

        /// <summary>Outbound pairing; onCode fires on the UI thread with the
        /// number and an answer callback; the task completes with the peer.</summary>
        public async Task<PeerRef> PairAsync(string address, Action<string, Action<bool>> onCode)
        {
            var session = await OpenSessionAsync(address, "pair").ConfigureAwait(false);
            try
            {
                var ssl = session.Item2; var buffer = session.Item3;
                var nonce = new byte[32];
                using (var rng = RandomNumberGenerator.Create()) rng.GetBytes(nonce);
                await SendAsync(ssl, new Dictionary<string, object>
                    { { "t", "commit" }, { "h", PeerCrypto.Commitment(nonce) } }).ConfigureAwait(false);
                var commit = await ReadFrameAsync(ssl, buffer).ConfigureAwait(false);
                if (commit == null || Json.Str(commit, "t") != "commit")
                    throw new InvalidOperationException("pairing refused");
                var theirCommit = Json.Str(commit, "h");
                await SendAsync(ssl, new Dictionary<string, object>
                    { { "t", "reveal" }, { "n", PeerCrypto.ToHex(nonce) } }).ConfigureAwait(false);
                var reveal = await ReadFrameAsync(ssl, buffer).ConfigureAwait(false);
                var theirNonce = reveal == null ? null : PeerCrypto.FromHex(Json.Str(reveal, "n"));
                if (theirNonce == null || PeerCrypto.Commitment(theirNonce) != theirCommit)
                    throw new InvalidOperationException("pairing failed");
                var serverFp = PeerCrypto.FromHex(session.Item5);
                var code = PeerCrypto.PairingCode(PeerIdentity.Fingerprint(_certificate), serverFp,
                                                  nonce, theirNonce);
                var answer = new TaskCompletionSource<bool>();
                RunOnUi(() => onCode(code, ok => answer.TrySetResult(ok)));
                if (!await answer.Task.ConfigureAwait(false))
                {
                    await SendAsync(ssl, new Dictionary<string, object> { { "t", "deny" } }).ConfigureAwait(false);
                    throw new OperationCanceledException("cancelled");
                }
                await SendAsync(ssl, new Dictionary<string, object> { { "t", "confirm" } }).ConfigureAwait(false);
                var confirmed = await ReadFrameAsync(ssl, buffer).ConfigureAwait(false);
                if (confirmed == null || Json.Str(confirmed, "t") != "confirm")
                    throw new InvalidOperationException("the other machine denied the pairing");
                return new PeerRef { Name = session.Item4, Fingerprint = session.Item5, Address = address };
            }
            finally
            {
                try { session.Item1.Close(); } catch { }
            }
        }

        /// <summary>One peer's context, or null on any failure.</summary>
        public async Task<MachineContext> FetchContextAsync(PeerRef peer)
        {
            if (peer.Address.Length == 0) return null;
            try
            {
                var session = await OpenSessionAsync(peer.Address, "peer").ConfigureAwait(false);
                try
                {
                    if (session.Item5 != peer.Fingerprint) return null;
                    await SendAsync(session.Item2, new Dictionary<string, object> { { "t", "context" } }).ConfigureAwait(false);
                    var response = await ReadFrameAsync(session.Item2, session.Item3).ConfigureAwait(false);
                    if (response == null || Json.Str(response, "t") != "context") return null;
                    var machine = Json.Str(response, "machine", peer.Name);
                    var context = new MachineContext { Machine = machine, IsLocal = false };
                    foreach (var item in Json.Arr(response, "screens") ?? new List<object>())
                        if (item is Dictionary<string, object> s)
                            context.Screens.Add(new ScreenshotAttachment
                            {
                                Jpeg = Convert.FromBase64String(Json.Str(s, "jpeg")),
                                Caption = "Machine \"" + machine + "\" — " + Json.Str(s, "caption"),
                            });
                    foreach (var item in Json.Arr(response, "windows") ?? new List<object>())
                        if (item is Dictionary<string, object> w)
                            context.Windows.Add(new WindowInfo
                            {
                                Id = (uint)Json.Num(w, "id", 0),
                                App = Json.Str(w, "app", "?"),
                                Title = Json.Str(w, "title"),
                            });
                    context.WindowLines = WindowInventory.Describe(context.Windows);
                    return context;
                }
                finally { try { session.Item1.Close(); } catch { } }
            }
            catch (Exception e)
            {
                Log.Error("Peer context from " + peer.Name + ": " + e.Message);
                return null;
            }
        }

        /// <summary>Sends routed text to a peer; returns null or an error message.</summary>
        public async Task<string> DeliverAsync(string text, uint window, PeerRef peer)
        {
            if (peer.Address.Length == 0) return "peer has no address";
            try
            {
                var session = await OpenSessionAsync(peer.Address, "peer").ConfigureAwait(false);
                try
                {
                    if (session.Item5 != peer.Fingerprint) return "could not reach " + peer.Name;
                    await SendAsync(session.Item2, new Dictionary<string, object>
                        { { "t", "deliver" }, { "text", text }, { "window", (double)window } }).ConfigureAwait(false);
                    var response = await ReadFrameAsync(session.Item2, session.Item3).ConfigureAwait(false);
                    if (response != null && Json.Str(response, "t") == "ok") return null;
                    return response != null ? Json.Str(response, "err", "delivery failed") : "delivery failed";
                }
                finally { try { session.Item1.Close(); } catch { } }
            }
            catch (Exception e)
            {
                return e.Message;
            }
        }

        /// <summary>This machine's context, captioned for the router.</summary>
        public static MachineContext LocalContext(string machineName, List<WindowInfo> windows,
                                                  ScreenshotSet screens)
        {
            var list = windows ?? WindowInventory.List();
            var set = screens ?? ScreenCapture.AllScreens();
            var attachments = new List<ScreenshotAttachment>();
            if (set != null)
                foreach (var shot in set.Attachments())
                    attachments.Add(new ScreenshotAttachment
                    {
                        Jpeg = shot.Jpeg,
                        Caption = "Machine \"" + machineName + "\" — " + shot.Caption,
                    });
            return new MachineContext
            {
                Machine = machineName, IsLocal = true, Windows = list,
                WindowLines = WindowInventory.Describe(list), Screens = attachments,
            };
        }
    }
}
