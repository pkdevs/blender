/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Pieces the interface shares that reach into UIKit: number scrubbing, which must arbitrate with
 * scrolling; the color picker, which needs explicit begin and end for one undo step per drag;
 * anchors for presenting UIKit controllers; haptics; and the glass, which visionOS draws its own
 * way.
 */

import SwiftUI
import UIKit

/* -------------------------------------------------------------------- */
/** \name Number scrubbing
 * \{ */

#if os(visionOS)
/** visionOS: a pinch and drag sideways. Only one value scrubs at a time. */
@MainActor
private final class ScrubTracker {
  static let shared = ScrubTracker()
  var active = false
}

struct ScrubGesture: Gesture {
  var enabled: Bool
  var anchor: AnchorBox? = nil
  var began: () -> Void
  var changed: (_ translation: CGPoint) -> Void
  var ended: (_ cancelled: Bool) -> Void

  var body: some Gesture {
    DragGesture(minimumDistance: 8, coordinateSpace: .global)
      .onChanged { value in
        let tracker = ScrubTracker.shared
        let translation = CGPoint(x: value.translation.width, y: value.translation.height)
        if !tracker.active {
          guard enabled, abs(translation.x) > abs(translation.y) * 1.2 else { return }
          tracker.active = true
          began()
        }
        changed(translation)
        if let view = anchor?.view { ScrubIndicator.shared.show(above: view, vertical: translation.y) }
      }
      .onEnded { value in
        let tracker = ScrubTracker.shared
        guard tracker.active else { return }
        tracker.active = false
        changed(CGPoint(x: value.translation.width, y: value.translation.height))
        ended(false)
        ScrubIndicator.shared.hide(for: anchor?.view)
      }
  }
}
#else
/** A horizontal drag that only begins when mostly sideways, so vertical drags still scroll. */
struct ScrubGesture: UIGestureRecognizerRepresentable {
  var enabled: Bool
  /** The field, which the sensitivity indicator floats above while scrubbing. */
  var anchor: AnchorBox? = nil
  var began: () -> Void
  var changed: (_ translation: CGPoint) -> Void
  var ended: (_ cancelled: Bool) -> Void

  func makeUIGestureRecognizer(context: Context) -> UIPanGestureRecognizer {
    let pan = UIPanGestureRecognizer()
    pan.maximumNumberOfTouches = 1
    pan.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue),
                             NSNumber(value: UITouch.TouchType.pencil.rawValue),
                             NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
    pan.delegate = context.coordinator
    return pan
  }

  func makeCoordinator(converter: CoordinateSpaceConverter) -> Coordinator { Coordinator() }

  func updateUIGestureRecognizer(_ recognizer: UIPanGestureRecognizer, context: Context) {
    context.coordinator.enabled = enabled
  }

  func handleUIGestureRecognizerAction(_ recognizer: UIPanGestureRecognizer, context: Context) {
    let translation = recognizer.translation(in: recognizer.view?.window)
    switch recognizer.state {
      case .began:
        began()
        if let view = anchor?.view { ScrubIndicator.shared.show(above: view, vertical: 0) }
      case .changed:
        changed(translation)
        if let view = anchor?.view { ScrubIndicator.shared.show(above: view, vertical: translation.y) }
      case .ended:
        changed(translation)
        ended(false)
        ScrubIndicator.shared.hide(for: anchor?.view)
      case .cancelled, .failed:
        ended(true)
        ScrubIndicator.shared.hide(for: anchor?.view)
      default: break
    }
  }

  final class Coordinator: NSObject, UIGestureRecognizerDelegate {
    var enabled = true

    func gestureRecognizerShouldBegin(_ recognizer: UIGestureRecognizer) -> Bool {
      guard enabled, let pan = recognizer as? UIPanGestureRecognizer else { return false }
      let velocity = pan.velocity(in: pan.view)
      return abs(velocity.x) > abs(velocity.y) * 1.2
    }

    func gestureRecognizer(_ recognizer: UIGestureRecognizer,
                           shouldBeRequiredToFailBy other: UIGestureRecognizer) -> Bool {
      /* The surrounding scroll view waits for a sideways drag to fail. */
      other.view is UIScrollView
    }
  }
}

#endif

/** Drag sensitivity: dragging further up or down while scrubbing makes changes coarser or finer. */
func scrubSensitivity(_ vertical: Double) -> Double {
  exp2(min(4.0, max(-4.0, -vertical / 80.0)))
}

@MainActor
final class ScrubIndicatorModel: ObservableObject {
  @Published var vertical = 0.0
  @Published var tint = Color.accentColor
}

struct ScrubIndicatorContent: View {
  @ObservedObject var model: ScrubIndicatorModel

