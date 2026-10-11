/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The few UIKit pieces the SwiftUI interface keeps: Blender's menus arrive as `UIMenu` (built
 * lazily from Blender's registered menus), number scrubbing must arbitrate with scrolling, and
 * the color picker needs explicit begin and end for one undo step per drag.
 */

import SwiftUI
import UIKit

/* -------------------------------------------------------------------- */
/** \name Menus
 * \{ */

/** A button opening a `UIMenu`, rebuilt each time it opens so Blender's polling is current. */
struct UIMenuButton: UIViewRepresentable {
  var title: String?
  var image: UIImage?
  var subtitle: String? = nil
  var style: Style = .plain
  var enabled = true
  var accessibilityLabel: String? = nil
  /** The image and title color, rather than the inherited tint. */
  var tint: UIColor? = nil
  /** Shown above the menu's items, as Blender's menus name themselves. */
  var menuTitle = ""
  /** Built when it opens, so Blender's polling is current; otherwise used as it is, keeping its
   * options such as single selection. */
  var deferred = true
  var menu: () -> UIMenu?

  /** `glassPopup`: a compact glass button with the popup indicator, as for a tool's icon menu. */
  /** `system`: a classic system button, its title in the tint at 15 points, centered. */
  enum Style { case plain, glass, popup, borderless, glassPopup, system }

  /** Holds the latest provider, so a menu is set once and never replaced while it is open. */
  final class Coordinator {
    var provider: () -> UIMenu? = { nil }
    var signature: String?
  }

  func makeCoordinator() -> Coordinator { Coordinator() }

  func makeUIView(context: Context) -> UIButton {
    let button = UIButton(type: .system)
    button.showsMenuAsPrimaryAction = true
    button.contentHorizontalAlignment = .leading
    button.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    return button
  }

  /** Blender sends new state many times a second: only touch the menu when it changed. */
  private func updateMenu(_ button: UIButton, _ coordinator: Coordinator) {
    coordinator.provider = menu
    if deferred {
      guard button.menu == nil || coordinator.signature != menuTitle else { return }
      coordinator.signature = menuTitle
      /* A deferred element builds the menu when it opens. */
      button.menu = UIMenu(title: menuTitle, children: [UIDeferredMenuElement.uncached { [weak coordinator] completion in
        completion(coordinator?.provider()?.children ?? [])
      }])
      return
    }
    let built = menu() ?? UIMenu(children: [])
    let signature = menuSignature(built)
    guard signature != coordinator.signature else { return }
    coordinator.signature = signature
    button.menu = built
  }

  func updateUIView(_ button: UIButton, context: Context) {
    if style == .system {
      button.configuration = nil
      button.setTitle(title, for: .normal)
      button.setImage(image, for: .normal)
      button.titleLabel?.lineBreakMode = .byTruncatingTail
      button.contentHorizontalAlignment = .center
      button.isEnabled = enabled
      button.tintColor = tint
      button.accessibilityLabel = accessibilityLabel ?? title
      updateMenu(button, context.coordinator)
      return
    }
    var configuration: UIButton.Configuration
    switch style {
      case .glass, .glassPopup: configuration = .panelGlass()
      case .popup, .plain, .borderless, .system: configuration = .plain()
    }
    if style == .popup || style == .glassPopup { configuration.indicator = .popup }
    if style == .glassPopup {
      configuration.contentInsets = NSDirectionalEdgeInsets(top: 2, leading: 8, bottom: 2, trailing: 8)
      configuration.baseForegroundColor = .label
    }
    configuration.title = title
    configuration.subtitle = subtitle
    configuration.image = image
    configuration.imagePadding = 8
    configuration.titleLineBreakMode = .byTruncatingTail
    if let tint { configuration.baseForegroundColor = tint }
    if style == .borderless || style == .popup {
      configuration.contentInsets = NSDirectionalEdgeInsets(top: 4, leading: 0, bottom: 4, trailing: 0)
    }
    /* Reapplying an unchanged glass configuration redraws the glass, which blinks. */
    if button.configuration != configuration { button.configuration = configuration }
    button.isEnabled = enabled
    button.tintColor = tint
    button.contentHorizontalAlignment = style == .glassPopup || style == .plain ? .center : .leading
    button.accessibilityLabel = accessibilityLabel ?? title
    updateMenu(button, context.coordinator)
  }

