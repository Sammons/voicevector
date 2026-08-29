// Draws the VoiceVector app icon with CoreGraphics and emits an .iconset
// directory — no binary assets in the repo. Run by scripts/make_app.sh:
//   swift scripts/make_icon.swift <output.iconset>
import Foundation
import AppKit

let sizes: [(Int, Int, String)] = [
    (16, 1, "icon_16x16"), (16, 2, "icon_16x16@2x"),
    (32, 1, "icon_32x32"), (32, 2, "icon_32x32@2x"),
    (128, 1, "icon_128x128"), (128, 2, "icon_128x128@2x"),
    (256, 1, "icon_256x256"), (256, 2, "icon_256x256@2x"),
    (512, 1, "icon_512x512"), (512, 2, "icon_512x512@2x"),
]

func draw(pixels: Int) -> NSBitmapImageRep {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
                               bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                               isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let context = NSGraphicsContext.current!.cgContext
    let size = CGFloat(pixels)

    // Rounded-square background, violet gradient.
    let inset = size * 0.05
    let rect = CGRect(x: inset, y: inset, width: size - inset * 2, height: size - inset * 2)
    let path = CGPath(roundedRect: rect, cornerWidth: size * 0.21, cornerHeight: size * 0.21, transform: nil)
    context.addPath(path)
    context.clip()
    let colors = [
        CGColor(red: 0.36, green: 0.24, blue: 0.86, alpha: 1),
        CGColor(red: 0.55, green: 0.40, blue: 1.00, alpha: 1),
    ] as CFArray
    let gradient = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: colors,
                              locations: [0, 1])!
    context.drawLinearGradient(gradient,
                               start: CGPoint(x: rect.minX, y: rect.maxY),
                               end: CGPoint(x: rect.maxX, y: rect.minY), options: [])

    // Waveform bars dipping into a "V" in the middle.
    let heights: [CGFloat] = [0.34, 0.52, 0.40, 0.62, 0.30, 0.20, 0.30, 0.62, 0.40, 0.52, 0.34]
    let barWidth = rect.width * 0.045
    let gap = (rect.width * 0.72 - barWidth * CGFloat(heights.count)) / CGFloat(heights.count - 1)
    var x = rect.minX + rect.width * 0.14
    context.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 0.96))
    for height in heights {
        let barHeight = rect.height * height
        let bar = CGRect(x: x, y: rect.midY - barHeight / 2, width: barWidth, height: barHeight)
        let rounded = CGPath(roundedRect: bar, cornerWidth: barWidth / 2,
                             cornerHeight: barWidth / 2, transform: nil)
        context.addPath(rounded)
        context.fillPath()
        x += barWidth + gap
    }

    NSGraphicsContext.restoreGraphicsState()
    return rep
}

guard CommandLine.arguments.count == 2 else {
    FileHandle.standardError.write(Data("usage: make_icon.swift <output.iconset>\n".utf8))
    exit(2)
}
let outputDir = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try? FileManager.default.removeItem(at: outputDir)
try FileManager.default.createDirectory(at: outputDir, withIntermediateDirectories: true)

for (points, scale, name) in sizes {
    let rep = draw(pixels: points * scale)
    guard let png = rep.representation(using: .png, properties: [:]) else { exit(1) }
    try png.write(to: outputDir.appendingPathComponent("\(name).png"))
}
print("iconset written to \(outputDir.path)")
