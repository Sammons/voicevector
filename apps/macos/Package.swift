// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "VoiceVector",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(
            name: "VoiceVector",
            path: "Sources/VoiceVector"
        ),
    ]
)
