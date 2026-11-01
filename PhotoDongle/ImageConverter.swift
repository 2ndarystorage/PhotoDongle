import UIKit
import CoreImage

/// How a picked photo is mapped onto the 160x80 panel.
enum ScaleMode: Int, CaseIterable, Identifiable {
    case fit = 0      // whole image visible, letterboxed
    case cover = 1    // fill the panel, crop the overflow
    case shrink = 2   // like fit, but never upscale

    var id: Int { rawValue }
    var label: String {
        switch self {
        case .fit: return "フィット"
        case .cover: return "拡大"
        case .shrink: return "縮小"
        }
    }
}

enum DisplayOrder: Int, CaseIterable, Identifiable {
    case sequential = 0
    case random = 1
    var id: Int { rawValue }
    var label: String { self == .sequential ? "順番" : "ランダム" }
}

/// VJ effects generated on the phone as an animation frame set.
enum VJEffect: Int, CaseIterable, Identifiable {
    case vividScroll = 0
    case psychedelic = 1
    case invertPulse = 2
    case mandala = 3

    var id: Int { rawValue }
    var label: String {
        switch self {
        case .vividScroll: return "ビビッドスクロール"
        case .psychedelic: return "サイケデリック"
        case .invertPulse: return "反転パルス"
        case .mandala: return "マンダラ"
        }
    }
}

/// Converts a UIImage into the dongle's frame format:
/// exactly 160x80 pixels, RGB565 big-endian, row-major (25600 bytes).
enum ImageConverter {
    static let panelW = 160
    static let panelH = 80
    static let frameBytes = panelW * panelH * 2

    /// anchorX/anchorY in 0...1 choose which part of the photo is kept when
    /// cropping (0 = left/top, 0.5 = center, 1 = right/bottom). In fit/shrink
    /// modes they position the image inside the letterbox instead.
    static func targetRect(imageSize: CGSize, mode: ScaleMode,
                           anchorX: CGFloat = 0.5, anchorY: CGFloat = 0.5) -> CGRect {
        let iw = imageSize.width, ih = imageSize.height
        guard iw > 0, ih > 0 else { return CGRect(x: 0, y: 0, width: panelW, height: panelH) }
        let sx = CGFloat(panelW) / iw
        let sy = CGFloat(panelH) / ih
        var scale: CGFloat
        switch mode {
        case .fit: scale = min(sx, sy)
        case .cover: scale = max(sx, sy)
        case .shrink: scale = min(1.0, min(sx, sy))
        }
        let w = iw * scale, h = ih * scale
        return CGRect(x: (CGFloat(panelW) - w) * anchorX,
                      y: (CGFloat(panelH) - h) * anchorY,
                      width: w, height: h)
    }

    /// Render the image onto a black 160x80 canvas. Returns the preview image
    /// and the RGB565 big-endian frame data.
    static func convert(_ image: UIImage, mode: ScaleMode,
                        anchorX: CGFloat = 0.5, anchorY: CGFloat = 0.5)
        -> (preview: UIImage, frame: Data)? {
        let size = CGSize(width: panelW, height: panelH)
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = true
        let renderer = UIGraphicsImageRenderer(size: size, format: format)
        let rendered = renderer.image { ctx in
            UIColor.black.setFill()
            ctx.fill(CGRect(origin: .zero, size: size))
            image.draw(in: targetRect(imageSize: image.size, mode: mode,
                                      anchorX: anchorX, anchorY: anchorY))
        }
        guard let cg = rendered.cgImage, let frame = rgb565Frame(from: cg) else { return nil }
        return (rendered, frame)
    }

    /// Extract RGB565 big-endian bytes from a 160x80 CGImage.
    static func rgb565Frame(from cg: CGImage) -> Data? {
        var rgba = [UInt8](repeating: 0, count: panelW * panelH * 4)
        let cs = CGColorSpaceCreateDeviceRGB()
        guard let cctx = CGContext(data: &rgba, width: panelW, height: panelH,
                                   bitsPerComponent: 8, bytesPerRow: panelW * 4,
                                   space: cs,
                                   bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        cctx.draw(cg, in: CGRect(x: 0, y: 0, width: panelW, height: panelH))
        var frame = Data()
        frame.reserveCapacity(frameBytes)
        for i in 0..<(panelW * panelH) {
            let r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2]
            let v: UInt16 = (UInt16(r & 0xF8) << 8) | (UInt16(g & 0xFC) << 3) | UInt16(b >> 3)
            frame.append(UInt8(v >> 8))
            frame.append(UInt8(v & 0xFF))
        }
        return frame
    }

    // MARK: - VJ frame generation (CoreImage)

    private static let ciContext = CIContext(options: [.useSoftwareRenderer: false])

    /// Strip EXIF orientation by redrawing, so CIImage geometry is simple.
    private static func normalized(_ image: UIImage) -> UIImage {
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = true
        let renderer = UIGraphicsImageRenderer(size: image.size, format: format)
        return renderer.image { _ in
            image.draw(in: CGRect(origin: .zero, size: image.size))
        }
    }

