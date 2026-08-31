# Multi-machine peering (VVP/1)

Optional feature: VoiceVector instances on the same private network (e.g. a
tailnet) pair with each other so that dictation on one machine can see the
others' screens and, with the AI router, deliver text to a window on any
paired machine. There is no central authority: trust is established pairwise
with a both-sides confirmation code, then pinned.

## Identity

Each instance generates, once, a P-256 key pair and a self-signed X.509
certificate (CN `VoiceVector-<machine name>`, 100-year validity) the first
time multi-machine is enabled. The private key lives in the platform secret
store (Keychain / DPAPI-protected file / Secret Service); the certificate is
the machine's identity. Its **fingerprint** is the lowercase-hex SHA-256 of
the certificate's DER bytes.

## Transport

TCP, default port **47800**, wrapped in TLS (1.2+) using the self-signed
certificates on both sides — the server requires a client certificate.
Certificate validation is fingerprint pinning, not chains: outside of
pairing, each side accepts exactly the fingerprints it has stored as peers.

On top of TLS, messages are frames: **4-byte big-endian length, then UTF-8
JSON**, one object per frame, 32 MB max. Every message has a string field
`t` (type). Unknown fields are ignored.

## Pairing (numeric comparison)

The one moment self-signed certs are accepted from strangers. Both sides
display the same 6-digit code; the user confirms on both machines; a
man-in-the-middle causes the two screens to show different codes.

Client connects with `{"t":"hello","ver":1,"name":"<machine>","purpose":"pair"}`;
server answers `{"t":"hello","ver":1,"name":"<machine>"}` (or
`{"t":"err","err":"busy"}` if a pairing is already in progress). Then a
commit-then-reveal exchange of 32-byte random nonces (hex in JSON):

1. client → `{"t":"commit","h":hex(SHA256(nonceC))}`
2. server → `{"t":"commit","h":hex(SHA256(nonceS))}`
3. client → `{"t":"reveal","n":hex(nonceC)}`
4. server → `{"t":"reveal","n":hex(nonceS)}`

Each side verifies the other's reveal against its commitment, then computes

    code = BE-uint32(SHA256(fpClient ‖ fpServer ‖ nonceC ‖ nonceS)[0..3]) mod 1e6

where `fpClient`/`fpServer` are the raw 32-byte certificate digests. The
commitment makes the code ungrindable: both nonces are fixed before either
is known. Zero-pad to six digits, display as `123 456`.

The user confirms on both machines; each side then sends `{"t":"confirm"}`
(or `{"t":"deny"}`). When a side has both confirmed locally and received
`confirm`, it stores the peer: name, fingerprint, address (the initiator
stores the address it dialed; the receiving side stores the peer without an
address until the user fills one in), and per-peer permissions, both off by
default: **allowScreens** (this peer may fetch my screens/windows) and
**allowDeliver** (this peer may paste text into me).

Test vector (all three self-tests assert it): fpC = SHA256("client-cert"),
fpS = SHA256("server-cert"), nonceC = 32×0x01, nonceS = 32×0x02 →
code **636241**.

## Peer session

`{"t":"hello","ver":1,"name":"<machine>","purpose":"peer"}` — the server
matches the TLS client certificate fingerprint against its stored peers and
answers `hello`, or `{"t":"err","err":"untrusted"}` and closes. The client
likewise drops the connection unless the server certificate matches the
peer it dialed. One request/response per connection; requests:

- `{"t":"context"}` → `{"t":"context","machine":"<name>","screens":
  [{"jpeg":"<base64>","caption":"…"}],"windows":[{"id":N,"app":"…","title":"…"}]}`.
  Requires **allowScreens**. Screens are the standard captioned per-display
  JPEGs; `windows` is empty on platforms that cannot enumerate windows
  (Linux/Wayland).
- `{"t":"deliver","text":"…","window":N,"submit":false}` → `{"t":"ok"}` or
  `{"t":"err","err":"…"}`. Requires **allowDeliver**. The peer activates
  window `N` first when it can (`0` or unknown id = paste into the current
  focus), then runs its normal paste path; when `submit` is true it presses
  Enter afterward (auto-submit). The text is also saved to the peer's library
  as a routed entry.

## AI routing

Per hotkey, only meaningful with **Review before pasting** on. At routing
time the initiating machine gathers its own context plus `context` from
every peer with an address, sends the draft + all window lists + all
screenshots to the router model (`shared/prompts/router.txt`), and expects
strict JSON `{"machine":"<name>","window":<id>}`. The verdict is shown in
the staging card ("→ Slack on crankshaft"); ⏎ delivers there. Any error,
timeout, or unknown machine/window falls back to the normal local paste.