  func sizeThatFits(_ proposal: ProposedViewSize, uiView: UIButton, context: Context) -> CGSize? {
    let fitting = uiView.systemLayoutSizeFitting(UIView.layoutFittingCompressedSize)
    if style == .glassPopup, let height = proposal.height {
      return CGSize(width: proposal.width ?? fitting.width, height: height)
    }
    return CGSize(width: proposal.width ?? fitting.width, height: max(fitting.height, 32))
  }
}

/** What a menu shows, to tell whether it changed. */
func menuSignature(_ element: UIMenuElement) -> String {
  if let menu = element as? UIMenu {
    return "[\(menu.title)|\(menu.options.rawValue):" + menu.children.map(menuSignature).joined(separator: ",") + "]"
  }
  if let action = element as? UIAction {
    return "\(action.title)|\(action.state.rawValue)|\(action.attributes.rawValue)"
  }
  return element.title
}

/** \} */

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
struct SegmentedControl: UIViewRepresentable {
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

  final class Coordinator: NSObject {
    var parent: SegmentedControl
    var segments: [Segment] = []
    /** The value chosen here that Blender does not show yet, and until when to wait for it. */
    var pending: String?
    var pendingUntil = Date.distantPast

    init(_ parent: SegmentedControl) {
      self.parent = parent
    }

    @objc func changed(_ control: UISegmentedControl) {
      let index = control.selectedSegmentIndex
      guard segments.indices.contains(index) else { return }
      pending = segments[index].value
      pendingUntil = Date().addingTimeInterval(1.0)
      parent.choose(segments[index].value)
    }
  }

  func makeCoordinator() -> Coordinator { Coordinator(self) }

  func makeUIView(context: Context) -> UISegmentedControl {
    let control = UISegmentedControl()
    control.addTarget(context.coordinator, action: #selector(Coordinator.changed(_:)),
                      for: .valueChanged)
    control.setContentHuggingPriority(.defaultLow, for: .horizontal)
    control.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    return control
  }

  func updateUIView(_ control: UISegmentedControl, context: Context) {
    let coordinator = context.coordinator
    coordinator.parent = self
    if coordinator.segments != segments {
      coordinator.segments = segments
      control.removeAllSegments()
      for (index, segment) in segments.enumerated() {
        if let icon = segment.image {
          /* A copy to name, as icons are shared. */
          let image = icon.withRenderingMode(icon.renderingMode)
          image.accessibilityLabel = segment.title
          control.insertSegment(with: image, at: index, animated: false)
        }
        else {
          control.insertSegment(withTitle: segment.title, at: index, animated: false)
        }
      }
    }
    control.isEnabled = enabled
    control.accessibilityLabel = accessibilityLabel
    /* Blender has caught up with the choice made here, or never will. */
    if let pending = coordinator.pending,
       pending == selection || Date() > coordinator.pendingUntil
    {
      coordinator.pending = nil
    }
    let shown = coordinator.pending ?? selection
    let index = segments.firstIndex { $0.value == shown } ?? UISegmentedControl.noSegment
    if control.selectedSegmentIndex != index {
      control.selectedSegmentIndex = index
    }
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

/** A UIKit panel's glass: `tint` lightens it in light mode on iOS. */
func makePanelGlass(tint: UIColor? = nil, interactive: Bool = false) -> UIVisualEffect {
#if os(visionOS)
  return UIBlurEffect(style: .systemThinMaterial)
#else
  let glass = UIGlassEffect(style: .regular)
  glass.tintColor = tint
  glass.isInteractive = interactive
  return glass
#endif
}

extension UIButton.Configuration {
  /** A glass button; visionOS draws its bordered buttons as glass. */
  static func panelGlass() -> UIButton.Configuration {
#if os(visionOS)
    return .bordered()
#else
    return .glass()
#endif
  }
}

/** \} */