    /// Base image: "cover"-cropped onto the panel rect with vivid colors.
    private static func vividBase(_ image: UIImage, saturation: CGFloat = 1.85) -> CIImage? {
        let img = normalized(image)
        guard let cg = img.cgImage else { return nil }
        let ci = CIImage(cgImage: cg)
        let iw = ci.extent.width, ih = ci.extent.height
        guard iw > 0, ih > 0 else { return nil }
        let s = max(CGFloat(panelW) / iw, CGFloat(panelH) / ih)
        var t = CGAffineTransform(scaleX: s, y: s)
        t = t.concatenating(CGAffineTransform(
            translationX: (CGFloat(panelW) - iw * s) / 2,
            y: (CGFloat(panelH) - ih * s) / 2))
        let placed = ci.transformed(by: t)
            .cropped(to: CGRect(x: 0, y: 0, width: panelW, height: panelH))
        return apply(placed, filter: "CIColorControls",
                     params: ["inputSaturation": saturation,
                              "inputContrast": 1.12,
                              "inputBrightness": 0.02])
    }

    private static func apply(_ image: CIImage, filter name: String,
                              params: [String: Any]) -> CIImage {
        guard let f = CIFilter(name: name) else { return image }
        f.setValue(image, forKey: kCIInputImageKey)
        for (k, v) in params { f.setValue(v, forKey: k) }
        return f.outputImage ?? image
    }

    /// Zoom around the panel center WITHOUT cropping, so the result still
    /// holds pixels outside the panel (needed before panning).
    private static func zoomedRaw(_ image: CIImage, factor: CGFloat) -> CIImage {
        let cx = CGFloat(panelW) / 2, cy = CGFloat(panelH) / 2
        var t = CGAffineTransform(translationX: cx, y: cy)
        t = t.concatenating(CGAffineTransform(scaleX: factor, y: factor))
        t = t.concatenating(CGAffineTransform(translationX: -cx, y: -cy))
        return image.transformed(by: t)
    }

    private static func zoomed(_ image: CIImage, factor: CGFloat) -> CIImage {
        zoomedRaw(image, factor: factor)
            .cropped(to: CGRect(x: 0, y: 0, width: panelW, height: panelH))
    }

    /// Render one CIImage as a frame.
    private static func frame(from ci: CIImage) -> Data? {
        let rect = CGRect(x: 0, y: 0, width: panelW, height: panelH)
        let normalized = ci.transformed(by: CGAffineTransform(
            translationX: -ci.extent.origin.x, y: -ci.extent.origin.y))
        guard let cg = ciContext.createCGImage(normalized, from: rect) else { return nil }
        return rgb565Frame(from: cg)
    }

    /// Build `count` animation frames for the chosen VJ effect.
    static func vjFrames(from image: UIImage, effect: VJEffect, count: Int) -> [Data] {
        guard count > 0, let base = vividBase(image) else { return [] }
        var frames: [Data] = []
        frames.reserveCapacity(count)
        let panel = CGRect(x: 0, y: 0, width: panelW, height: panelH)
        for i in 0..<count {
            let phase = CGFloat(i) / CGFloat(count)          // 0..<1, loops
            let cycle = phase * 2 * CGFloat.pi
            var out: CIImage
            switch effect {
            case .vividScroll:
                // Zoom first (uncropped) so there is real image data on both
                // sides, then ping-pong the view window across it.
                let z: CGFloat = 1.45
                let wide = zoomedRaw(base, factor: z)
                let travel = CGFloat(panelW) * (z - 1)
                let tri = phase < 0.5 ? phase * 2 : 2 - phase * 2
                let shifted = wide.transformed(by: CGAffineTransform(
                    translationX: travel / 2 - tri * travel, y: 0))
                out = shifted.cropped(to: panel)
            case .psychedelic:
                var im = apply(base, filter: "CIHueAdjust",
                               params: ["inputAngle": phase * 4 * CGFloat.pi])
                im = apply(im, filter: "CIColorControls",
                           params: ["inputSaturation": 2.1, "inputContrast": 1.1,
                                    "inputBrightness": 0.0])
                out = zoomed(im, factor: 1.08 + 0.07 * sin(cycle))
            case .invertPulse:
                var im = apply(base, filter: "CIHueAdjust",
                               params: ["inputAngle": phase * 1.2])
                if (i % 4) >= 2 {
                    im = apply(im, filter: "CIColorInvert", params: [:])
                }
                out = im
            case .mandala:
                let clamped = apply(base, filter: "CIAffineClamp",
                                    params: ["inputTransform": NSValue(cgAffineTransform: .identity)])
                let kal = apply(clamped, filter: "CIKaleidoscope",
                                params: ["inputCount": 6,
                                         "inputCenter": CIVector(x: CGFloat(panelW) / 2, y: CGFloat(panelH) / 2),
                                         "inputAngle": phase * CGFloat.pi])
                out = apply(kal, filter: "CIColorControls",
                            params: ["inputSaturation": 1.7, "inputContrast": 1.08,
                                     "inputBrightness": 0.0]).cropped(to: panel)
            }
            if let d = frame(from: out) { frames.append(d) }
        }
        return frames
    }
}
