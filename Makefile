# VoiceVector monorepo
.PHONY: macos macos-test windows windows-test

macos:
	$(MAKE) -C apps/macos app

macos-test:
	$(MAKE) -C apps/macos test

# Windows targets run on a Windows machine (dotnet 9 SDK, no admin needed).
windows:
	cd apps/windows && dotnet build src/VoiceVector.Win -c Release

# Compile-check the WPF app from any host (Docker or CI Linux). Reference
# assemblies only — no Windows required.
windows-compile-check:
	docker compose -f compose.dotnet.yml run --rm dotnet sh -c \
	  'dotnet build apps/windows/src/VoiceVector.Win -c Release -p:EnableWindowsTargeting=true'

# Core logic self-test runs anywhere dotnet runs (including this repo's CI/Linux).
windows-test:
	cd apps/windows && dotnet run --project src/VoiceVector.SelfTest
