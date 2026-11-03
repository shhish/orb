// Labeled Time review sheets from deterministic simulator PNGs. CoreGraphics
// keeps this command headless; it needs no Python imaging package or AppKit.
// swift -module-cache-path /tmp/orb-time-swift-cache sim/time_contact.swift OUTDIR ...
import CoreGraphics
import CoreText
import ImageIO
import UniformTypeIdentifiers
import Foundation

let stills = [
    "timer-idle", "timer-minute", "timer-second", "timer-underhour-boundary", "timer-rim-hours", "timer-rim-reverse",
    "timer-hour", "timer-longduration", "timer-hour-running", "timer-hour-crossed",
    "timer-running", "timer-paused", "timer-finished", "timer-ring-quarter",
    "stopwatch-idle", "stopwatch-running", "stopwatch-paused", "stopwatch-underhour-boundary",
    "stopwatch-hour-running", "stopwatch-max", "stopwatch-laps",
    "stopwatch-laps-scroll", "stopwatch-laps-button", "stopwatch-laps-rim", "stopwatch-laps-extremes", "stopwatch-laps-live", "stopwatch-laps-long",
    "alarm-idle", "alarm-hour", "alarm-minute", "alarm-enabled", "alarm-ringing",
    "alarm-waiting-for-time", "alarm-time-arrived", "timer-finish-on-stopwatch",
    "alarm-ringing-on-timer", "timer-background-finished",
]

func contact(_ paths: [URL], output: URL, columns: Int, width: Int) throws {
    let cellHeight = width + 32
    let rows = (paths.count + columns - 1) / columns
    let pixelsWide = columns * width
    let pixelsHigh = rows * cellHeight
    let context = CGContext(data: nil, width: pixelsWide, height: pixelsHigh,
        bitsPerComponent: 8, bytesPerRow: pixelsWide * 4, space: CGColorSpaceCreateDeviceRGB(),
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    context.setFillColor(CGColor(gray: 0.09, alpha: 1))
    context.fill(CGRect(x: 0, y: 0, width: pixelsWide, height: pixelsHigh))
    let font = CTFontCreateWithName("Menlo" as CFString, 11, nil)
    let attrs: [NSAttributedString.Key: Any] = [
        NSAttributedString.Key(kCTFontAttributeName as String): font,
        NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor(gray: 1, alpha: 1),
    ]
    for (index, path) in paths.enumerated() {
        guard let source = CGImageSourceCreateWithURL(path as CFURL, nil),
            let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { continue }
        let x = (index % columns) * width
        let y = (rows - 1 - index / columns) * cellHeight
        context.draw(image, in: CGRect(x: x, y: y + 32, width: width, height: width))
        let name = path.deletingPathExtension().lastPathComponent
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: name, attributes: attrs))
        let lineWidth = CTLineGetTypographicBounds(line, nil, nil, nil)
        context.textPosition = CGPoint(x: CGFloat(x) + (CGFloat(width) - lineWidth) / 2, y: CGFloat(y + 10))
        CTLineDraw(line, context)
    }
    guard let image = context.makeImage(),
        let destination = CGImageDestinationCreateWithURL(output as CFURL, UTType.jpeg.identifier as CFString, 1, nil) else {
        throw NSError(domain: "TimeContact", code: 1)
    }
    CGImageDestinationAddImage(destination, image, [kCGImageDestinationLossyCompressionQuality: 0.92] as CFDictionary)
    guard CGImageDestinationFinalize(destination) else { throw NSError(domain: "TimeContact", code: 2) }
    print(output.path)
}

for argument in CommandLine.arguments.dropFirst() {
    let directory = URL(fileURLWithPath: argument, isDirectory: true)
    let paths = stills.map { directory.appendingPathComponent($0 + ".png") }
        .filter { FileManager.default.fileExists(atPath: $0.path) }
    if !paths.isEmpty { try contact(paths, output: directory.appendingPathComponent("contact.jpg"), columns: 5, width: 250) }
    let all = try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)
    let shell = all.filter { $0.lastPathComponent.range(of: "^[0-9]+.*\\.png$", options: .regularExpression) != nil }
        .sorted { $0.lastPathComponent < $1.lastPathComponent }
    if !shell.isEmpty { try contact(shell, output: directory.appendingPathComponent("contact.jpg"), columns: 4, width: 300) }
    for sequence in ["start", "pause", "finish", "lap", "mode", "rim", "laps-scroll", "page-cancel", "page-regrab", "launch"] {
        let pattern = "^motion-" + sequence + "-[0-9]+\\.png$"
        let frames = all.filter { $0.lastPathComponent.range(of: pattern, options: .regularExpression) != nil }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
        if !frames.isEmpty { try contact(frames, output: directory.appendingPathComponent("motion-" + sequence + "-contact.jpg"), columns: min(6, frames.count), width: 233) }
    }
}
