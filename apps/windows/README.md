# VoiceVector for Windows

C# + **WPF on .NET Framework 4.8** — the framework that ships inside
Windows 10/11. The result is a **single ~2 MB `VoiceVector.exe`** with zero
prerequisites: no runtime installs, no installer, no admin, ever.
Fluent-by-hand UI from OS built-ins (Segoe UI Variable, Segoe Fluent Icons,
DWM dark title bars / Mica / rounded corners on Win11).

## Build (any machine with the .NET SDK — even Linux/macOS compile-checks)

```powershell
msbuild apps\windows\src\VoiceVector.Win\VoiceVector.Win.csproj /restore /t:Build /p:Configuration=Release
# → apps\windows\src\VoiceVector.Win\bin\Release\net48\VoiceVector.exe
```

Tests (run anywhere dotnet runs — the same Shared/ sources the exe ships):

```sh
dotnet run --project src/VoiceVector.SelfTest
```

## Layout

- `src/Shared/` — portable core (config, state machine, providers, markdown
  library, WAV, hand-rolled JSON — old-framework .NET has none built in),
  compiled directly into the exe and into the self-test.
- `src/VoiceVector.Win/` — the WPF app (code-built UI, no XAML compiler),
  WASAPI capture via COM interop, low-level keyboard hook, clipboard
  snapshot/restore paste, DPAPI key storage, tray, one-click updates.
- `src/VoiceVector.E2E/` — CI driver: mock provider + synthetic hotkey +
  paste assertion against the real exe.

## First run

- If recording fails, allow microphone for desktop apps: Settings → Privacy &
  security → Microphone.
- Wizard sets up providers and the hotkey (default **Right Alt**;
  hold-to-talk always works, double-tap latches, Esc cancels).
- Closing the window hides to the tray; quit from the tray menu.

## Providers on Windows

- **Azure AI Foundry** (cloud, API key): OpenAI-compatible provider with base
  URL `https://<resource>.openai.azure.com/openai/v1`, model = deployment name.
- **Foundry Local** (on-device, free): `winget install Microsoft.FoundryLocal`
  (per-user), point an OpenAI-compatible provider at its localhost endpoint.
- ElevenLabs / Vercel AI Gateway / Fireworks / Cerebras work exactly as on
  macOS. One-click updates live in Settings.

## Userland caveats

- A non-elevated app cannot dictate into elevated (admin) windows (UIPI).
- On layouts where Right Alt is AltGr, pick a different hotkey.
