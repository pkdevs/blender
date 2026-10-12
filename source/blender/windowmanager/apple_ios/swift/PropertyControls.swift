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

/** Numbers as Blender's native fields draw them: 15 point digits that scale with the text size. */
private let numberFont = Font(UIFontMetrics(forTextStyle: .body)
  .scaledFont(for: .monospacedDigitSystemFont(ofSize: 15, weight: .regular)))

/** A number that scrubs with a sideways drag (drag up for coarser, down for finer steps) and
 * takes a typed value on tap, with a stepper above the keyboard. */
struct NumberField: View, Equatable {
  let store: InspectorStore
  let property: JSON
  var compact = false
  /** Taller than the compact size, as in a vector. */
  var height: CGFloat?
  /** Sliders scrub with their track instead. */
  var scrubs = true
  var significant = 9

  /** A snapshot that leaves the property as it was leaves the field alone. */
  static func == (a: NumberField, b: NumberField) -> Bool {
    a.store === b.store && a.compact == b.compact && a.height == b.height && a.scrubs == b.scrubs &&
      a.significant == b.significant && jsonDictEqual(a.property, b.property)
  }

  @State private var editing = false
  @State private var text = ""
  @State private var scrubValue: Double?
  @State private var scrubStart = 0.0
  @State private var lastTranslation = 0.0
  @State private var interaction = ""
  @State private var lastPreview: CFTimeInterval = 0
  @State private var stepperValue = 0.0
  /** The field's UIKit view, for the drag sensitivity indicator. */
  @State private var scrubAnchor = AnchorBox()
  @FocusState private var focused: Bool

  private var editable: Bool { property.bool("editable") }
  private var value: Double { scrubValue ?? numericValue(store.value(property)) }

