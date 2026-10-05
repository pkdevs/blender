// SPDX-FileCopyrightText: 2026 Blender Authors
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit

let bitmap = NSBitmapImageRep(data: try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1])))!
let color = bitmap.colorAt(x: bitmap.pixelsWide * 4 / 5, y: bitmap.pixelsHigh / 2)!
    .usingColorSpace(.deviceRGB)!
let r = color.redComponent, g = color.greenComponent, b = color.blueComponent
print("Viewport pixel: \(r), \(g), \(b)")
let valid = CommandLine.arguments[2] == "green" ?
    g > r * 1.5 && g > b * 1.5 : b > r * 1.5 && b > g * 1.5
exit(valid ? 0 : 1)
