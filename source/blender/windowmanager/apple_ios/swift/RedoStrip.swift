/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Adjust Last Operation: the last operator's settings, which the actions row shows on a strip
 * above the bottom edge, folded into a pill beside the actions until opened.
 */

import SwiftUI
import UIKit

/* -------------------------------------------------------------------- */
/** \name State
 * \{ */

@MainActor
final class RedoStore: ObservableObject {
  /** Short titles for the folded pill, where Blender's are long. */
  static let shortNames = [
    "TRANSFORM_OT_translate": "Move", "TRANSFORM_OT_rotate": "Rotate",
    "TRANSFORM_OT_resize": "Scale", "MESH_OT_extrude_region_move": "Extrude",
    "MESH_OT_extrude_context_move": "Extrude", "MESH_OT_extrude_faces_move": "Extrude",
    "MESH_OT_duplicate_move": "Duplicate", "OBJECT_OT_duplicate_move": "Duplicate",
    "OBJECT_OT_duplicate_move_linked": "Duplicate Linked",
  ]

  @Published private(set) var snapshot: JSON?
  @Published private var local: [String: Any] = [:]
  /** Bumped when another operation arrives, so fields drop what was being typed. */
  @Published private(set) var generation = 0
  /** Width of the title and controls, which the strip hugs. */
  var contentWidth: CGFloat = 0
  var contentWidthChanged: () -> Void = {}
  private var pending: [String: UInt64] = [:]
  private var sequence: UInt64 = 0
  var icon: (String) -> UIImage? = { BlenderIcon.image($0) }
  var close: () -> Void = {}

  var operation: String? { snapshot?.string("id") }
  var properties: [JSON] { snapshot?.array("properties") ?? [] }

  func update(_ next: JSON?) {
    guard let next else {
      snapshot = nil
      local.removeAll()
      pending.removeAll()
      return
    }
    if next.string("id") != snapshot?.string("id") {
      local.removeAll()
      pending.removeAll()
      generation += 1
    }
    let ack = (next["ack"] as? NSNumber)?.uint64Value ?? 0
    sequence = max(sequence, ack)
    /* Keep a value the user just set until Blender has applied it. */
    for (key, edit) in pending where edit <= ack {
      pending.removeValue(forKey: key)
      local.removeValue(forKey: key)
    }
    snapshot = next
  }

  func value(_ property: JSON) -> Any? {
    local[property.string("key") ?? ""] ?? property["value"]
  }

  func send(_ property: JSON?, value: Any?, interaction: String? = nil, phase: String? = nil) {
    guard let operation else { return }
    sequence += 1
    var request: JSON = ["id": operation, "sequence": NSNumber(value: sequence)]
    if let interaction, let phase {
      request["interaction"] = interaction
      request["phase"] = phase
    }
    if let property, let key = property.string("key"), let value {
      request["key"] = key
      request["value"] = value
      pending[key] = sequence
      local[key] = value
    }
    else {
      request["action"] = "dismiss"
    }
    Engine.setting("native_redo_edit", json: request)
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Controls
 * \{ */

private func redoNumberText(_ value: Double, _ property: JSON) -> String {
  let formatter = NumberFormatter()
  formatter.numberStyle = .decimal
  formatter.usesGroupingSeparator = false
  formatter.maximumFractionDigits = property.string("type") == "INT" ? 0 : 4
  return formatter.string(from: NSNumber(value: value * (property.bool("angle") ? 180 / .pi : 1))) ?? ""
}

struct RedoContent: View {
  @ObservedObject var store: RedoStore

  var body: some View {
    HStack(spacing: 0) {
      Button(action: store.close) {
        Image(systemName: "xmark")
          .font(.system(size: 19, weight: .semibold))
          .foregroundStyle(.primary)
          .frame(width: 44, height: 48)
          .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .padding(.leading, 4)
      .accessibilityLabel("Collapse Last Operation")
      .accessibilityIdentifier("blender.redo.done")
      .accessibilityHint(store.snapshot?.string("error") ?? "")
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 14) {
          Text(store.snapshot?.string("title") ?? "")
            .font(.subheadline)
            .lineLimit(2)
            .frame(width: 112, alignment: .leading)
            .accessibilityIdentifier("blender.redo.title")
          ForEach(Array(store.properties.enumerated()), id: \.element.redoKey) { _, property in
            RedoColumn(store: store, property: property)
          }
        }
        .padding(.horizontal, 12)
        .frame(maxHeight: .infinity)
        .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { width in
          store.contentWidth = width
          store.contentWidthChanged()
        }
      }
      .softHorizontalScrollEdge()
    }
    .tint(Color(uiColor: .systemBlue))
  }
}

private extension Dictionary where Key == String, Value == Any {
  var redoKey: String { "\(string("key") ?? ""):\(string("type") ?? "")" }
}

struct RedoColumn: View {
  @ObservedObject var store: RedoStore
  let property: JSON

  private var type: String { property.string("type") ?? "" }
  private var name: String {
    if let group = property.string("group"), !group.isEmpty { return "\(group) \(property.string("label") ?? "")" }
    return property.string("label") ?? ""
  }

  private var width: CGFloat {
    switch type {
      case "BOOLEAN":
        let measured = (name as NSString).size(withAttributes: [.font: UIFont.preferredFont(forTextStyle: .caption1)]).width
        return max(60, min(180, ceil(measured)))
      case "ENUM": return 148
      case "COLOR": return 72
      default: return property.has("slider") ? 148 : 110
    }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 2) {
      Text(name)
        .font(.caption)
        .foregroundStyle(.secondary)
        .lineLimit(1)
        .minimumScaleFactor(0.8)
      control
        .disabled(!property.bool("editable"))
        .accessibilityLabel(name)
        .accessibilityIdentifier("blender.redo.property." + (property.string("key") ?? ""))
    }
    .frame(width: width, alignment: .leading)
  }

  @ViewBuilder
  private var control: some View {
    switch type {
      case "BOOLEAN":
        Toggle(name, isOn: Binding(get: { (store.value(property) as? NSNumber)?.boolValue ?? false },
                                   set: { store.send(property, value: $0) }))
          .labelsHidden()
      case "ENUM":
        RedoEnum(store: store, property: property)
          .frame(height: 32)
      case "COLOR":
        RedoColor(store: store, property: property)
          .frame(height: 32)
      default:
        if let range = property.dict("slider") {
          RedoSlider(store: store, property: property,
                     range: (range.double("min") ?? 0)...(max(range.double("min") ?? 0, range.double("max") ?? 1)))
            .frame(height: 32)
        }
        else {
          RedoField(store: store, property: property)
            .frame(height: 32)
        }
    }
  }
}

struct RedoEnum: View {
  @ObservedObject var store: RedoStore
  let property: JSON