  var body: some View {
    /* Same mapping as `scrubSensitivity`: up is faster, down is finer. */
    let octaves = min(4.0, max(-4.0, -model.vertical / 80.0))
    let factor = exp2(octaves)
    let normal = abs(octaves) < 0.15
    HStack(spacing: 8) {
      Image(systemName: "arrow.up.and.down")
        .font(.system(size: 12, weight: .semibold))
        .foregroundStyle(.secondary)
      Group {
        if abs(model.vertical) < 6 {
          Text("Up faster · Down finer")
        }
        else {
          Text((factor >= 1 ? String(format: "×%.3g", factor) : String(format: "×%.2g", factor)) + " " +
               (normal ? "Normal" : octaves > 0 ? "Faster" : "Finer"))
        }
      }
      .font(.system(size: 13, weight: .medium).monospacedDigit())
      .foregroundStyle(.primary)
      /* A small gauge: the dot rises as dragging gets coarser. */
      ZStack {
        Capsule().fill(Color(uiColor: .tertiarySystemFill)).frame(width: 4, height: 24)
        Rectangle().fill(Color(uiColor: .tertiaryLabel)).frame(width: 8, height: 1)
        Circle().fill(normal ? Color.secondary : model.tint).frame(width: 8, height: 8)
          .offset(y: -octaves / 4.0 * (24 - 8) / 2)
      }
      .frame(width: 8, height: 24)
    }
    .padding(.horizontal, 12)
    .padding(.vertical, 6)
    .fixedSize()
  }
}

/** A floating glass pill above a field while it is scrubbed. It tells the user that vertical
 * movement changes the drag speed, then shows the current multiplier on a small gauge. */
@MainActor
final class ScrubIndicator {
  static let shared = ScrubIndicator()

  private let model = ScrubIndicatorModel()
  private lazy var host = UIHostingController(rootView: ScrubIndicatorContent(model: model))
  private lazy var pill: UIVisualEffectView = {
    let pill = UIVisualEffectView(effect: makePanelGlass())
    pill.isUserInteractionEnabled = false
    pill.clipsToBounds = true
    pill.layer.cornerCurve = .continuous
    pill.accessibilityIdentifier = "blender.number.scrub-indicator"
    host.view.backgroundColor = .clear
    host.sizingOptions = []
    host.safeAreaRegions = []
    pill.contentView.addSubview(host.view)
    return pill
  }()
  private weak var field: UIView?

