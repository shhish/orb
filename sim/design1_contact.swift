// Lossless, headless pairs from the approved pixels and actual SDL screenshots.
import CoreGraphics
import CoreText
import ImageIO
import UniformTypeIdentifiers
import Foundation

let screens = ["Face", "FaceWaiting", "AlwaysOn", "Stack", "Notifications", "NotificationDetail", "Offline", "Charging", "Apps", "ControlCenter", "Compass", "PowerSheet", "Settings", "Setup", "Updating"]
let args = CommandLine.arguments
if args.count != 3 { fatalError("usage: design1_contact.swift DESIGN_PNG_DIR RENDER_DIR") }
let designDir = URL(fileURLWithPath: args[1], isDirectory: true)
let outDir = URL(fileURLWithPath: args[2], isDirectory: true)
func read(_ url: URL) throws -> CGImage {
    guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
          let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
        throw NSError(domain: "Design1Contact", code: 1, userInfo: [NSLocalizedDescriptionKey: url.path])
    }
    return image
}
func label(_ string: String, x: Int, y: Int, context: CGContext) {
    let attrs: [NSAttributedString.Key: Any] = [
        NSAttributedString.Key(kCTFontAttributeName as String): CTFontCreateWithName("Menlo" as CFString, 14, nil),
        NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor(gray: 1, alpha: 1)]
    context.textPosition = CGPoint(x: x, y: y)
    CTLineDraw(CTLineCreateWithAttributedString(NSAttributedString(string: string, attributes: attrs)), context)
}
do {
for screen in screens {
    let design = try read(designDir.appendingPathComponent(screen + ".png"))
    for variant in ["default", "ax3-bold-rt"] {
        let render = try read(outDir.appendingPathComponent(screen + "-" + variant + ".png"))
        guard design.width == 466 && design.height == 466 && render.width == 466 && render.height == 466 else {
            fatalError("466 px source and 1x SDL render required")
        }
        let context = CGContext(data: nil, width: 932, height: 510, bitsPerComponent: 8,
            bytesPerRow: 932 * 4, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        context.setFillColor(CGColor(gray: 0.08, alpha: 1))
        context.fill(CGRect(x: 0, y: 0, width: 932, height: 510))
        context.draw(design, in: CGRect(x: 0, y: 0, width: 466, height: 466))
        context.draw(render, in: CGRect(x: 466, y: 0, width: 466, height: 466))
        label(screen + " | Design 1.0", x: 14, y: 486, context: context)
        label("Firmware | " + variant, x: 480, y: 486, context: context)
        guard let image = context.makeImage(),
              let destination = CGImageDestinationCreateWithURL(outDir.appendingPathComponent(screen + "-" + variant + "-contact.png") as CFURL, UTType.png.identifier as CFString, 1, nil) else {
            fatalError("Could not create contact sheet")
        }
        CGImageDestinationAddImage(destination, image, nil)
        guard CGImageDestinationFinalize(destination) else { fatalError("Could not save PNG") }
    }
}
print("30 lossless design | firmware contact sheets: " + outDir.path)
} catch {
    fputs("design1 contact: \(error.localizedDescription)\n", stderr)
    exit(1)
}
