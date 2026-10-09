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
  static func image(_ name: String?, themed: Bool = false) -> UIImage? {
    guard let name, !name.isEmpty, name != "NONE" else { return nil }
    let key = name.lowercased()
    if let multicolor = UIImage(named: "blender.multicolor." + key) {
      return multicolor.withRenderingMode(.alwaysOriginal)
    }
    if themed, let tinted = UIImage(named: "blender.themed." + key) {
      return tinted.withRenderingMode(.alwaysOriginal)
    }
    return UIImage(named: "blender." + key)?.withRenderingMode(.alwaysTemplate)
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

func withMotion(_ animation: Animation = .smooth(duration: 0.28), _ body: () -> Void) {
  if UIAccessibility.isReduceMotionEnabled { body() }
  else { withAnimation(animation, body) }
}

/** \} */