  var body: some View {
    let options = property.array("options")
    let current = options.first { $0.bool("checked") }
    Menu {
      ForEach(Array(options.enumerated()), id: \.offset) { _, option in
        Button {
          store.send(property, value: option["value"] ?? "")
        } label: {
          Label {
            Text(option.string("label") ?? "")
          } icon: {
            if option.bool("checked") { Image(systemName: "checkmark") }
            else if let image = store.icon(option.string("icon") ?? "") { Image(uiImage: image) }
          }
        }
      }
    } label: {
      HStack(spacing: 4) {
        if let image = store.icon(current?.string("icon") ?? "") {
          Image(uiImage: image).resizable().scaledToFit().frame(width: 18, height: 18)
        }
        Text(current?.string("label") ?? "None").lineLimit(1)
      }
      .foregroundStyle(.primary)
      .frame(maxWidth: .infinity)
    }
    .accessibilityValue(current?.string("label") ?? "")
  }
}

struct RedoColor: View {
  @ObservedObject var store: RedoStore
  let property: JSON
  private let anchor = AnchorBox()

  var body: some View {
    Button {
      RedoColorSession(property: property, store: store).present(from: anchor,
                                                                 color: blenderColor(store.value(property)))
    } label: {
      RoundedRectangle(cornerRadius: 6, style: .continuous)
        .fill(Color(uiColor: blenderColor(store.value(property))))
        .background(PresentationAnchor(anchor: anchor))
    }
    .buttonStyle(.plain)
  }
}

/** The color picker for a redo color: changes preview, closing commits. */
@MainActor
final class RedoColorSession: NSObject, UIColorPickerViewControllerDelegate {
  private static var active: RedoColorSession?
  let property: JSON
  weak var store: RedoStore?
  private let operation: String?
  private var lastPreview: CFTimeInterval = 0

  init(property: JSON, store: RedoStore) {
    self.property = property
    self.store = store
    operation = store.operation
  }

  func present(from anchor: AnchorBox, color: UIColor) {
    guard anchor.presenter?.presentedViewController == nil else { return }
    let picker = UIColorPickerViewController()
    picker.delegate = self
    picker.selectedColor = color
    picker.supportsAlpha = property.bool("alpha")
    anchor.present(picker)
    RedoColorSession.active = self
  }

  private func components(_ color: UIColor, alpha: Bool) -> [Double]? {
    var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
    guard color.getRed(&r, green: &g, blue: &b, alpha: &a) else { return nil }
    return alpha ? [r, g, b, a].map(Double.init) : [r, g, b].map(Double.init)
  }

  func colorPickerViewController(_ picker: UIColorPickerViewController, didSelect color: UIColor,
                                 continuously: Bool)
  {
    guard let store, store.operation == operation, let operation,
          !continuously || CACurrentMediaTime() - lastPreview >= 1.0 / 60.0,
          let value = components(color, alpha: picker.supportsAlpha)
    else { return }
    store.send(property, value: value, interaction: "color:" + operation,
               phase: continuously ? "update" : "end")
    lastPreview = CACurrentMediaTime()
  }