  func show(above field: UIView, vertical: Double) {
    guard let window = field.window else {
      hide(for: self.field)
      return
    }
    model.vertical = vertical
    model.tint = Color(uiColor: field.tintColor)
    let appearing = self.field !== field || pill.superview !== window
    self.field = field
    if pill.superview !== window { window.addSubview(pill) }
    window.bringSubviewToFront(pill)
    var size = host.sizeThatFits(in: CGSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude))
    size.width = max(size.width, 120)
    pill.layer.cornerRadius = size.height / 2
    host.view.frame = CGRect(origin: .zero, size: size)
    /* Sit above the field, clear of the hand holding the Pencil; flip below near the top. */
    let anchor = field.convert(field.bounds, to: window)
    let safe = window.safeAreaInsets
    var y = anchor.minY - 10 - size.height
    if y < safe.top + 4 { y = anchor.maxY + 10 }
    var x = anchor.midX - size.width / 2
    x = max(safe.left + 8, min(x, window.bounds.width - safe.right - 8 - size.width))
    if appearing {
      pill.frame = CGRect(x: x, y: y, width: size.width, height: size.height)
      pill.alpha = 0
      pill.transform = CGAffineTransform(scaleX: 0.9, y: 0.9)
      UIView.animate(withDuration: UIAccessibility.isReduceMotionEnabled ? 0 : 0.18) {
        self.pill.alpha = 1
        self.pill.transform = .identity
      }
    }
    else {
      pill.bounds = CGRect(origin: .zero, size: size)
      pill.center = CGPoint(x: x + size.width / 2, y: y + size.height / 2)
    }
  }

  func hide(for field: UIView?) {
    guard let field, self.field === field else { return }
    self.field = nil
    UIView.animate(withDuration: UIAccessibility.isReduceMotionEnabled ? 0 : 0.15) {
      self.pill.alpha = 0
    } completion: { _ in
      if self.field == nil { self.pill.removeFromSuperview() }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Segmented control
 * \{ */

/**
 * The system's segmented control, for choices Blender draws as a row. It moves its Liquid Glass
 * selection over at once when tapped; Blender's value only arrives a moment later, so the control
 * keeps the choice it made until Blender shows it, instead of jumping back meanwhile.
 */
struct SegmentedControl: View {
  struct Segment: Equatable {
    var value: String
    var title: String
    var image: UIImage?
  }

  var segments: [Segment]
  var selection: String
  var enabled = true
  var accessibilityLabel: String? = nil
  var choose: (String) -> Void

  /** The value chosen here that Blender does not show yet, and until when to wait for it. */
  @State private var pending: String?
  @State private var pendingUntil = Date.distantPast

  private var shown: String {
    if let pending, pending != selection, Date() < pendingUntil { return pending }
    return selection
  }

  var body: some View {
    Picker(accessibilityLabel ?? "", selection: Binding(get: { shown }, set: { value in
      pending = value
      pendingUntil = Date().addingTimeInterval(1.0)
      choose(value)
    })) {
      ForEach(segments, id: \.value) { segment in
        Group {
          if let image = segment.image {
            /* At a symbol's size, as Blender's icons are larger artwork. */
            Image(uiImage: image)
              .renderingMode(image.renderingMode == .alwaysOriginal ? .original : .template)
              .resizable()
              .scaledToFit()
              .frame(width: 18, height: 18)
              .accessibilityLabel(segment.title)
          }
          else {
            Text(segment.title)
          }
        }
        .tag(segment.value)
      }
    }
    .pickerStyle(.segmented)
    .labelsHidden()
    .disabled(!enabled)
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Presenting UIKit controllers
 * \{ */

/** Captures the UIKit view a SwiftUI control sits in, to anchor popovers on it. */
struct PresentationAnchor: UIViewRepresentable {
  let anchor: AnchorBox

  func makeUIView(context: Context) -> UIView {
    let view = UIView()
    view.isUserInteractionEnabled = false
    view.backgroundColor = .clear
    anchor.view = view
    return view
  }

  func updateUIView(_ view: UIView, context: Context) { anchor.view = view }
}

final class AnchorBox {
  weak var view: UIView?

  var presenter: UIViewController? {
    var presenter = view?.window?.rootViewController
    while let next = presenter?.presentedViewController, !next.isBeingDismissed { presenter = next }
    return presenter
  }

  func present(_ controller: UIViewController, preferredSize: CGSize? = nil) {
    guard let presenter, let view else { return }
    controller.modalPresentationStyle = .popover
    if let preferredSize { controller.preferredContentSize = preferredSize }
    controller.popoverPresentationController?.sourceView = view
    controller.popoverPresentationController?.sourceRect = view.bounds
    presenter.present(controller, animated: true)
  }
}

/** The system color picker, with Blender's interaction phases: continuous changes preview, and
 * the picker closing or a discrete choice commits one undo step. */
@MainActor
final class ColorPickerSession: NSObject, UIColorPickerViewControllerDelegate,
  UIPopoverPresentationControllerDelegate, UIAdaptivePresentationControllerDelegate
{
  static var active: ColorPickerSession?

  let property: JSON
  let alpha: Bool
  weak var store: InspectorStore?
  private var interaction: String?
  private var lastPreview: CFTimeInterval = 0
  private var pending: [Double]?
  private let picker = UIColorPickerViewController()

  init(property: JSON, store: InspectorStore) {
    self.property = property
    self.alpha = property.bool("alpha")
    self.store = store
  }

  func present(from anchor: AnchorBox, color: UIColor) {
    ColorPickerSession.active?.close()
    picker.title = property.string("label")
    picker.supportsAlpha = alpha
    picker.maximumLinearExposure = max(1, property.double("exposure") ?? 1)
    picker.selectedColor = color
    picker.delegate = self
    anchor.present(picker)
    picker.popoverPresentationController?.delegate = self
    picker.presentationController?.delegate = self
    ColorPickerSession.active = self
  }

  func colorPickerViewController(_ picker: UIColorPickerViewController, didSelect color: UIColor,
                                 continuously: Bool)
  {
    guard let components = colorComponents(color, alpha: alpha), let store else { return }
    pending = components
    if !continuously {
      commit()
      return
    }
    if interaction == nil { interaction = UUID().uuidString }
    if CACurrentMediaTime() - lastPreview >= 1.0 / 60.0 {
      store.preview(property, value: components, interaction: interaction!, phase: "update")
      lastPreview = CACurrentMediaTime()
    }
  }

  private func commit() {
    guard let components = pending, let store else { return }
    pending = nil
    if let interaction {
      store.preview(property, value: components, interaction: interaction, phase: "end")
      self.interaction = nil
    }
    else {
      store.edit(property, value: components)
    }
  }

  func close() {
    commit()
    picker.delegate = nil
    picker.dismiss(animated: true)
    if ColorPickerSession.active === self { ColorPickerSession.active = nil }
  }

  func colorPickerViewControllerDidFinish(_ picker: UIColorPickerViewController) { close() }

  func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
    close()
  }
}

func blenderColor(_ value: Any?) -> UIColor {
  guard let components = value as? [NSNumber], components.count == 3 || components.count == 4 else {
    return .clear
  }
  var values = components.map { CGFloat($0.doubleValue) }
  if values.count == 3 { values.append(1) }
  guard let space = CGColorSpace(name: CGColorSpace.extendedSRGB),
        let color = CGColor(colorSpace: space, components: values)
  else { return .clear }
  return UIColor(cgColor: color)
}

func colorComponents(_ color: UIColor, alpha: Bool) -> [Double]? {
  guard let space = CGColorSpace(name: CGColorSpace.extendedSRGB),
        let converted = color.cgColor.converted(to: space, intent: .defaultIntent, options: nil),
        let values = converted.components, values.count == 4
  else { return nil }
  let result = values.map { Double($0) }
  return alpha ? result : Array(result.prefix(3))
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Haptics
 * \{ */

/** Haptics where the device has them; Apple Vision Pro has none. */
enum Haptics {
  enum Impact { case light, medium }

  static func selection() {
#if !os(visionOS)
    UISelectionFeedbackGenerator().selectionChanged()
#endif
  }

  static func impact(_ style: Impact = .light) {
#if !os(visionOS)
    UIImpactFeedbackGenerator(style: style == .medium ? .medium : .light).impactOccurred()
#endif
  }

  static func success() {
#if !os(visionOS)
    UINotificationFeedbackGenerator().notificationOccurred(.success)
#endif
  }

  /** Something could not be done. */
  static func error() {
#if !os(visionOS)
    UINotificationFeedbackGenerator().notificationOccurred(.error)
#endif
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glass
 *
 * Liquid Glass on iOS. visionOS windows are glass already: panels there use the system's glass
 * material, without tints.
 * \{ */

enum PanelGlass {
  case regular
  case clear
  case tinted(Color)
  /** No glass, keeping the view's identity so changes animate. */
  case none

#if !os(visionOS)
  func liquid(interactive: Bool) -> Glass {
    let glass: Glass
    switch self {
      case .regular: glass = .regular
      case .clear: glass = .clear
      case .tinted(let color): glass = .regular.tint(color)
      case .none: return .identity
    }
    return interactive ? glass.interactive() : glass
  }
#endif
}

extension View {
  @ViewBuilder
  func panelGlass<S: Shape>(_ glass: PanelGlass = .regular, interactive: Bool = false, in shape: S) -> some View {
#if os(visionOS)
    switch glass {
      case .tinted(let color): self.background(color, in: shape)
      case .regular, .clear: self.background(.thinMaterial, in: shape)
      case .none: self
    }
#else
    /* One effect whose value changes, so iOS morphs between tints. */
    self.glassEffect(glass.liquid(interactive: interactive), in: shape)
#endif
  }
}

extension View {
  /** Glass that morphs into other glass of the same `id` in a `PanelGlassGroup`. */
  @ViewBuilder
  func panelGlassID(_ id: String, in namespace: Namespace.ID) -> some View {
#if os(visionOS)
    self
#else
    self.glassEffectID(id, in: namespace)
#endif
  }
}

/** Glass shapes that blend and morph into each other as they come and go. */
struct PanelGlassGroup<Content: View>: View {
  var spacing: CGFloat = 8
  @ViewBuilder let content: Content

  var body: some View {
#if os(visionOS)
    content
#else
    GlassEffectContainer(spacing: spacing) { content }
#endif
  }
}

extension View {
  /** A glass button; visionOS draws bordered buttons on glass. */
  @ViewBuilder
  func glassButtonStyle() -> some View {
#if os(visionOS)
    self.buttonStyle(.bordered)
#else
    self.buttonStyle(.glass)
#endif
  }

  /** Scrolling puts the keyboard away; visionOS has no keyboard over the content to put away. */
  @ViewBuilder
  func dismissesKeyboardOnScroll(interactively: Bool) -> some View {
#if os(visionOS)
    self
#else
    self.scrollDismissesKeyboard(interactively ? .interactively : .immediately)
#endif
  }

  /** Controls above the keyboard while a field is edited. visionOS's keyboard floats apart from
   * the window, with no bar of its own. */
  @ViewBuilder
  func keyboardAccessory<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
#if os(visionOS)
    self
#else
    self.toolbar { ToolbarItemGroup(placement: .keyboard) { content() } }
#endif
  }

  /** A soft scroll edge, where the platform has one. */
  @ViewBuilder
  func softHorizontalScrollEdge() -> some View {
#if os(visionOS)
    self
#else
    self.scrollEdgeEffectStyle(.soft, for: .horizontal)
#endif
  }
}

/** Glass for a UIKit view, as SwiftUI's `panelGlass` gives a SwiftUI one. */
func makePanelGlass() -> UIVisualEffect {
#if os(visionOS)
  return UIBlurEffect(style: .systemThinMaterial)
#else
  return UIGlassEffect(style: .regular)
#endif
}

/** \} */
