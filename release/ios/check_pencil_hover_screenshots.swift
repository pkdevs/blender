// SPDX-FileCopyrightText: 2026 Blender Authors
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Check screenshots captured during pencil_hover_device_test.py's named stages.
// Usage: swift check_pencil_hover_screenshots.swift /path/to/prefix
// Files: prefix-{object,cleared,vertex,edge,face,unselected}.png
import Foundation
import CoreGraphics
import ImageIO

precondition(CommandLine.arguments.count == 2, "Supply the screenshot filename prefix")
let prefix = CommandLine.arguments[1]
for stage in ["object", "cleared", "vertex", "edge", "face", "unselected"] {
  let url = URL(fileURLWithPath: "\(prefix)-\(stage).png")
  guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
        let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
    fatalError("Missing screenshot: \(url.path)")
  }
  let width = image.width, height = image.height
  var pixels = [UInt8](repeating: 0, count: width * height * 4)
  pixels.withUnsafeMutableBytes { bytes in
    let context = CGContext(data: bytes.baseAddress, width: width, height: height,
                            bitsPerComponent: 8, bytesPerRow: width * 4,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
  }
  // Exclude native controls and the navigation gizmo; test geometry occupies the middle.
  var blue = 0
  for y in (height / 4)..<(height * 3 / 4) {
    for x in (width * 3 / 10)..<(width * 17 / 20) {
      let index = (y * width + x) * 4
      let r = Int(pixels[index]), g = Int(pixels[index + 1]), b = Int(pixels[index + 2])
      if r < 170 && g > r + 35 && b > g + 20 { blue += 1 }
    }
  }
  let valid = stage == "cleared" ? blue == 0 : blue >= (stage == "vertex" ? 30 : 300)
  guard valid else {
    print("FAIL: \(stage): missing visible hover highlight (\(blue) blue pixels)")
    exit(1)
  }
  print("\(stage): verified \(blue) blue highlight pixels")
}
