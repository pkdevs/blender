/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Shared pieces of the SwiftUI interface: Blender's snapshots as Swift values, the engine inbox,
 * and Blender's icons. UIKit owns only immutable snapshots; every edit goes through the inbox.
 */

import SwiftUI
import UIKit

typealias JSON = [String: Any]

extension Dictionary where Key == String, Value == Any {
  func string(_ key: String) -> String? { self[key] as? String }
  func bool(_ key: String) -> Bool {
    if let number = self[key] as? NSNumber { return number.boolValue }
    if let flag = self[key] as? Bool { return flag }
    return false
  }
  func has(_ key: String) -> Bool { self[key] != nil && !(self[key] is NSNull) }
  func int(_ key: String) -> Int? { (self[key] as? NSNumber)?.intValue }
  func double(_ key: String) -> Double? { (self[key] as? NSNumber)?.doubleValue }
  func array(_ key: String) -> [JSON] { (self[key] as? [Any])?.compactMap { $0 as? JSON } ?? [] }
  func strings(_ key: String) -> [String] { (self[key] as? [Any])?.compactMap { $0 as? String } ?? [] }
  func dict(_ key: String) -> JSON? { self[key] as? JSON }
}

/** Compares snapshot values, which are Foundation property lists. */
func jsonEqual(_ a: Any?, _ b: Any?) -> Bool {
  switch (a, b) {
    case (nil, nil): return true
    case let (x as NSObject, y as NSObject): return x.isEqual(y)
    default: return false
  }
}

enum Engine {
  /** Queue a setting for the engine; `value` is usually JSON. */
  static func setting(_ name: String, _ value: String) {
    WM_apple_ios_ui_setting_enqueue(name, value)
  }

  static func setting(_ name: String, json: Any) {
    guard JSONSerialization.isValidJSONObject(json),
          let data = try? JSONSerialization.data(withJSONObject: json),
          let text = String(data: data, encoding: .utf8)
    else { return }
    setting(name, text)
  }

  static func operatorCall(_ idname: String) {
    WM_apple_ios_ui_operator_enqueue(idname)
  }
}

/* -------------------------------------------------------------------- */
/** \name Icons
 * \{ */

enum BlenderIcon {
  /** Blender's icon by name, as `GHOSTIOSBlenderIcon` in `GHOST_IOSIcons.hh`. */
  /** One image per icon, so controls can tell an unchanged icon from a new one. */
  private static let cache = NSCache<NSString, UIImage>()

  static func image(_ name: String?, themed: Bool = false) -> UIImage? {
    guard let name, !name.isEmpty, name != "NONE" else { return nil }
    let key = name.lowercased()
    let cacheKey = (themed ? "themed." : "") + key as NSString
    if let cached = cache.object(forKey: cacheKey) { return cached }
    var image: UIImage?
    if let multicolor = UIImage(named: "blender.multicolor." + key) {
      image = multicolor.withRenderingMode(.alwaysOriginal)
    }
    else if themed, let tinted = UIImage(named: "blender.themed." + key) {
      image = tinted.withRenderingMode(.alwaysOriginal)
    }
    else {
      image = UIImage(named: "blender." + key)?.withRenderingMode(.alwaysTemplate)
    }
    if let image { cache.setObject(image, forKey: cacheKey) }
    return image
  }
}

/** A Blender icon, or an SF Symbol when Blender has none by that name. */
struct IconView: View {
  var name: String?
  var symbol: String? = nil
  var themed = false
  var size: CGFloat = 18

  var body: some View {
    if let image = BlenderIcon.image(name, themed: themed) {
      Image(uiImage: image)
        .resizable()
        .scaledToFit()
        .frame(width: size, height: size)
    }
    else if let symbol {
      Image(systemName: symbol)
        .frame(width: size, height: size)
    }
  }
}

/** Thumbnails Blender renders for previews (materials, MatCaps, HDRIs, data-blocks). */
enum PreviewCache {
  private static let cache = NSCache<NSString, UIImage>()

  static func image(_ path: String?) -> UIImage? {
    guard let path, !path.isEmpty else { return nil }
    if let image = cache.object(forKey: path as NSString) { return image }
    guard let image = UIImage(contentsOfFile: path) else { return nil }
    cache.setObject(image, forKey: path as NSString)
    return image
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Appearance
 * \{ */

/** Blender's widget colors for animated properties: yellow when keyed on this frame, green when
 * animated, orange when changed from the animated value, purple when driven. */
func animationStateColor(_ state: String?) -> Color? {
  switch state {
    case "KEYED": return .yellow
    case "ANIMATED": return .green
    case "CHANGED": return .orange
    case "DRIVEN": return .purple
    default: return nil
  }
}

func animationStateDescription(_ state: String?) -> String? {
  switch state {
    case "KEYED": return "Keyframed"
    case "ANIMATED", "CHANGED": return "Animated"
    case "DRIVEN": return "Driven"
    default: return nil
  }
}

/** Axis captions in Blender's axis colors. */
func axisColor(_ label: String?) -> Color {
  switch label {
    case "X", "R": return .red
    case "Y", "G": return .green
    case "Z", "B": return .blue
    default: return .secondary
  }
}

extension View {
  /** Run when Reduce Motion is off; otherwise apply immediately. */
  func motionAware<V: Equatable>(_ animation: Animation = .smooth(duration: 0.28), value: V) -> some View {
    self.animation(UIAccessibility.isReduceMotionEnabled ? nil : animation, value: value)
  }
}

/** Fills as UIKit draws them on the panels' glass, measured from UIKit-drawn panels: on glass,
 * UIKit's grouped and fill colors come out lighter in dark mode and darker in light mode than
 * the same colors drawn by SwiftUI. */
enum PanelStyle {
  private static func dynamic(dark: UIColor, light: UIColor) -> Color {
    Color(uiColor: UIColor { $0.userInterfaceStyle == .dark ? dark : light })
  }

  /** Grouped sections (table cells). */
  static let card = dynamic(dark: UIColor(white: 1, alpha: 0.085), light: UIColor(white: 0, alpha: 0.064))
  /** Number fields and capsules on a section. */
  static let field = dynamic(dark: UIColor(white: 1, alpha: 0.17), light: UIColor(white: 0, alpha: 0.14))
  /** The Outliner's search field. */
  static let search = dynamic(dark: UIColor(white: 1, alpha: 0.125), light: UIColor(white: 1, alpha: 0.48))
  /** Placeholder behind a missing preview. */
  static let thumbnail = dynamic(dark: UIColor(white: 1, alpha: 0.04), light: UIColor(white: 0, alpha: 0.11))
  /** Selected rows and tabs. */
  static let selection = dynamic(dark: UIColor(red: 47 / 255, green: 192 / 255, blue: 1, alpha: 1),
                                 light: UIColor(red: 0, green: 130 / 255, blue: 247 / 255, alpha: 1))
  /** Tinted text and glyphs: the blue as UIKit draws it on the panels' glass, which lightens it. */
  static let uiTint = UIColor {
    $0.userInterfaceStyle == .dark ? UIColor(red: 58 / 255, green: 203 / 255, blue: 1, alpha: 1) :
                                     UIColor(red: 0, green: 130 / 255, blue: 247 / 255, alpha: 1)
  }
  static let tint = Color(uiColor: uiTint)
  /** The active item of a list. */
  static let activeRow = Color(uiColor: .systemBlue).opacity(0.22)
}

func withMotion(_ animation: Animation = .smooth(duration: 0.28), _ body: () -> Void) {
  if UIAccessibility.isReduceMotionEnabled { body() }
  else { withAnimation(animation, body) }
}

/** \} */
