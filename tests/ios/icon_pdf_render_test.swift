// SPDX-FileCopyrightText: 2026 Blender Authors
//
// SPDX-License-Identifier: GPL-2.0-or-later

import CoreGraphics
import Foundation

var results: [[String: Any]] = []
for path in CommandLine.arguments.dropFirst() {
  guard let document = CGPDFDocument(URL(fileURLWithPath: path) as CFURL),
        let page = document.page(at: 1),
        let space = CGColorSpace(name: CGColorSpace.sRGB),
        let context = CGContext(data: nil, width: 256, height: 256, bitsPerComponent: 8,
                                bytesPerRow: 1024, space: space,
                                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue),
        let data = context.data else { fatalError("Cannot render \(path)") }
  let rect = page.getBoxRect(.mediaBox)
  context.scaleBy(x: 256 / rect.width, y: 256 / rect.height)
  context.drawPDFPage(page)
  let pixels = data.assumingMemoryBound(to: UInt8.self)
  var opaqueColors = Set<UInt32>()
  var transparent = 0
  var translucent = 0
  for offset in stride(from: 0, to: 256 * 256 * 4, by: 4) {
    if pixels[offset + 3] == 0 { transparent += 1 }
    else if pixels[offset + 3] < 255 { translucent += 1 }
    else {
      opaqueColors.insert(UInt32(pixels[offset]) << 16 |
                          UInt32(pixels[offset + 1]) << 8 | UInt32(pixels[offset + 2]))
    }
  }
  results.append(["width": rect.width, "height": rect.height,
                  "opaqueColors": opaqueColors.sorted(), "transparent": transparent,
                  "translucent": translucent])
}
let json = try JSONSerialization.data(withJSONObject: results, options: [.sortedKeys])
print(String(data: json, encoding: .utf8)!)
