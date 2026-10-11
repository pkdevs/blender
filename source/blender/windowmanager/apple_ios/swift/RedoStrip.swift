/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Adjust Last Operation: the last operator's settings on a glass strip above the bottom edge,
 * folded into a pill beside the viewport actions until opened. Answers the `WMIOSRedoStrip`
 * protocol's messages; the view controller finds it by name, see `GHOST_WindowIOS.mm`. The strip's
 * glass and its morph from the pill stay in UIKit, which also places the pill; SwiftUI draws the
 * controls.
 */

import SwiftUI
import UIKit

/* -------------------------------------------------------------------- */
/** \name State
 * \{ */

@MainActor
final class RedoStore: ObservableObject {
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

/* -------------------------------------------------------------------- */
/** \name Strip
 * \{ */

@MainActor
@objc(WMIOSSwiftRedoStrip)
public final class SwiftRedoStrip: NSObject {
  private static let height: CGFloat = 60
  private static let radius: CGFloat = 20
  static let shortNames = [
    "TRANSFORM_OT_translate": "Move", "TRANSFORM_OT_rotate": "Rotate",
    "TRANSFORM_OT_resize": "Scale", "MESH_OT_extrude_region_move": "Extrude",
    "MESH_OT_extrude_context_move": "Extrude", "MESH_OT_extrude_faces_move": "Extrude",
    "MESH_OT_duplicate_move": "Duplicate", "OBJECT_OT_duplicate_move": "Duplicate",
    "OBJECT_OT_duplicate_move_linked": "Duplicate Linked",
  ]

  private weak var overlay: UIView?
  private let store = RedoStore()
  private let rail: UIVisualEffectView
  private let host: UIHostingController<RedoContent>
  @objc public let collapsedButton = UIButton(type: .system)
  @objc public var presentationDidChange: ((Bool) -> Void)?

  private var expandedID: String?
  private var visible = false
  private var morphing = false
  private var pillShown = false
  private var transition = 0
  private var pillTransition = 0
  private var layoutRect = CGRect.zero
  private var layoutCenterX: CGFloat = 0
  private var lastError: String?

