/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * One control per Blender property: switches, number fields that scrub sideways or take typed
 * values, sliders, menus, colors and text. Touch and hold for locking and keyframing.
 */

import SwiftUI
import UIKit

/* -------------------------------------------------------------------- */
/** \name Number formatting
 * \{ */

func formatNumber(_ value: Double, property: JSON, significant: Int) -> String {
  let formatter = NumberFormatter()
  formatter.numberStyle = .decimal
  formatter.usesGroupingSeparator = false
  formatter.usesSignificantDigits = true
  formatter.maximumSignificantDigits = property.string("type") == "INT" ? 10 : significant
  var shown = value * (property.bool("angle") ? 180.0 / .pi : 1)
  if property.string("type") == "INT" { shown = shown.rounded() }
  return formatter.string(from: NSNumber(value: shown)) ?? "\(shown)"
}

func parseNumber(_ text: String, property: JSON) -> Double? {
  let scanner = Scanner(string: text.trimmingCharacters(in: .whitespaces))
  scanner.locale = Locale.current
  guard let number = scanner.scanDouble(), scanner.isAtEnd, number.isFinite else {
    /* Accept a period even where the locale's decimal separator differs. */
    guard let number = Double(text.trimmingCharacters(in: .whitespaces)), number.isFinite else { return nil }
    return property.bool("angle") ? number * .pi / 180.0 : number
  }
  return property.bool("angle") ? number * .pi / 180.0 : number
}