  var body: some View {
    ZStack(alignment: .trailing) {
      if editing {
        TextField("", text: $text)
          .keyboardType(.numbersAndPunctuation)
          .multilineTextAlignment(.trailing)
          .font(numberFont)
          .focused($focused)
          .submitLabel(.done)
          .onSubmit { commit() }
          .keyboardAccessory {
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
      else {
        Text(formatNumber(value, property: property, significant: significant))
          .font(numberFont)
          /* New values just show, even when rows around them animate. */
          .contentTransition(.identity)
          .lineLimit(1)
          .minimumScaleFactor(0.7)
          .foregroundStyle(editable ? .primary : .tertiary)
          .frame(maxWidth: .infinity, alignment: .trailing)
      }
    }
    .padding(.horizontal, 8)
    .frame(minHeight: height ?? (compact ? 30 : 40))
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
    .background(PresentationAnchor(anchor: scrubAnchor))
    .gesture(ScrubGesture(enabled: scrubs && editable && !editing, anchor: scrubAnchor,
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
    return PanelStyle.field
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

struct PropertySlider: View, Equatable {
  let store: InspectorStore
  let property: JSON
  @State private var dragging: Double?
  @State private var start = 0.0
  @State private var interaction = ""
  @State private var lastPreview: CFTimeInterval = 0

  static func == (a: PropertySlider, b: PropertySlider) -> Bool {
    a.store === b.store && jsonDictEqual(a.property, b.property)
  }

  var body: some View {
    let scale = SliderScale(property)
    let integer = property.string("type") == "INT"
    /* Read here, so the slider follows the value it shows. */
    let fraction = dragging ?? scale.fraction(numericValue(store.value(property)))
    Slider(value: Binding(
      get: { fraction },
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
  /** Blender's animation color, or the panels' blue when the property isn't animated. */
  func animationTint(_ state: String?) -> some View {
    self.tint(animationStateColor(state) ?? PanelStyle.tint)
  }

  /** Blender's animation color, or the system's green for a switch that isn't animated. */
  func switchTint(_ state: String?) -> some View {
    self.tint(animationStateColor(state) ?? Color(uiColor: .systemGreen))
  }
}

/** Lock and keyframe actions for a property, shown when touching and holding it. */
struct PropertyActions: View {
  let store: InspectorStore
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

/** A label beside a control given a fraction of the row's width, as Blender's split rows. */
struct FractionRow: Layout {
  var fraction: CGFloat
  var spacing: CGFloat = 10

  private func widths(_ width: CGFloat) -> (lead: CGFloat, control: CGFloat) {
    let control = (width * fraction).rounded()
    return (max(0, width - control - spacing), control)
  }

  func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
    guard subviews.count == 2 else { return .zero }
    let width = proposal.width ?? 320
    let (lead, control) = widths(width)
    let height = max(subviews[0].sizeThatFits(ProposedViewSize(width: lead, height: proposal.height)).height,
                     subviews[1].sizeThatFits(ProposedViewSize(width: control, height: proposal.height)).height)
    return CGSize(width: width, height: height)
  }

  func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
    guard subviews.count == 2 else { return }
    let (lead, control) = widths(bounds.width)
    subviews[0].place(at: CGPoint(x: bounds.minX, y: bounds.midY), anchor: .leading,
                      proposal: ProposedViewSize(width: lead, height: bounds.height))
    subviews[1].place(at: CGPoint(x: bounds.maxX, y: bounds.midY), anchor: .trailing,
                      proposal: ProposedViewSize(width: control, height: nil))
  }
}

/** A property as a form row: its label and its control. */
struct PropertyRow: View, Equatable {
  let store: InspectorStore
  let property: JSON

  /** A snapshot that leaves the property as it was leaves the row alone. */
  static func == (a: PropertyRow, b: PropertyRow) -> Bool {
    a.store === b.store && jsonDictEqual(a.property, b.property)
  }

  private var type: String { property.string("type") ?? "" }
  private var label: String { property.string("label") ?? "" }

  var body: some View {
    Group {
      if property.has("slider"), type == "FLOAT" || type == "INT" {
        VStack(spacing: 2) {
          labeled { NumberField(store: store, property: property, scrubs: false, significant: 6).equatable() }
          PropertySlider(store: store, property: property)
            .equatable()
        }
      }
      else {
        switch type {
          case "OPERATOR" where property.bool("checkbox"):
            labeled(width: nil) { OperatorToggle(store: store, property: property) }
          case "OPERATOR":
            OperatorButton(store: store, property: property)
          case "MENU":
            BlenderMenuButton(property: property)
              .frame(maxWidth: .infinity, minHeight: 44)
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
            labeled { NumberField(store: store, property: property).equatable() }
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
    let title = HStack(spacing: 10) {
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
    }
    Group {
      if let width {
        FractionRow(fraction: width) {
          title
          control()
        }
      }
      else {
        HStack(spacing: 10) {
          title
          control()
        }
      }
    }
    .frame(minHeight: 44)
  }
}

struct PropertyToggle: View, Equatable {
  let store: InspectorStore
  let property: JSON

  static func == (a: PropertyToggle, b: PropertyToggle) -> Bool {
    a.store === b.store && jsonDictEqual(a.property, b.property)
  }

  var body: some View {
    /* Read here, so the switch follows the value it shows. */
    let on = (store.value(property) as? NSNumber)?.boolValue ?? false
    Toggle(propertyLabel(property), isOn: Binding(
      get: { on },
      set: { store.edit(property, value: $0) }))
      .labelsHidden()
      .switchTint(property.string("anim_state"))
      .disabled(!property.bool("editable"))
  }
}

/** Blender draws some toggle operators as checkboxes (e.g. Dyntopo); a switch runs them. */
struct OperatorToggle: View {
  let store: InspectorStore
  let property: JSON
  @State private var on: Bool?

  var body: some View {
    let shown = on ?? property.bool("value")
    Toggle(property.string("label") ?? "", isOn: Binding(
      get: { shown },
      set: { value in
        on = value
        store.runOperator(property)
        if value, let panel = property.string("physics_panel") { store.expandSection(panel) }
      }))
      .labelsHidden()
      .switchTint(nil)
      .disabled(!property.bool("editable"))
      .onChange(of: property.bool("value")) { _, _ in on = nil }
  }
}

struct OperatorButton: View {
  let store: InspectorStore
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
  let store: InspectorStore
  let property: JSON

  var body: some View {
    Button {
      store.runOperator(property)
    } label: {
      if let image = BlenderIcon.image(property.string("icon")) {
        /* At the icon's own size, as a plain button draws it. */
        Image(uiImage: image)
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
  let store: InspectorStore
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
      .frame(minHeight: 40)
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
  let store: InspectorStore
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
  let store: InspectorStore
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
      /* Read here, so the picker follows the value it shows. */
      let selection = store.value(property) as? String ?? ""
      Picker(label, selection: Binding(
        get: { selection },
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
      /* As tall as UIKit makes it under the form's 40 point control minimum. */
      .controlSize(.large)
      .disabled(!property.bool("editable"))
    }
    else if options.contains(where: { !($0.string("preview") ?? "").isEmpty }) {
      PreviewEnumButton(store: store, property: property)
    }
    else if property.bool("expand") && (2...6).contains(options.count) {
      /* Choices Blender draws as a row (a brush's Add and Subtract, the selection modes): one tap
       * each, with the system's sliding selection. */
      if compact {
        segmented.controlSize(.mini)
      }
      else {
        FractionRow(fraction: 0.53) {
          Text(property.string("label") ?? "")
            .lineLimit(2)
            .frame(maxWidth: .infinity, alignment: .leading)
          segmented
        }
        .frame(minHeight: 44)
      }
    }
    else if compact {
      /* A glass button showing the current choice's icon, with the choices in a menu. */
      Menu {
        Picker(label, selection: chosen) { choices }
          .pickerStyle(.inline)
      } label: {
        if let icon = BlenderIcon.image(current?.string("icon")) {
          Image(uiImage: icon).resizable().scaledToFit().frame(width: 20, height: 20)
        }
        else {
          Text(current?.string("label") ?? "—").font(.caption).lineLimit(1)
        }
      }
      .frame(minWidth: 40, minHeight: 26)
      .padding(.horizontal, 6)
      .panelGlass(in: Capsule())
      .disabled(!property.bool("editable"))
      .accessibilityLabel(label)
    }
    else {
      /* The system's menu picker: the current choice in the tint, the choices in a menu. */
      FractionRow(fraction: 0.53) {
        Text(property.string("label") ?? "")
          .lineLimit(2)
          .frame(maxWidth: .infinity, alignment: .leading)
        Picker(label, selection: chosen) { choices }
          .pickerStyle(.menu)
          .labelsHidden()
          .disabled(!property.bool("editable"))
          .frame(maxWidth: .infinity, alignment: .trailing)
      }
      .frame(minHeight: 44)
    }
  }

  /** The choice by its position, as Blender's values are of any type. */
  private var chosen: Binding<Int> {
    Binding(get: { options.firstIndex(where: isChecked) ?? -1 }, set: { index in
      if options.indices.contains(index) { store.edit(property, value: options[index]["value"] ?? "") }
    })
  }

  private var choices: some View {
    ForEach(Array(options.enumerated()), id: \.offset) { index, option in
      Label {
        Text(option.string("label") ?? "")
      } icon: {
        if let image = BlenderIcon.image(option.string("icon")) { Image(uiImage: image) }
      }
      .tag(index)
    }
  }

  private var segmented: some View {
    /* Icons only when every choice has one, so none is left as a blank segment. */
    let icons = options.allSatisfy { BlenderIcon.image($0.string("icon")) != nil }
    let segments = options.map { option in
      SegmentedControl.Segment(value: option.string("value") ?? "",
                               title: option.string("label") ?? "",
                               image: icons ? BlenderIcon.image(option.string("icon")) : nil)
    }
    return SegmentedControl(segments: segments, selection: (current?["value"] as? String) ?? "",
                            enabled: property.bool("editable"), accessibilityLabel: label) {
      store.edit(property, value: $0)
    }
  }
}

/** Blender shows these as a large preview (template_icon_view), not a list of names. */
struct PreviewEnumButton: View {
  let store: InspectorStore
  let property: JSON
  @State private var showing = false

  var body: some View {
    let options = property.array("options")
    let current = options.first { jsonEqual($0["value"], store.value(property)) }
    Button {
      showing = true
    } label: {
      /* The current choice's preview and name, as a plain button. */
      HStack(spacing: 12) {
        PreviewThumbnail(path: current?.string("preview"), size: 56, cornerRadius: 8)
        Text(current?.string("label") ?? (store.value(property) as? String ?? ""))
          .foregroundStyle(.primary)
        Spacer(minLength: 0)
      }
      .padding(.vertical, 4)
      .frame(minHeight: 64)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .disabled(!property.bool("editable"))
    .accessibilityLabel(property.string("label") ?? "")
    .accessibilityValue(current?.string("label") ?? "")
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
  var cornerRadius: CGFloat? = nil

  var body: some View {
    Group {
      if let image = PreviewCache.image(path) {
        Image(uiImage: image).resizable().scaledToFill()
      }
      else {
        ZStack {
          PanelStyle.thumbnail
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
    .clipShape(RoundedRectangle(cornerRadius: cornerRadius ?? size * 0.22, style: .continuous))
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
                  .foregroundStyle(current ? PanelStyle.tint : .secondary)
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
struct VectorRow: View, Equatable {
  let store: InspectorStore
  let title: String
  let items: [JSON]

  /** A snapshot that leaves the components as they were leaves the row alone. */
  static func == (a: VectorRow, b: VectorRow) -> Bool {
    a.store === b.store && a.title == b.title && jsonListEqual(a.items, b.items)
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
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
                .equatable()
            }
            else {
              NumberField(store: store, property: item, compact: true, height: 40, significant: 6)
                .equatable()
            }
          }
          .frame(maxWidth: .infinity)
          .propertyActions(store, item)
        }
      }
      .frame(minHeight: 44)
    }
  }

  @ViewBuilder
  private var caption: some View {
    if let mode = items.first?.dict("group_menu") {
      let options = mode.array("options")
      let current = options.first { jsonEqual($0["value"], mode["value"]) }
      /* The mode is a touch and hold away, as in the caption's context menu. */
      Text(title)
        .font(.caption)
        .foregroundStyle(.secondary)
        .frame(minHeight: 28)
        .contentShape(Rectangle())
        .contextMenu {
          if mode.bool("editable") {
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
          }
        }
        .accessibilityValue(current?.string("label") ?? "")
        .accessibilityHint("Touch and hold to change the mode.")
    }
    else {
      Text(title).font(.caption).foregroundStyle(.secondary)
    }
  }
}

/** Toggles shown together, such as Mirror X Y Z: tinted while on. */
struct ToggleGroupRow: View, Equatable {
  let store: InspectorStore
  let caption: String
  let items: [JSON]
  var compact = false

  static func == (a: ToggleGroupRow, b: ToggleGroupRow) -> Bool {
    a.store === b.store && a.caption == b.caption && a.compact == b.compact && jsonListEqual(a.items, b.items)
  }

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
      .background(PanelStyle.field, in: RoundedRectangle(cornerRadius: compact ? 9 : 10, style: .continuous))
    }
    .frame(minHeight: compact ? 40 : 44)
  }
}

/** \} */