  @objc(initWithOverlay:icon:)
  public init(overlay: UIView, icon: @escaping (String) -> UIImage?) {
    self.overlay = overlay
    rail = UIVisualEffectView(effect: makePanelGlass(interactive: true))
    host = UIHostingController(rootView: RedoContent(store: store))
    super.init()
    store.icon = icon
    store.close = { [weak self] in self?.dismiss() }
    store.contentWidthChanged = { [weak self] in
      guard let self, !self.morphing else { return }
      self.applyLayout()
    }
    rail.layer.cornerRadius = SwiftRedoStrip.radius
    rail.layer.cornerCurve = .continuous
    rail.clipsToBounds = true
    rail.isHidden = true
    rail.accessibilityIdentifier = "blender.redo.strip"
    rail.tintColor = .systemBlue
    host.view.backgroundColor = .clear
    host.sizingOptions = []
    host.safeAreaRegions = []
    rail.contentView.addSubview(host.view)
    overlay.addSubview(rail)

    collapsedButton.configuration = .panelGlass()
    collapsedButton.titleLabel?.lineBreakMode = .byTruncatingTail
    collapsedButton.tintColor = .label
    collapsedButton.accessibilityIdentifier = "blender.redo.collapsed"
    collapsedButton.isHidden = true
    collapsedButton.addTarget(self, action: #selector(expand), for: .touchUpInside)
  }

  @objc public var view: UIView { rail }

  @objc(isExpanded) public var expanded: Bool {
    expandedID != nil && store.operation == expandedID && !store.properties.isEmpty
  }

  @objc public var collapsedWidth: CGFloat {
    if store.operation == expandedID || store.properties.isEmpty { return 0 }
    return min(136, max(72, collapsedButton.sizeThatFits(CGSize(width: CGFloat.greatestFiniteMagnitude, height: 44)).width))
  }

  @objc public func collapse() {
    if expanded { dismiss() }
  }

  @objc(layoutInRect:)
  public func layout(in rect: CGRect) {
    layout(in: rect, centerX: rect.midX)
  }

  @objc(layoutInRect:centerX:)
  public func layout(in rect: CGRect, centerX: CGFloat) {
    layoutRect = rect
    layoutCenterX = centerX
    /* A morph owns the rail's geometry; its completion applies the latest rect. */
    if !morphing { applyLayout() }
  }

  private func targetFrame() -> CGRect {
    /* Hug the controls, centered on the viewport but never outside the free lane. */
    /* The close button and its margin lead the controls. */
    let width = min(min(1000, max(0, layoutRect.width)), ceil(store.contentWidth) + 48)
    let x = min(max(layoutCenterX - width / 2, layoutRect.minX), layoutRect.maxX - width)
    return CGRect(x: x, y: layoutRect.maxY - SwiftRedoStrip.height, width: width, height: SwiftRedoStrip.height)
  }

  private func applyLayout() {
    let frame = targetFrame()
    /* Bounds and center stay valid while the appearance transform is applied. */
    rail.bounds = CGRect(origin: .zero, size: frame.size)
    rail.center = CGPoint(x: frame.midX, y: frame.midY)
    rail.layer.cornerRadius = SwiftRedoStrip.radius
    host.view.frame = rail.contentView.bounds
  }

  private var animates: Bool { !UIAccessibility.isReduceMotionEnabled && UIView.areAnimationsEnabled }

  private func pillFrame() -> CGRect {
    guard collapsedButton.window != nil, let overlay, overlay.window != nil else { return .null }
    let frame = collapsedButton.convert(collapsedButton.bounds, to: overlay)
    return frame.width > 1 && frame.height > 1 ? frame : .null
  }

  private func dismiss() {
    rail.endEditing(true)
    expandedID = nil
    let pill = collapsedWidth > 0
    /* The host reserves the pill's slot first, so its final frame is the morph target. */
    presentationDidChange?(true)
    setVisible(false, morph: pill ? pillFrame() : .null)
    if !morphing { setPillShown(pill, animated: true) }
  }

  @objc private func expand() {
    guard collapsedWidth > 0 else { return }
    /* Capture the pill before the host releases its slot. */
    let source = pillFrame()
    expandedID = store.operation
    setPillShown(false, animated: source.isNull)
    presentationDidChange?(true)
    setVisible(true, morph: source)
  }

  private func setPillShown(_ shown: Bool, animated: Bool) {
    let pill = collapsedButton
    if pillShown == shown && pill.isHidden == !shown { return }
    pillShown = shown
    pillTransition += 1
    let current = pillTransition
    if !animated || !animates {
      pill.layer.removeAllAnimations()
      pill.isHidden = !shown
      pill.alpha = 1
      pill.transform = .identity
      return
    }
    if shown && pill.isHidden {
      pill.isHidden = false
      pill.alpha = 0
      pill.transform = CGAffineTransform(scaleX: 0.8, y: 0.8)
    }
    UIView.animate(withDuration: shown ? 0.38 : 0.16, delay: 0, usingSpringWithDamping: shown ? 0.78 : 1,
                   initialSpringVelocity: 0, options: [.beginFromCurrentState, .allowUserInteraction])
    {
      pill.alpha = shown ? 1 : 0
      pill.transform = shown ? .identity : CGAffineTransform(scaleX: 0.8, y: 0.8)
    } completion: { _ in
      if self.pillTransition != current || shown { return }
      pill.isHidden = true
      pill.alpha = 1
      pill.transform = .identity
    }
  }

  private func updateCollapsedTitle() {
    let snapshot = store.snapshot
    let title = SwiftRedoStrip.shortNames[snapshot?.string("operator") ?? ""] ?? snapshot?.string("title")
    var config = collapsedButton.configuration ?? .panelGlass()
    if config.title == title { return }
    config.title = title
    config.titleLineBreakMode = .byTruncatingTail
    config.cornerStyle = .capsule
    config.contentInsets = NSDirectionalEdgeInsets(top: 8, leading: 12, bottom: 8, trailing: 12)
    collapsedButton.configuration = config
    collapsedButton.titleLabel?.numberOfLines = 1
    collapsedButton.accessibilityLabel = "Adjust \(snapshot?.string("title") ?? "")"
    collapsedButton.accessibilityHint = "Show last operation controls"
    collapsedButton.toolTip = collapsedButton.accessibilityLabel
  }

  private func finishTransition(_ current: Int) {
    guard transition == current else { return }
    morphing = false
    rail.transform = .identity
    rail.isHidden = !visible
    rail.alpha = visible ? 1 : 0
    host.view.alpha = 1
    applyLayout()
  }

  /** With a pill frame the strip grows out of, or folds back into, the collapsed pill;
   * otherwise it rises and fades in place. */
  private func setVisible(_ shown: Bool, morph pill: CGRect = .null) {
    if visible == shown { return }
    visible = shown
    transition += 1
    let current = transition
    rail.isUserInteractionEnabled = shown
    if !shown { rail.endEditing(true) }
    rail.layer.removeAllAnimations()
    host.view.layer.removeAllAnimations()
    if !animates {
      finishTransition(current)
      return
    }
    let wasHidden = rail.isHidden
    morphing = false
    rail.transform = .identity
    applyLayout()
    let target = rail.frame
    let options: UIView.AnimationOptions = [.beginFromCurrentState, .allowUserInteraction]
    if pill.isNull || target.width < 1 {
      if shown && wasHidden {
        rail.alpha = 0
        rail.transform = CGAffineTransform(translationX: 0, y: 12)
      }
      rail.isHidden = false
      UIView.animate(withDuration: shown ? 0.4 : 0.22, delay: 0, usingSpringWithDamping: shown ? 0.84 : 1,
                     initialSpringVelocity: 0, options: options)
      {
        self.rail.alpha = shown ? 1 : 0
        self.rail.transform = shown ? .identity : CGAffineTransform(translationX: 0, y: 8)
      } completion: { _ in self.finishTransition(current) }
      return
    }
    morphing = true
    rail.isHidden = false
    rail.alpha = 1
    if shown {
      rail.frame = pill
      rail.layer.cornerRadius = pill.height / 2
      host.view.alpha = 0
    }
    UIView.animate(withDuration: shown ? 0.5 : 0.42, delay: 0, usingSpringWithDamping: shown ? 0.8 : 0.9,
                   initialSpringVelocity: 0, options: options)
    {
      self.rail.frame = shown ? target : pill
      self.rail.layer.cornerRadius = shown ? SwiftRedoStrip.radius : pill.height / 2
    } completion: { _ in self.finishTransition(current) }
    UIView.animate(withDuration: shown ? 0.26 : 0.14, delay: shown ? 0.1 : 0, options: options) {
      self.host.view.alpha = shown ? 1 : 0
    }
    if !shown {
      /* Cross-fade into the real pill as the strip lands on it. */
      UIView.animate(withDuration: 0.18, delay: 0.2, options: options) { self.rail.alpha = 0 }
      setPillShown(true, animated: false)
      collapsedButton.alpha = 0
      UIView.animate(withDuration: 0.18, delay: 0.2, options: options) { self.collapsedButton.alpha = 1 }
    }
  }

  @objc(updateSnapshot:)
  public func updateSnapshot(_ snapshot: Any?) {
    let previousWidth = collapsedWidth
    /* No operation to adjust arrives as null, which is NSNull here. */
    guard let next = snapshot as? JSON else {
      store.update(nil)
      setVisible(false)
      setPillShown(false, animated: true)
      if previousWidth > 0 { presentationDidChange?(true) }
      return
    }
    if next.string("id") != store.operation {
      /* Expansion belongs only to the operation the user explicitly opened. */
      if expandedID != next.string("id") { expandedID = nil }
      rail.endEditing(true)
    }
    store.update(next)
    updateCollapsedTitle()
    if !morphing { applyLayout() }
    let width = collapsedWidth
    setPillShown(width > 0, animated: true)
    setVisible(width == 0 && !store.properties.isEmpty)
    if previousWidth != width { presentationDidChange?(true) }
    if let error = next.string("error"), !error.isEmpty {
      let identity = "\(next["id"] ?? ""):\(next["ack"] ?? ""):\(error)"
      if let controller = overlay?.window?.rootViewController, lastError != identity,
         controller.presentedViewController == nil
      {
        lastError = identity
        let alert = UIAlertController(title: "Unable to Adjust Operation", message: error, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        controller.present(alert, animated: true)
      }
    }
  }
}

/** \} */
