/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The Properties and Outliner panels, viewport shading and tool settings, in SwiftUI. This class
 * answers the same messages as `GHOSTIOSScenePanels`; the view controller finds it by name, see
 * `GHOST_WindowIOS.mm`. UIKit keeps placing the panels and the bar items; SwiftUI draws them.
 */

import SwiftUI
import UIKit

@MainActor
@objc(GHOSTIOSSwiftScenePanels)
public final class SwiftScenePanels: NSObject, UIPopoverPresentationControllerDelegate,
  UIAdaptivePresentationControllerDelegate
{
  @objc public let inspectorButton: UIBarButtonItem
  @objc public let hierarchyButton: UIBarButtonItem
  @objc public var menuProvider: ((NSDictionary) -> UIMenu?)? {
    didSet {
      inspector.menuProvider = menuProvider
      hierarchy.menuProvider = menuProvider
      shading.menuProvider = menuProvider
      toolStrip.menuProvider = menuProvider
      toolPopover.menuProvider = menuProvider
    }
  }

  private weak var overlay: UIView?
  private let inspector = InspectorStore(mode: .properties)
  private let shading = InspectorStore(mode: .shading, tab: "SHADING")
  private let toolStrip = InspectorStore(mode: .toolStrip, tab: "TOOL")
  private let toolPopover = InspectorStore(mode: .toolPopover, tab: "TOOL")
  private lazy var hierarchy = HierarchyStore { [weak self] request in self?.inspector.send(request) }

  private var inspectorHost: UIHostingController<AnyView>!
  private var hierarchyHost: UIHostingController<AnyView>!
  /** The panels' glass, as UIKit draws it: content inside its view is shown with the glass's
   * vibrancy, which SwiftUI's glass modifier does not give. */
  private var inspectorPanel: UIVisualEffectView!
  private var hierarchyPanel: UIVisualEffectView!
  private var showInspector = false
  private var showHierarchy = false
  private var layoutRect = CGRect.zero

  private var shadingController: UIViewController?
  private var shadingVisible = false
  private var toolGroup: UIBarButtonItemGroup?
  private var toolHost: UIHostingController<AnyView>?
  private var toolRow: UIStackView?
  private var toolMore: UIButton?
  private var toolWidth: NSLayoutConstraint?
  private var toolContentWidth: CGFloat = 0
  private var toolRemeasurePending = false
  private var toolPopoverController: UIViewController?
  private var toolID: String?

  @objc(initWithOverlay:icon:)
  public init(overlay: UIView, icon: @escaping (String) -> UIImage?) {
    self.overlay = overlay
    /* Blender's own editor-type icons, as in the area header's editor menu. */
    inspectorButton = UIBarButtonItem(image: icon("PROPERTIES") ?? UIImage(systemName: "sidebar.right"),
                                      style: .plain, target: nil, action: nil)
    hierarchyButton = UIBarButtonItem(image: icon("OUTLINER") ?? UIImage(systemName: "list.bullet.indent"),
                                      style: .plain, target: nil, action: nil)
    super.init()
    inspectorButton.target = self
    inspectorButton.action = #selector(toggleInspector)
    hierarchyButton.target = self
    hierarchyButton.action = #selector(toggleHierarchy)
    inspectorButton.title = "Properties"
    inspectorButton.accessibilityLabel = "Properties"
    hierarchyButton.title = "Outliner"
    hierarchyButton.accessibilityLabel = "Outliner"
    inspectorButton.accessibilityIdentifier = "blender.inspector"
    hierarchyButton.accessibilityIdentifier = "blender.hierarchy"
    inspector.visible = false

    inspectorHost = makeHost(AnyView(InspectorPanel(store: inspector) { [weak self] in self?.toggleInspector() }))
    hierarchyHost = makeHost(AnyView(HierarchyPanel(store: hierarchy) { [weak self] in self?.toggleHierarchy() }))
    inspectorPanel = makePanel(inspectorHost)
    hierarchyPanel = makePanel(hierarchyHost)
    inspectorPanel.accessibilityIdentifier = "blender.inspector.panel"
    hierarchyPanel.accessibilityIdentifier = "blender.hierarchy.panel"
    for panel in [inspectorPanel!, hierarchyPanel!] {
      panel.isHidden = true
      overlay.addSubview(panel)
    }
  }

  private func makePanel(_ host: UIHostingController<AnyView>) -> UIVisualEffectView {
    let glass = UIGlassEffect(style: .regular)
    /* Over Blender's dark viewport untinted glass turns mid-grey even in light mode, which mutes
     * colored icons and text. A light tint keeps light mode light; dark mode stays untinted. */
    glass.tintColor = UIColor { traits in
      traits.userInterfaceStyle == .dark ? .clear : UIColor(white: 1, alpha: 0.6)
    }
    let panel = UIVisualEffectView(effect: glass)
    panel.layer.cornerRadius = 16
    panel.clipsToBounds = true
    host.view.frame = panel.contentView.bounds
    host.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    panel.contentView.addSubview(host.view)
    return panel
  }

  private func makeHost(_ view: AnyView) -> UIHostingController<AnyView> {
    let host = UIHostingController(rootView: view)
    host.view.backgroundColor = .clear
    host.sizingOptions = []
    host.safeAreaRegions = []
    return host
  }

  /** SwiftUI presents popovers and alerts from the view controller its host belongs to. */
  private func adoptHosts() {
    guard let overlay, overlay.window != nil else { return }
    var responder: UIResponder? = overlay
    while let next = responder?.next, !(next is UIViewController) { responder = next }
    guard let parent = responder?.next as? UIViewController else { return }
    for host in [inspectorHost!, hierarchyHost!] where host.parent == nil {
      parent.addChild(host)
      host.didMove(toParent: parent)
    }
  }

  /* -------------------------------------------------------------------- */
  /** \name Panels
   * \{ */

  @objc private func toggleInspector() {
    overlay?.endEditing(true)
    showInspector.toggle()
    if showInspector && layoutRect.width < 700 { showHierarchy = false }
    applyVisibility()
  }

  @objc private func toggleHierarchy() {
    overlay?.endEditing(true)
    showHierarchy.toggle()
    if showHierarchy && layoutRect.width < 700 { showInspector = false }
    applyVisibility()
  }

  @objc public func dismiss() {
    dismissShading()
    overlay?.endEditing(true)
    showInspector = false
    showHierarchy = false
    applyVisibility()
  }

  private func applyVisibility() {
    if !showInspector { ColorPickerSession.active?.close() }
    inspector.visible = showInspector
    inspectorButton.isSelected = showInspector
    hierarchyButton.isSelected = showHierarchy
    animate(inspectorPanel, visible: showInspector, leading: false)
    animate(hierarchyPanel, visible: showHierarchy, leading: true)
    Engine.setting("native_panels", "\((showInspector ? 1 : 0) | (showHierarchy ? 2 : 0))")
    layout(in: layoutRect)
  }

  private func animate(_ panel: UIView, visible: Bool, leading: Bool) {
    let reduce = UIAccessibility.isReduceMotionEnabled
    let hidden = reduce ? .identity :
      CGAffineTransform(translationX: leading ? -14 : 14, y: 0).scaledBy(x: 0.98, y: 0.98)
    panel.isUserInteractionEnabled = visible
    if visible {
      overlay?.bringSubviewToFront(panel)
      if panel.isHidden {
        panel.alpha = 0
        panel.transform = hidden
      }
      panel.isHidden = false
    }
    UIView.animate(withDuration: reduce ? 0 : 0.28, delay: 0, usingSpringWithDamping: 0.9,
                   initialSpringVelocity: 0, options: [.beginFromCurrentState, .allowUserInteraction])
    {
      panel.alpha = visible ? 1 : 0
      panel.transform = visible ? .identity : hidden
    } completion: { finished in
      if finished && panel.alpha == 0 { panel.isHidden = true }
    }
  }

  @objc(layoutInRect:)
  public func layout(in rect: CGRect) {
    adoptHosts()
    /* The top bar's room for tool settings changes with rotation and window size. */
    updateToolStripWidth()
    layoutRect = rect
    if rect.width < 700 && showInspector && showHierarchy {
      showHierarchy = false
      applyVisibility()
      return
    }
    let width = min(336, max(0, rect.width - 24))
    /* Use all the height there is, so portrait and tall windows show more rows. */
    let height = max(0, rect.height - 24)
    /* Frame is undefined while transformed; layout must not disturb the presentation animation. */
    for panel in [inspectorPanel!, hierarchyPanel!] {
      panel.bounds = CGRect(x: 0, y: 0, width: width, height: height)
    }
    hierarchyPanel.center = CGPoint(x: rect.minX + 12 + width / 2, y: rect.minY + 12 + height / 2)
    inspectorPanel.center = CGPoint(x: rect.maxX - 12 - width / 2, y: rect.minY + 12 + height / 2)
  }

  @objc(updateSnapshot:)
  public func updateSnapshot(_ snapshot: Any?) {
    guard let snapshot = snapshot as? JSON else { return }
    if let shadingSnapshot = snapshot.dict("shading"), shadingVisible {
      shading.update(snapshot: ["ack": snapshot["ack"] ?? 0, "error": snapshot["error"] ?? "",
                                "inspector": shadingSnapshot])
    }
    let ack = (snapshot["ack"] as? NSNumber)?.uint64Value ?? 0
    if ack < inspector.sequence { return }
    if let nodes = snapshot["hierarchy"] as? [Any] {
      hierarchy.update(nodes: nodes.compactMap { $0 as? JSON }, status: snapshot.string("error") ?? "")
    }
    if showInspector, snapshot["inspector"] != nil {
      inspector.update(snapshot: snapshot)
    }
  }

  /** \} */

  /* -------------------------------------------------------------------- */
  /** \name Viewport shading
   * \{ */

  @objc(presentShadingFrom:controller:)
  public func presentShading(from button: UIBarButtonItem, controller: UIViewController) {
    if shadingController?.presentingViewController != nil || controller.presentedViewController != nil {
      return
    }
    if shadingController == nil {
      let content = UIHostingController(rootView: PopoverForm(store: shading, title: "Viewport Shading") {
        [weak self] in self?.dismissShading()
      })
      content.preferredContentSize = CGSize(width: 400, height: 640)
      shadingController = content
    }
    guard let shadingController else { return }
    shadingVisible = true
    shading.requestProperties()
    Engine.setting("native_shading_visible", "1")
    shadingController.modalPresentationStyle = .popover
    shadingController.popoverPresentationController?.barButtonItem = button
    shadingController.popoverPresentationController?.delegate = self
    shadingController.presentationController?.delegate = self
    controller.present(shadingController, animated: true)
  }

  @objc public func dismissShading() {
    guard shadingVisible else { return }
    shadingVisible = false
    ColorPickerSession.active?.close()
    shadingController?.view.endEditing(true)
    Engine.setting("native_shading_visible", "0")
    shadingController?.dismiss(animated: true)
  }

  public func presentationControllerWillDismiss(_ presentationController: UIPresentationController) {
    /* Resign the keyboard while the field is still in the popover's window. */
    presentationController.presentedViewController.view.endEditing(true)
  }

  public func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
    if presentationController.presentedViewController === shadingController { dismissShading() }
    ColorPickerSession.active?.close()
  }

  /** \} */

  /* -------------------------------------------------------------------- */
  /** \name Tool settings
   * \{ */

  @objc(updateToolSettings:acknowledgement:)
  public func updateToolSettings(_ settings: Any?, acknowledgement ack: Any?)
    -> UIBarButtonItemGroup?
  {
    let settingsJSON = settings as? JSON ?? [:]
    if toolID != settingsJSON.string("id") {
      hideToolSettings()
      toolID = settingsJSON.string("id")
    }
    let snapshot: JSON = ["inspector": settingsJSON, "ack": ack as? NSNumber ?? 0]
    toolStrip.update(snapshot: snapshot)
    toolPopover.update(snapshot: snapshot)
    let group = ensureToolGroup()
    let empty = toolStrip.sections.allSatisfy { $0.rows.isEmpty }
    group.isHidden = empty
    updateToolStripWidth()
    return group
  }

  private func ensureToolGroup() -> UIBarButtonItemGroup {
    if let toolGroup { return toolGroup }
    let host = UIHostingController(rootView: AnyView(
      ScrollView(.horizontal, showsIndicators: false) {
        ToolStrip(store: toolStrip)
          .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { [weak self] width in
            self?.toolContentWidth = width
            self?.updateToolStripWidth()
          }
      }
      .scrollClipDisabled(false)))
    host.view.backgroundColor = .clear
    host.sizingOptions = []
    host.safeAreaRegions = []
    host.view.translatesAutoresizingMaskIntoConstraints = false
    let width = host.view.widthAnchor.constraint(equalToConstant: 160)
    NSLayoutConstraint.activate([width, host.view.heightAnchor.constraint(equalToConstant: 40)])
    toolWidth = width
    toolHost = host

    let more = UIButton(type: .system)
    more.setImage(UIImage(systemName: "ellipsis"), for: .normal)
    more.addTarget(self, action: #selector(presentToolSettings(_:)), for: .touchUpInside)
    more.accessibilityIdentifier = "blender.tool-settings.more"
    more.accessibilityLabel = "More Tool Settings"
    more.widthAnchor.constraint(equalToConstant: 32).isActive = true
    more.isHidden = true
    toolMore = more

    let row = UIStackView(arrangedSubviews: [host.view, more])
    row.axis = .horizontal
    row.alignment = .center
    row.spacing = 4
    toolRow = row

    let compact = UIBarButtonItem(image: BlenderIcon.image("TOOL_SETTINGS") ??
                                    UIImage(systemName: "slider.horizontal.3"),
                                  style: .plain, target: self, action: #selector(presentToolSettings(_:)))
    compact.accessibilityLabel = "Tool Settings"
    /* Keep one group installed in the bar: replacing it on every update makes the bar
     * re-measure from scratch and fall back to the compact item. */
    let group = UIBarButtonItemGroup.fixedGroup(representativeItem: compact,
                                                items: [UIBarButtonItem(customView: row)])
    toolGroup = group
    return group
  }

  /** The free gap between the bar's controls on either side of the strip. Before the strip is
   * on screen, estimate the room the bar's leading and trailing items leave. */
  private func availableToolStripWidth() -> CGFloat {
    guard let row = toolRow, row.window != nil else {
      return max(160, (overlay?.bounds.width ?? 800) - 620)
    }
    var bar: UIView? = row.superview
    while let current = bar, !(current is UINavigationBar) { bar = current.superview }
    guard let bar else { return max(160, (overlay?.bounds.width ?? 800) - 620) }
    let frame = row.convert(row.bounds, to: bar)
    var left = bar.bounds.minX
    var right = bar.bounds.maxX
    var pending: [UIView] = [bar]
    while let view = pending.popLast() {
      if view === row || view.isHidden || view.alpha < 0.01 { continue }
      let other = view.convert(view.bounds, to: bar)
      if view is UIControl && other.width < bar.bounds.width / 2 {
        if other.midX < frame.midX { left = max(left, other.maxX) }
        else { right = min(right, other.minX) }
        continue
      }
      pending.append(contentsOf: view.subviews)
    }
    return max(160, right - left - 24)
  }

  private func updateToolStripWidth() {
    guard let toolWidth else { return }
    /* Fill the room the bar leaves between its other items; the strip scrolls for the rest. */
    let available = availableToolStripWidth()
    let overflow = toolContentWidth > available + 0.5
    let width = overflow ? available - 36 : max(1, toolContentWidth)
    toolMore?.isHidden = !overflow
    if abs(toolWidth.constant - width) > 0.5 {
      toolWidth.constant = width
      /* The bar measures custom views when it lays out; make it measure again, then check the
       * gap once more, since the bar may move its other items to fit the new width. */
      toolRow?.superview?.superview?.setNeedsLayout()
      if !toolRemeasurePending {
        toolRemeasurePending = true
        DispatchQueue.main.async { [weak self] in
          self?.toolRemeasurePending = false
          self?.updateToolStripWidth()
        }
      }
    }
  }

  @objc private func presentToolSettings(_ sender: Any) {
    guard let presenter = overlay?.window?.rootViewController, presenter.presentedViewController == nil else {
      return
    }
    let content = UIHostingController(rootView: PopoverForm(store: toolPopover, title: "Tool Settings") {
      [weak self] in self?.dismissToolSettings()
    })
    /* Fit the popover to the few settings a tool has rather than a fixed sheet. */
    let fitting = content.sizeThatFits(in: CGSize(width: 320, height: 1000))
    content.preferredContentSize = CGSize(width: 320, height: min(420, max(88, fitting.height)))
    content.modalPresentationStyle = .popover
    if let item = sender as? UIBarButtonItem {
      content.popoverPresentationController?.barButtonItem = item
    }
    else if let view = sender as? UIView {
      content.popoverPresentationController?.sourceView = view
      content.popoverPresentationController?.sourceRect = view.bounds
    }
    content.presentationController?.delegate = self
    toolPopoverController = content
    presenter.present(content, animated: true)
  }

  private func dismissToolSettings() {
    toolPopoverController?.view.endEditing(true)
    ColorPickerSession.active?.close()
    toolPopoverController?.dismiss(animated: true)
    toolPopoverController = nil
  }

  @objc public func hideToolSettings() {
    toolHost?.view.endEditing(true)
    dismissToolSettings()
  }

  /** \} */
}
