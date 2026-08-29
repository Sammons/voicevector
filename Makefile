# VoiceVector monorepo
.PHONY: macos macos-test windows windows-test

macos:
	$(MAKE) -C apps/macos app

macos-test:
	$(MAKE) -C apps/macos test

# Windows targets run on a Windows machine (dotnet 9 SDK, no admin needed).
windows:
	cd apps/windows && dotnet publish src/VoiceVector.App -c Release -r win-x64 -p:Platform=x64 --self-contained

# Core logic self-test runs anywhere dotnet runs (including this repo's CI/Linux).
windows-test:
	cd apps/windows && dotnet run --project src/VoiceVector.SelfTest