  func colorPickerViewControllerDidFinish(_ picker: UIColorPickerViewController) {
    if let store, store.operation == operation, let value = components(picker.selectedColor,
                                                                        alpha: picker.supportsAlpha)
    {
      store.send(property, value: value)
    }
    RedoColorSession.active = nil
  }
}

struct RedoSlider: View {
  @ObservedObject var store: RedoStore
  let property: JSON
  let range: ClosedRange<Double>
  @State private var dragging: Double?
  @State private var start = 0.0
  @State private var interaction: String?
  @State private var lastPreview: CFTimeInterval = 0

  var body: some View {
    Slider(value: Binding(
      get: { dragging ?? numericValue(store.value(property)) },
      set: { value in
        dragging = value
        guard CACurrentMediaTime() - lastPreview >= 1.0 / 60.0 else { return }
        store.send(property, value: rounded(value), interaction: interaction, phase: "update")
        lastPreview = CACurrentMediaTime()
      }), in: range, onEditingChanged: { began in
        if began {
          interaction = UUID().uuidString
          start = numericValue(store.value(property))
          lastPreview = 0
          return
        }
        guard let interaction else { return }
        store.send(property, value: rounded(dragging ?? start), interaction: interaction, phase: "end")
        self.interaction = nil
        dragging = nil
      })
  }

  private func rounded(_ value: Double) -> Double {
    property.string("type") == "INT" ? value.rounded() : value
  }
}

/** A number scrubs with a sideways drag, as in Properties; a tap types a value. */
struct RedoField: View {
  @ObservedObject var store: RedoStore
  let property: JSON
  @State private var editing = false
  @State private var text = ""
  @State private var scrub: Double?
  /** The field's UIKit view, for the drag sensitivity indicator. */
  @State private var scrubAnchor = AnchorBox()
  @State private var scrubStart = 0.0
  @State private var lastTranslation = 0.0
  @State private var interaction = ""
  @State private var lastPreview: CFTimeInterval = 0
  @FocusState private var focused: Bool

  private var isString: Bool { property.string("type") == "STRING" }

  private var shown: String {
    if isString { return store.value(property) as? String ?? "" }
    return redoNumberText(scrub ?? numericValue(store.value(property)), property)
  }

  var body: some View {
    ZStack {
      if editing {
        TextField("", text: $text)
          .multilineTextAlignment(.center)
          .keyboardType(isString ? .default : .numbersAndPunctuation)
          .autocorrectionDisabled()
          .submitLabel(.done)
          .focused($focused)
          .onSubmit { commit() }
      }
      else {
        Text(shown)
          .lineLimit(1)
          .minimumScaleFactor(0.8)
          .frame(maxWidth: .infinity)
      }
    }
    .font(isString ? .body : .system(size: 15).monospacedDigit())
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(scrub != nil || editing ? Color.accentColor.opacity(0.18) : PanelStyle.field,
                in: RoundedRectangle(cornerRadius: 6, style: .continuous))
    .contentShape(Rectangle())
    .hoverEffect(.highlight)
    .onTapGesture {
      guard property.bool("editable"), scrub == nil else { return }
      text = shown
      editing = true
      focused = true
    }
    .background(PresentationAnchor(anchor: scrubAnchor))
    .gesture(ScrubGesture(enabled: property.has("dragStep") && !isString && !editing && property.bool("editable"),
                          anchor: scrubAnchor, began: beginScrub, changed: changeScrub, ended: endScrub))
    .onChange(of: focused) { _, isFocused in if !isFocused { commit() } }
    .onChange(of: store.generation) { _, _ in
      editing = false
      scrub = nil
    }
  }

  private func commit() {
    guard editing else { return }
    editing = false
    focused = false
    guard text != shown else { return }
    if isString {
      store.send(property, value: text)
      return
    }
    guard let number = parseNumber(text, property: property) else {
      UIAccessibility.post(notification: .announcement, argument: "Enter a valid number.")
      return
    }
    store.send(property, value: number)
  }

  private func beginScrub() {
    scrubStart = numericValue(store.value(property))
    scrub = scrubStart
    lastTranslation = 0
    interaction = UUID().uuidString
    lastPreview = 0
  }

  private func changeScrub(_ translation: CGPoint) {
    guard var value = scrub else { return }
    let step = (property.double("dragStep") ?? 0.01) * (property.bool("angle") ? 10 : 1)
    value += (translation.x - lastTranslation) * step * scrubSensitivity(translation.y)
    value = min(property.double("max") ?? .greatestFiniteMagnitude,
                max(property.double("min") ?? -.greatestFiniteMagnitude, value))
    lastTranslation = translation.x
    scrub = value
    if CACurrentMediaTime() - lastPreview >= 1.0 / 60.0 {
      store.send(property, value: rounded(value), interaction: interaction, phase: "update")
      lastPreview = CACurrentMediaTime()
    }
  }

  private func endScrub(_ cancelled: Bool) {
    guard let value = scrub else { return }
    store.send(property, value: cancelled ? scrubStart : rounded(value), interaction: interaction,
               phase: cancelled ? "cancel" : "end")
    scrub = nil
  }

  private func rounded(_ value: Double) -> Double {
    property.string("type") == "INT" ? value.rounded() : value
  }
}

/** \} */
