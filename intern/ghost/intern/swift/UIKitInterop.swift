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

  func makeUIView(context: Context) -> UIButton {
    let button = UIButton(type: .system)
    button.showsMenuAsPrimaryAction = true
    button.contentHorizontalAlignment = .leading
    button.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    return button
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
      button.menu = makeMenu()
      return
    }
    var configuration: UIButton.Configuration
    switch style {
      case .glass, .glassPopup: configuration = .glass()
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
    button.configuration = configuration
    button.isEnabled = enabled
    button.tintColor = tint
    button.contentHorizontalAlignment = style == .glassPopup || style == .plain ? .center : .leading
    button.accessibilityLabel = accessibilityLabel ?? title
    button.menu = makeMenu()
  }

  private func makeMenu() -> UIMenu {
    guard deferred else { return menu() ?? UIMenu(children: []) }
    /* A deferred element builds the menu when it opens. */
    let provider = menu
    return UIMenu(title: menuTitle, children: [UIDeferredMenuElement.uncached { completion in
      completion(provider()?.children ?? [])
    }])
  }

  func sizeThatFits(_ proposal: ProposedViewSize, uiView: UIButton, context: Context) -> CGSize? {
    let fitting = uiView.systemLayoutSizeFitting(UIView.layoutFittingCompressedSize)
    if style == .glassPopup, let height = proposal.height {
      return CGSize(width: proposal.width ?? fitting.width, height: height)
    }
    return CGSize(width: proposal.width ?? fitting.width, height: max(fitting.height, 32))
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Number scrubbing
 * \{ */

/** A horizontal drag that only begins when mostly sideways, so vertical drags still scroll. */
struct ScrubGesture: UIGestureRecognizerRepresentable {
  var enabled: Bool
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
    switch recognizer.state {
      case .began: began()
      case .changed: changed(recognizer.translation(in: recognizer.view?.window))
      case .ended:
        changed(recognizer.translation(in: recognizer.view?.window))
        ended(false)
      case .cancelled, .failed: ended(true)
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

/** Drag sensitivity: dragging further up or down while scrubbing makes changes coarser or finer,
 * as `GHOSTIOSNumberSensitivity`. */
func scrubSensitivity(_ vertical: Double) -> Double {
  exp2(min(4.0, max(-4.0, -vertical / 80.0)))
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

enum Haptics {
  static func selection() {
    UISelectionFeedbackGenerator().selectionChanged()
  }

  static func impact(_ style: UIImpactFeedbackGenerator.FeedbackStyle = .light) {
    UIImpactFeedbackGenerator(style: style).impactOccurred()
  }
}

/** \} */