func numericValue(_ value: Any?) -> Double {
  (value as? NSNumber)?.doubleValue ?? 0
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Number field
 * \{ */

/** A number that scrubs with a sideways drag (drag up for coarser, down for finer steps) and
 * takes a typed value on tap, with a stepper above the keyboard. */
struct NumberField: View {
  @ObservedObject var store: InspectorStore
  let property: JSON
  var compact = false
  /** Sliders scrub with their track instead. */
  var scrubs = true
  var significant = 9

  @State private var editing = false
  @State private var text = ""
  @State private var scrubValue: Double?
  @State private var scrubStart = 0.0
  @State private var lastTranslation = 0.0
  @State private var interaction = ""
  @State private var lastPreview: CFTimeInterval = 0
  @State private var stepperValue = 0.0
  @FocusState private var focused: Bool

  private var editable: Bool { property.bool("editable") }
  private var value: Double { scrubValue ?? numericValue(store.value(property)) }

  var body: some View {
    ZStack(alignment: .trailing) {
      if editing {
        TextField("", text: $text)
          .keyboardType(.numbersAndPunctuation)
          .multilineTextAlignment(.trailing)
          .font(.body.monospacedDigit())
          .focused($focused)
          .submitLabel(.done)
          .onSubmit { commit() }
          .toolbar {
            ToolbarItemGroup(placement: .keyboard) {
              Stepper("Adjust \(propertyLabel(property))", value: $stepperValue,
                      in: stepperRange, step: stepperStep)
                .labelsHidden()
                .onChange(of: stepperValue) { _, newValue in
                  text = formatStepper(newValue)
                }
              Spacer()
              Button("Done") { focused = false }
                .bold()
            }
          }
      }
      else {
        Text(formatNumber(value, property: property, significant: significant))
          .font((compact ? Font.callout : Font.body).monospacedDigit())
          .lineLimit(1)
          .minimumScaleFactor(0.7)
          .foregroundStyle(editable ? .primary : .secondary)
          .frame(maxWidth: .infinity, alignment: .trailing)
      }
    }
    .padding(.horizontal, 8)
    .frame(minHeight: compact ? 30 : 36)
    .background(background, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
    .overlay {
      if scrubValue != nil {
        HStack {
          Image(systemName: "chevron.left")
          Spacer()
          Image(systemName: "chevron.right")
        }
        .font(.caption2.weight(.semibold))
        .foregroundStyle(.tint)
        .padding(.horizontal, 3)
        .allowsHitTesting(false)
      }
    }
    .contentShape(RoundedRectangle(cornerRadius: 8))
    .hoverEffect(.highlight)
    .onTapGesture { beginEditing() }
    .gesture(ScrubGesture(enabled: scrubs && editable && !editing,
                          began: beginScrub, changed: changeScrub, ended: endScrub))
    .onChange(of: focused) { _, isFocused in
      if !isFocused && editing { commit() }
    }
    .onChange(of: store.generation) { _, _ in
      editing = false
      scrubValue = nil
    }
    .accessibilityElement()
    .accessibilityLabel(propertyLabel(property))
    .accessibilityValue(formatNumber(value, property: property, significant: significant))
    .accessibilityAdjustableAction { direction in
      let step = stepperStep * (property.bool("angle") ? .pi / 180 : 1)
      store.edit(property, value: clamp(value + (direction == .increment ? step : -step)))
    }
  }

  private var background: Color {
    if scrubValue != nil || editing { return Color.accentColor.opacity(0.18) }
    if let color = animationStateColor(property.string("anim_state")) { return color.opacity(0.35) }
    return Color(uiColor: .tertiarySystemFill)
  }

  private var stepperStep: Double {
    property.bool("angle") || property.string("type") == "INT" ? 1 : 0.1
  }

  private var stepperRange: ClosedRange<Double> {
    let factor = property.bool("angle") ? 180.0 / .pi : 1
    let low = max(-1e12, (property.double("min") ?? -1e12) * factor)
    let high = min(1e12, (property.double("max") ?? 1e12) * factor)
    return low <= high ? low...high : -1e12...1e12
  }

  private func formatStepper(_ value: Double) -> String {
    let formatter = NumberFormatter()
    formatter.numberStyle = .decimal
    formatter.usesGroupingSeparator = false
    formatter.maximumFractionDigits = 6
    return formatter.string(from: NSNumber(value: value)) ?? "\(value)"
  }

  private func clamp(_ value: Double) -> Double {
    min(property.double("max") ?? .greatestFiniteMagnitude,
        max(property.double("min") ?? -.greatestFiniteMagnitude, value))
  }

  private func beginEditing() {
    guard editable, scrubValue == nil else { return }
    text = formatNumber(value, property: property, significant: significant)
    stepperValue = value * (property.bool("angle") ? 180.0 / .pi : 1)
    editing = true
    focused = true
  }

  private func commit() {
    guard editing else { return }
    editing = false
    focused = false
    if text == formatNumber(value, property: property, significant: significant) { return }
    guard let number = parseNumber(text, property: property) else {
      store.showStatus("Enter a valid number.")
      return
    }
    let result = property.string("type") == "INT" ? number.rounded() : number
    store.edit(property, value: result)
  }

  private func beginScrub() {
    guard editable else { return }
    scrubStart = numericValue(store.value(property))
    scrubValue = scrubStart
    lastTranslation = 0
    interaction = UUID().uuidString
    lastPreview = 0
    Haptics.selection()
  }

  private func changeScrub(_ translation: CGPoint) {
    guard let current = scrubValue, translation.x.isFinite, translation.y.isFinite else { return }
    var step = property.double("dragStep") ?? (property.string("type") == "INT" ? 0.5 : 0.01)
    if property.bool("angle") { step *= 10 * .pi / 180 }
    /* Integrate the horizontal motion, so changing precision never reinterprets earlier motion. */
    let next = current + (translation.x - lastTranslation) * step * scrubSensitivity(translation.y)
    lastTranslation = translation.x
    scrubValue = clamp(next)
    if CACurrentMediaTime() - lastPreview >= 1.0 / 60.0, let value = scrubValue {
      store.preview(property, value: rounded(value), interaction: interaction, phase: "update")
      lastPreview = CACurrentMediaTime()
    }
  }

  private func endScrub(_ cancelled: Bool) {
    guard let value = scrubValue else { return }
    let result = rounded(value)
    store.preview(property, value: cancelled ? scrubStart : result, interaction: interaction,
                  phase: cancelled || result == scrubStart ? "cancel" : "end")
    scrubValue = nil
  }

  private func rounded(_ value: Double) -> Double {
    property.string("type") == "INT" ? value.rounded() : value
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Slider
 * \{ */

/** Blender's slider ranges, including logarithmic and cubic scales. */
struct SliderScale {
  let minimum: Double
  let maximum: Double
  let scale: String

  init(_ property: JSON) {
    let range = property.dict("slider") ?? [:]
    minimum = range.double("min") ?? 0
    maximum = range.double("max") ?? 1
    scale = range.string("scale") ?? ""
  }

  func value(_ fraction: Double) -> Double {
    if scale == "LOG" && minimum > 0 { return minimum * pow(maximum / minimum, fraction) }
    let linear = minimum + fraction * (maximum - minimum)
    if scale == "CUBIC" {
      return (pow(linear, 3) - pow(minimum, 3)) / (pow(maximum, 3) - pow(minimum, 3)) *
        (maximum - minimum) + minimum
    }
    return linear
  }

  func fraction(_ value: Double) -> Double {
    var fraction = (value - minimum) / (maximum - minimum)
    if scale == "LOG" && minimum > 0 {
      fraction = value > 0 ? log(value / minimum) / log(maximum / minimum) : 0
    }
    else if scale == "CUBIC" {
      let cube = fraction * (pow(maximum, 3) - pow(minimum, 3)) + pow(minimum, 3)
      fraction = (cbrt(cube) - minimum) / (maximum - minimum)
    }
    return fraction.isFinite ? min(1, max(0, fraction)) : 0
  }
}

struct PropertySlider: View {
  @ObservedObject var store: InspectorStore
  let property: JSON
  @State private var dragging: Double?
  @State private var start = 0.0
  @State private var interaction = ""
  @State private var lastPreview: CFTimeInterval = 0

  var body: some View {
    let scale = SliderScale(property)
    let integer = property.string("type") == "INT"
    Slider(value: Binding(
      get: { dragging ?? scale.fraction(numericValue(store.value(property))) },
      set: { fraction in
        dragging = fraction
        var value = scale.value(fraction)
        if integer { value = value.rounded() }
        if CACurrentMediaTime() - lastPreview >= 1.0 / 60.0 {
          store.preview(property, value: value, interaction: interaction, phase: "update")
          lastPreview = CACurrentMediaTime()
        }
      }), in: 0...1, onEditingChanged: { began in
        if began {
          start = numericValue(store.value(property))
          interaction = UUID().uuidString
          lastPreview = 0
          return
        }
        guard let fraction = dragging else { return }
        var value = scale.value(fraction)
        if integer { value = value.rounded() }
        store.preview(property, value: value, interaction: interaction,
                      phase: value == start ? "cancel" : "end")
        dragging = nil
      })
      .animationTint(property.string("anim_state"))
      .disabled(!property.bool("editable"))
      .accessibilityLabel(propertyLabel(property))
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Property rows
 * \{ */

extension View {
  /** Blender's animation color, keeping the app's accent when the property isn't animated. */
  @ViewBuilder
  func animationTint(_ state: String?) -> some View {
    if let color = animationStateColor(state) { self.tint(color) }
    else { self }
  }
}

/** Lock and keyframe actions for a property, shown when touching and holding it. */
struct PropertyActions: View {
  @ObservedObject var store: InspectorStore
  let property: JSON

  var body: some View {
    if let lock = property.dict("lock") {
      let locked = lock.bool("value")
      Button {
        if let current = store.current(propertyKey(property))?.dict("lock"), current.bool("editable") {
          store.edit(current, value: !locked)
        }
      } label: {
        Label(locked ? "Unlock" : "Lock", systemImage: locked ? "lock.open" : "lock")
      }
      .disabled(!lock.bool("editable"))
    }
    ForEach(Array(property.array("animation").enumerated()), id: \.offset) { _, entry in
      Button {
        store.animate(store.current(propertyKey(property)) ?? property, action: entry.string("id") ?? "")
      } label: {
        Label {
          Text(entry.string("title") ?? "")
        } icon: {
          if let image = BlenderIcon.image(entry.string("icon")) { Image(uiImage: image) }
        }
      }
    }
  }
}

extension View {
  @ViewBuilder
  func propertyActions(_ store: InspectorStore, _ property: JSON) -> some View {
    if property.has("lock") || !property.array("animation").isEmpty {
      self.contextMenu { PropertyActions(store: store, property: property) }
    }
    else {
      self
    }
  }
}

/** A property as a form row: its label and its control. */
struct PropertyRow: View {
  @ObservedObject var store: InspectorStore
  let property: JSON

  private var type: String { property.string("type") ?? "" }
  private var label: String { property.string("label") ?? "" }

  var body: some View {
    Group {
      if property.has("slider"), type == "FLOAT" || type == "INT" {
        VStack(spacing: 2) {
          labeled { NumberField(store: store, property: property, scrubs: false, significant: 6) }
          PropertySlider(store: store, property: property)
        }
      }
      else {
        switch type {
          case "OPERATOR" where property.bool("checkbox"):
            labeled(width: nil) { OperatorToggle(store: store, property: property) }
          case "OPERATOR":
            OperatorButton(store: store, property: property)
          case "MENU":
            UIMenuButton(title: label, image: BlenderIcon.image(property.string("icon")),
                         enabled: property.bool("editable")) {
              store.menuProvider?(property as NSDictionary)
            }
            .frame(minHeight: 44)
          case "LABEL":
            Text(label)
              .font(.subheadline)
              .foregroundStyle(.secondary)
              .frame(maxWidth: .infinity, alignment: .leading)
          case "BOOLEAN":
            labeled(width: nil) { PropertyToggle(store: store, property: property) }
          case "COLOR":
            labeled(width: nil) { ColorSwatch(store: store, property: property) }
          case "ENUM":
            EnumControl(store: store, property: property)
          case "STRING":
            labeled { TextControl(store: store, property: property) }
          case "INT", "FLOAT":
            labeled { NumberField(store: store, property: property) }
          default:
            EmptyView()
        }
      }
    }
    .propertyActions(store, property)
  }

  @ViewBuilder
  private func labeled<Control: View>(width: CGFloat? = 0.53,
                                      @ViewBuilder control: () -> Control) -> some View
  {
    HStack(spacing: 10) {
      if let icon = BlenderIcon.image(property.string("leading_icon")) {
        Image(uiImage: icon)
          .resizable()
          .scaledToFit()
          .frame(width: 22, height: 22)
          .foregroundStyle(.secondary)
      }
      Text(label)
        .lineLimit(2)
        .frame(maxWidth: .infinity, alignment: .leading)
      if let lock = property.dict("lock"), lock.bool("value") {
        Image(systemName: "lock.fill")
          .font(.caption2)
          .foregroundStyle(.secondary)
          .accessibilityLabel("Locked")
      }
      if let width {
        control()
          .containerRelativeFrame(.horizontal) { length, _ in length * width }
      }
      else {
        control()
      }
    }
    .frame(minHeight: 44)
  }
}

struct PropertyToggle: View {
  @ObservedObject var store: InspectorStore
  let property: JSON

  var body: some View {
    Toggle(propertyLabel(property), isOn: Binding(
      get: { (store.value(property) as? NSNumber)?.boolValue ?? false },
      set: { store.edit(property, value: $0) }))
      .labelsHidden()
      .animationTint(property.string("anim_state"))
      .disabled(!property.bool("editable"))
  }
}

/** Blender draws some toggle operators as checkboxes (e.g. Dyntopo); a switch runs them. */
struct OperatorToggle: View {
  @ObservedObject var store: InspectorStore
  let property: JSON
  @State private var on: Bool?

  var body: some View {
    Toggle(property.string("label") ?? "", isOn: Binding(
      get: { on ?? property.bool("value") },
      set: { value in
        on = value
        store.runOperator(property)
        if value, let panel = property.string("physics_panel") { store.expandSection(panel) }
      }))
      .labelsHidden()
      .disabled(!property.bool("editable"))
      .onChange(of: property.bool("value")) { _, _ in on = nil }
  }
}

struct OperatorButton: View {
  @ObservedObject var store: InspectorStore
  let property: JSON
  var compact = false

  var body: some View {
    Button {
      store.runOperator(property)
    } label: {
      Label {
        Text(property.string("label") ?? "")
      } icon: {
        IconView(name: property.string("icon"))
      }
      .frame(maxWidth: compact ? nil : .infinity, alignment: .leading)
    }
    .buttonStyle(.borderless)
    .disabled(!property.bool("editable"))
    .frame(minHeight: compact ? 26 : 44)
  }
}

/** A header operator, such as a modifier's Remove: a compact icon beside the header switches. */
struct HeaderOperatorButton: View {
  @ObservedObject var store: InspectorStore
  let property: JSON

  var body: some View {
    Button {
      store.runOperator(property)
    } label: {
      if let image = BlenderIcon.image(property.string("icon")) {
        Image(uiImage: image)
          .resizable()
          .scaledToFit()
          .frame(width: 18, height: 18)
          .frame(width: 36, height: 36)
      }
      else {
        Text(property.string("label") ?? "")
      }
    }
    .buttonStyle(.borderless)
    .foregroundStyle(.secondary)
    .disabled(!property.bool("editable"))
    .accessibilityLabel(property.string("label") ?? "")
    .help(property.string("label") ?? "")
  }
}

struct TextControl: View {
  @ObservedObject var store: InspectorStore
  let property: JSON
  @State private var text = ""
  @FocusState private var focused: Bool

  var body: some View {
    TextField(property.string("label") ?? "", text: $text)
      .multilineTextAlignment(.trailing)
      .autocorrectionDisabled()
      .focused($focused)
      .submitLabel(.done)
      .padding(.horizontal, 8)
      .frame(minHeight: 36)
      .background(Color(uiColor: .tertiarySystemFill), in: RoundedRectangle(cornerRadius: 8))
      .disabled(!property.bool("editable"))
      .onAppear { text = store.value(property) as? String ?? "" }
      .onChange(of: store.value(property) as? String ?? "") { _, newValue in
        if !focused { text = newValue }
      }
      .onSubmit { commit() }
      .onChange(of: focused) { _, isFocused in if !isFocused { commit() } }
  }

  private func commit() {
    if text != store.value(property) as? String ?? "" { store.edit(property, value: text) }
  }
}

struct ColorSwatch: View {
  @ObservedObject var store: InspectorStore
  let property: JSON
  private let anchor = AnchorBox()

  var body: some View {
    let color = blenderColor(store.value(property))
    Button {
      guard property.bool("editable") else { return }
      ColorPickerSession(property: property, store: store).present(from: anchor, color: color)
    } label: {
      Circle()
        .fill(Color(uiColor: color))
        .overlay(Circle().strokeBorder(.separator, lineWidth: 0.5))
        .overlay {
          if let state = animationStateColor(property.string("anim_state")) {
            Circle().strokeBorder(state, lineWidth: 2.5)
          }
        }
        .frame(width: 32, height: 32)
        .frame(width: 44, height: 44)
        .background(PresentationAnchor(anchor: anchor))
    }
    .buttonStyle(.plain)
    .disabled(!property.bool("editable"))
    .accessibilityLabel(propertyLabel(property))
    .accessibilityHint("Opens the color picker.")
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Enums
 * \{ */

struct EnumControl: View {
  @ObservedObject var store: InspectorStore
  let property: JSON
  var compact = false

  private var options: [JSON] { property.array("options") }
  private var label: String { property.string("toolbar_label") ?? property.string("label") ?? "" }

  private func isChecked(_ option: JSON) -> Bool {
    if option.has("checked") { return option.bool("checked") }
    return jsonEqual(option["value"], store.value(property))
  }

  private var current: JSON? { options.first(where: isChecked) }

  var body: some View {
    if store.mode == .shading && property.string("property") == "type" {
      Picker(label, selection: Binding(
        get: { store.value(property) as? String ?? "" },
        set: { store.edit(property, value: $0) })) {
        ForEach(Array(options.enumerated()), id: \.offset) { _, option in
          if let image = BlenderIcon.image(option.string("icon")) {
            Image(uiImage: image).tag(option.string("value") ?? "")
              .accessibilityLabel(option.string("label") ?? "")
          }
          else {
            Text(option.string("label") ?? "").tag(option.string("value") ?? "")
          }
        }
      }
      .pickerStyle(.segmented)
      .disabled(!property.bool("editable"))
    }
    else if options.contains(where: { !($0.string("preview") ?? "").isEmpty }) {
      PreviewEnumButton(store: store, property: property)
    }
    else if compact || (store.selectedTab == "TOOL" && property.bool("expand") &&
                        (2...6).contains(options.count))
    {
      menu {
        HStack(spacing: 4) {
          if let image = BlenderIcon.image(current?.string("icon")) {
            Image(uiImage: image).resizable().scaledToFit().frame(width: 18, height: 18)
          }
          else {
            Text(current?.string("label") ?? "").lineLimit(1)
          }
          Image(systemName: "chevron.up.chevron.down").font(.caption2)
        }
        .padding(.horizontal, 8)
        .frame(minHeight: 26)
      }
      .buttonStyle(.glass)
      .accessibilityLabel(label)
    }
    else {
      HStack(spacing: 10) {
        Text(property.string("label") ?? "")
          .frame(maxWidth: .infinity, alignment: .leading)
        menu {
          HStack(spacing: 6) {
            if let image = BlenderIcon.image(current?.string("icon")) {
              Image(uiImage: image).resizable().scaledToFit().frame(width: 16, height: 16)
            }
            Text(current?.string("label") ?? "—")
              .lineLimit(1)
            Image(systemName: "chevron.up.chevron.down")
              .font(.caption2)
              .foregroundStyle(.secondary)
          }
          .frame(maxWidth: .infinity, alignment: .trailing)
        }
        .containerRelativeFrame(.horizontal) { length, _ in length * 0.53 }
      }
      .frame(minHeight: 44)
    }
  }

  private func menu<LabelView: View>(@ViewBuilder label: () -> LabelView) -> some View {
    Menu {
      ForEach(Array(options.enumerated()), id: \.offset) { _, option in
        Button {
          store.edit(property, value: option["value"] ?? "")
        } label: {
          Label {
            Text(option.string("label") ?? "")
          } icon: {
            if isChecked(option) { Image(systemName: "checkmark") }
            else if let image = BlenderIcon.image(option.string("icon")) { Image(uiImage: image) }
          }
        }
      }
    } label: {
      label()
    }
    .menuOrder(.fixed)
    .disabled(!property.bool("editable"))
  }
}

/** Blender shows these as a large preview (template_icon_view), not a list of names. */
struct PreviewEnumButton: View {
  @ObservedObject var store: InspectorStore
  let property: JSON
  @State private var showing = false

  var body: some View {
    let options = property.array("options")
    let current = options.first { jsonEqual($0["value"], store.value(property)) }
    Button {
      showing = true
    } label: {
      HStack(spacing: 12) {
        PreviewThumbnail(path: current?.string("preview"), size: 56)
        VStack(alignment: .leading) {
          Text(property.string("label") ?? "").font(.caption).foregroundStyle(.secondary)
          Text(current?.string("label") ?? (store.value(property) as? String ?? ""))
            .foregroundStyle(.primary)
        }
        Spacer()
        Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary)
      }
      .padding(.vertical, 4)
    }
    .buttonStyle(.plain)
    .disabled(!property.bool("editable"))
    .accessibilityHint("Shows all choices as previews.")
    .popover(isPresented: $showing) {
      PreviewGrid(title: property.string("label") ?? "", options: options,
                  value: store.value(property)) { option in
        store.edit(property, value: option["value"] ?? "")
        showing = false
      }
      .frame(minWidth: 400, minHeight: 440)
    }
  }
}

struct PreviewThumbnail: View {
  var path: String?
  var fallback: UIImage? = nil
  var size: CGFloat

  var body: some View {
    Group {
      if let image = PreviewCache.image(path) {
        Image(uiImage: image).resizable().scaledToFill()
      }
      else {
        ZStack {
          Color(uiColor: .tertiarySystemFill)
          if let fallback {
            Image(uiImage: fallback)
              .resizable()
              .scaledToFit()
              .padding(size * 0.26)
              .foregroundStyle(.secondary)
          }
        }
      }
    }
    .frame(width: size, height: size)
    .clipShape(RoundedRectangle(cornerRadius: size * 0.22, style: .continuous))
  }
}

struct PreviewGrid: View {
  let title: String
  let options: [JSON]
  let value: Any?
  let choose: (JSON) -> Void

  var body: some View {
    NavigationStack {
      ScrollView {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 84), spacing: 8)], spacing: 8) {
          ForEach(Array(options.enumerated()), id: \.offset) { _, option in
            let current = jsonEqual(option["value"], value)
            Button {
              choose(option)
            } label: {
              VStack(spacing: 4) {
                PreviewThumbnail(path: option.string("preview"), size: 80)
                Text(option.string("label") ?? "")
                  .font(.caption2)
                  .lineLimit(1)
                  .foregroundStyle(current ? Color.accentColor : .secondary)
              }
              .padding(4)
              .overlay(RoundedRectangle(cornerRadius: 12)
                .strokeBorder(current ? Color.accentColor : .clear, lineWidth: 2))
            }
            .buttonStyle(.plain)
            .accessibilityLabel(option.string("label") ?? "")
            .accessibilityAddTraits(current ? .isSelected : [])
          }
        }
        .padding(16)
      }
      .navigationTitle(title)
      .navigationBarTitleDisplayMode(.inline)
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Groups
 * \{ */

/** A vector such as Location: its caption over a field per component. */
struct VectorRow: View {
  @ObservedObject var store: InspectorStore
  let title: String
  let items: [JSON]

  var body: some View {
    VStack(alignment: .leading, spacing: 2) {
      caption
      HStack(spacing: 4) {
        ForEach(Array(items.enumerated()), id: \.offset) { _, item in
          HStack(spacing: 4) {
            Text(item.string("label") ?? "")
              .font(.caption.weight(.semibold))
              .foregroundStyle(axisColor(item.string("label")))
              .fixedSize()
            if let lock = item.dict("lock"), lock.bool("value") {
              Image(systemName: "lock.fill").font(.system(size: 9)).foregroundStyle(.secondary)
            }
            if item.string("type") == "BOOLEAN" {
              PropertyToggle(store: store, property: item)
            }
            else {
              NumberField(store: store, property: item, compact: true, significant: 6)
            }
          }
          .frame(maxWidth: .infinity)
          .propertyActions(store, item)
        }
      }
    }
    .padding(.vertical, 4)
  }

  @ViewBuilder
  private var caption: some View {
    if let mode = items.first?.dict("group_menu") {
      let options = mode.array("options")
      let current = options.first { jsonEqual($0["value"], mode["value"]) }
      Menu {
        ForEach(Array(options.enumerated()), id: \.offset) { _, option in
          Button {
            if !jsonEqual(option["value"], mode["value"]) { store.edit(mode, value: option["value"] ?? "") }
          } label: {
            if jsonEqual(option["value"], mode["value"]) {
              Label(option.string("label") ?? "", systemImage: "checkmark")
            }
            else {
              Text(option.string("label") ?? "")
            }
          }
        }
      } label: {
        HStack(spacing: 4) {
          Text(title).foregroundStyle(.secondary)
          Text(current?.string("label") ?? "").foregroundStyle(Color.accentColor)
          Image(systemName: "chevron.up.chevron.down").font(.system(size: 9))
            .foregroundStyle(Color.accentColor)
        }
        .font(.caption)
        .frame(minHeight: 28)
      }
      .disabled(!mode.bool("editable"))
      .accessibilityLabel("\(title) mode")
    }
    else {
      Text(title).font(.caption).foregroundStyle(.secondary)
    }
  }
}

/** Toggles shown together, such as Mirror X Y Z: tinted while on. */
struct ToggleGroupRow: View {
  @ObservedObject var store: InspectorStore
  let caption: String
  let items: [JSON]
  var compact = false

  var body: some View {
    let layout = compact ? AnyLayout(VStackLayout(spacing: 0)) : AnyLayout(HStackLayout(spacing: 10))
    layout {
      Text(caption)
        .font(compact ? .caption2 : .body)
        .foregroundStyle(compact ? .secondary : .primary)
        .frame(maxWidth: compact ? nil : .infinity, alignment: .leading)
      HStack(spacing: 2) {
        ForEach(Array(items.enumerated()), id: \.offset) { _, item in
          let on = (store.value(item) as? NSNumber)?.boolValue ?? false
          Button {
            store.edit(item, value: !on)
            Haptics.selection()
          } label: {
            Text(item.string("toolbar_label") ?? item.string("label") ?? "")
              .font(.system(size: compact ? 13 : 15, weight: .semibold, design: .monospaced))
              .foregroundStyle(on ? Color.white : .primary)
              .frame(width: compact ? 28 : 36, height: compact ? 22 : 30)
              .background(on ? Color.accentColor : .clear,
                          in: RoundedRectangle(cornerRadius: compact ? 7 : 8, style: .continuous))
          }
          .buttonStyle(.plain)
          .disabled(!item.bool("editable"))
          .accessibilityLabel("\(caption) \(item.string("label") ?? "")")
          .accessibilityAddTraits(on ? .isSelected : [])
        }
      }
      .padding(2)
      .background(Color(uiColor: .tertiarySystemFill),
                  in: RoundedRectangle(cornerRadius: compact ? 9 : 10, style: .continuous))
    }
    .frame(minHeight: compact ? 40 : 44)
  }
}

/** \} */
