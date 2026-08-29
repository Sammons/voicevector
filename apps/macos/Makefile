# VoiceVector — native macOS dictation app (SwiftPM, no Xcode needed)
.PHONY: app run test clean notarize

app:
	bash scripts/make_app.sh release

run: app
	open build/VoiceVector.app

test:
	swift build
	.build/debug/VoiceVector --self-test

notarize: app
	bash scripts/notarize.sh

clean:
	rm -rf .build build
