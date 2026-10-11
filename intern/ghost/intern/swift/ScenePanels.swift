/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The Properties and Outliner panels, viewport shading and tool settings, in SwiftUI. This class
 * answers the `GHOSTIOSScenePanels` protocol's messages; the view controller finds it by name, see
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
  private var toolIcon: UIButton?
  private var toolReinstalledWidth: CGFloat?
  private var toolHost: UIHostingController<AnyView>?
  private var toolRow: ToolStripRow?
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
#if os(visionOS)
    Self.current = self
#endif
  }

  private func makePanel(_ host: UIHostingController<AnyView>) -> UIVisualEffectView {
    /* Over Blender's dark viewport untinted glass turns mid-grey even in light mode, which mutes
     * colored icons and text. A light tint keeps light mode light; dark mode stays untinted. */
    let glass = makePanelGlass(tint: UIColor { traits in
      traits.userInterfaceStyle == .dark ? .clear : UIColor(white: 1, alpha: 0.6)
    })
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
    /* UIKit buttons in the panels take the tint as it reads on glass. */
    host.view.tintColor = PanelStyle.uiTint
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
#if os(visionOS)
    toggleWindow(.properties)
    return
#endif
    overlay?.endEditing(true)
    showInspector.toggle()
    if showInspector && layoutRect.width < 700 { showHierarchy = false }
    applyVisibility()
  }

  @objc private func toggleHierarchy() {
#if os(visionOS)
    toggleWindow(.outliner)
    return
#endif
    overlay?.endEditing(true)
    showHierarchy.toggle()
    if showHierarchy && layoutRect.width < 700 { showInspector = false }
    applyVisibility()
  }

  @objc public func dismiss() {
    dismissShading()
#if os(visionOS)
    /* Windows of their own stay open beside the editor. */
    return
#endif
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
#if !os(visionOS)
    animate(inspectorPanel, visible: showInspector, leading: false)
    animate(hierarchyPanel, visible: showHierarchy, leading: true)
#endif
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
#if os(visionOS)
    return
#endif
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

#if os(visionOS)
  /* -------------------------------------------------------------------- */
  /** \name Windows of their own
   *
   * On Apple Vision Pro, Properties and the Outliner open as windows beside the editor, which
   * the person can place, resize and keep open, instead of covering the viewport.
   * \{ */

  static weak var current: SwiftScenePanels?

  private func toggleWindow(_ kind: PanelWindow) {
    PanelWindows.toggle(kind)
  }

  /** The panel a window shows: the same store as the editor's, so both stay in step. */
  func windowContent(_ kind: PanelWindow) -> AnyView {
    kind == .properties ? AnyView(InspectorPanel(store: inspector) {}) :
                          AnyView(HierarchyPanel(store: hierarchy) {})
  }

  func setWindow(_ kind: PanelWindow, open: Bool) {
    if kind == .properties { showInspector = open }
    else { showHierarchy = open }
    applyVisibility()
  }

  /** \} */
#endif

  /* -------------------------------------------------------------------- */
  /** \name Viewport shading
   * \{ */

  @objc(presentShadingFrom:controller:)
  public func presentShading(from button: UIBarButtonItem, controller: UIViewController) {
    if shadingController?.presentingViewController != nil || controller.presentedViewController != nil {
      return
    }
    if shadingController == nil {
      let content = popoverController(shading, title: "Viewport Shading", done: #selector(dismissShading))
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
    /* The window may only now have put the group in the bar. */
    scheduleToolStripUpdate()
    return group
  }

  private func ensureToolGroup() -> UIBarButtonItemGroup {
    if let toolGroup { return toolGroup }
    let host = UIHostingController(rootView: AnyView(
      ScrollView(.horizontal, showsIndicators: false) {
        ToolStrip(store: toolStrip)
          .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { [weak self] width in
            self?.toolContentWidth = width
            self?.scheduleToolStripUpdate()
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

    let row = ToolStripRow(arrangedSubviews: [host.view, more])
    row.axis = .horizontal
    row.alignment = .center
    row.spacing = 4
    /* The bar lays its items out again when any of them changes: fit the strip to the room
     * left then, and should the bar have collapsed it to its icon, bring it back. */
    row.laidOut = { [weak self] in self?.scheduleToolStripUpdate() }
    row.windowChanged = { [weak self] in self?.scheduleToolStripUpdate() }
    toolRow = row

    /* A view of our own, so the strip's measuring can tell it apart from the bar's other items. */
    let icon = UIButton(type: .system)
    icon.setImage(BlenderIcon.image("TOOL_SETTINGS") ?? UIImage(systemName: "slider.horizontal.3"), for: .normal)
    icon.addTarget(self, action: #selector(presentToolSettings(_:)), for: .touchUpInside)
    icon.accessibilityIdentifier = Self.toolIconIdentifier
    icon.accessibilityLabel = "Tool Settings"
    toolIcon = icon
    let compact = UIBarButtonItem(customView: icon)
    /* Keep one group installed in the bar: replacing it on every update makes the bar
     * re-measure from scratch and fall back to the compact item. */
    let group = UIBarButtonItemGroup.fixedGroup(representativeItem: compact,
                                                items: [UIBarButtonItem(customView: row)])
    toolGroup = group
    return group
  }

  private static let toolIconIdentifier = "blender.tool-settings.icon"

  /** The top bar the strip sits in, also while the bar shows the strip as its icon. */
  private func toolBar() -> UINavigationBar? {
    var view: UIView? = toolRow?.superview
    while let current = view {
      if let bar = current as? UINavigationBar { return bar }
      view = current.superview
    }
    var pending: [UIView] = overlay?.window.map { [$0] } ?? []
    while let current = pending.popLast() {
      if let bar = current as? UINavigationBar { return bar }
      pending.append(contentsOf: current.subviews)
    }
    return nil
  }

  /**
   * The room for the strip: the bar centers it, so twice the distance from the bar's middle to the
   * nearest control on either side, not counting the strip itself or the icon the bar shows in its
   * place when it collapses it.
   */
  private func availableToolStripWidth(in bar: UINavigationBar) -> CGFloat {
    let center = bar.bounds.midX
    var left = bar.bounds.minX
    var right = bar.bounds.maxX
    var pending: [UIView] = [bar]
    while let view = pending.popLast() {
      if view.isHidden || view.alpha < 0.01 { continue }
      /* Skip the strip and its icon along with whatever the bar wraps them in. */
      if view !== bar, [toolRow, toolIcon].contains(where: { $0?.isDescendant(of: view) == true }) {
        if view === toolRow || view === toolIcon || view is UIControl { continue }
        pending.append(contentsOf: view.subviews)
        continue
      }
      let other = view.convert(view.bounds, to: bar)
      if view is UIControl && other.width < bar.bounds.width / 2 {
        if other.midX < center { left = max(left, other.maxX) }
        else { right = min(right, other.minX) }
        continue
      }
      pending.append(contentsOf: view.subviews)
    }
    return max(80, 2 * min(center - left, right - center) - 32)
  }

  private func scheduleToolStripUpdate() {
    if toolRemeasurePending { return }
    toolRemeasurePending = true
    DispatchQueue.main.async { [weak self] in
      self?.toolRemeasurePending = false
      self?.updateToolStripWidth()
    }
  }

  /**
   * Fill the room the bar leaves between its other items, as a toolbar does: settings that do not
   * fit stay reachable through the overflow button at the end. The strip is never wider than that
   * room, so the bar keeps it rather than collapsing it to its icon.
   */
  private func updateToolStripWidth() {
    guard let toolWidth, let toolGroup, !toolGroup.isHidden, let bar = toolBar() else { return }
    let available = availableToolStripWidth(in: bar)
    let overflow = toolContentWidth > available + 0.5
    let width = max(1, overflow ? available - 36 : toolContentWidth)
    toolMore?.isHidden = !overflow
    if abs(toolWidth.constant - width) > 0.5 {
      toolWidth.constant = width
      /* The bar measures its items' custom views when it lays out. */
      bar.setNeedsLayout()
    }
    /* Once the bar has shown the strip as its icon it keeps doing so until its items change:
     * hand it the group again now that the strip fits. */
    if toolRow?.window == nil, let item = bar.topItem, item.centerItemGroups.contains(toolGroup) {
      if toolReinstalledWidth == width { return }
      toolReinstalledWidth = width
      let groups = item.centerItemGroups
      item.centerItemGroups = []
      item.centerItemGroups = groups
    }
    else {
      toolReinstalledWidth = nil
    }
  }

  @objc private func presentToolSettings(_ sender: Any) {
    guard let presenter = overlay?.window?.rootViewController, presenter.presentedViewController == nil else {
      return
    }
    let content = popoverController(toolPopover, title: "Tool Settings", done: #selector(dismissToolSettings))
    /* Fit the popover to the few settings a tool has rather than a fixed sheet. */
    let fitting = UIHostingController(rootView: InspectorForm(store: toolPopover, scrolls: false))
      .sizeThatFits(in: CGSize(width: 320, height: CGFloat.greatestFiniteMagnitude))
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

  /** A form in a navigation controller with the system's Done, as Blender's popovers have. */
  private func popoverController(_ store: InspectorStore, title: String, done: Selector) -> UIViewController {
    let form = UIHostingController(rootView: InspectorForm(store: store).tint(PanelStyle.tint))
    /* Let the popover's own glass show through, matching the inspector and rails. */
    form.view.backgroundColor = .clear
    form.view.tintColor = PanelStyle.uiTint
    form.title = title
    form.navigationItem.rightBarButtonItem = UIBarButtonItem(barButtonSystemItem: .done, target: self, action: done)
    return UINavigationController(rootViewController: form)
  }

  @objc private func dismissToolSettings() {
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

/** The tool settings strip's row in the top bar, telling when the bar lays it out or moves it. */
private final class ToolStripRow: UIStackView {
  var laidOut: (() -> Void)?
  var windowChanged: (() -> Void)?

  override func layoutSubviews() {
    super.layoutSubviews()
    laidOut?()
  }

  override func didMoveToWindow() {
    super.didMoveToWindow()
    windowChanged?()
  }
}

#if os(visionOS)
/** The panels that open as windows of their own on Apple Vision Pro. */
enum PanelWindow: String, CaseIterable {
  case properties = "org.blender.window.properties"
  case outliner = "org.blender.window.outliner"
  case timeline = "org.blender.window.timeline"

  var title: String {
    switch self {
    case .properties: "Properties"
    case .outliner: "Outliner"
    case .timeline: "Timeline"
    }
  }
}

@MainActor
enum PanelWindows {
  /** Closes a panel's window, while it is open. */
  static var close: [PanelWindow: () -> Void] = [:]

  static func toggle(_ kind: PanelWindow) {
    if let close = close[kind] {
      close()
      return
    }
    guard let request = UISceneSessionActivationRequest(hostingDelegateClass: PanelSceneDelegate.self,
                                                        id: kind.rawValue) else { return }
    UIApplication.shared.activateSceneSession(for: request) { _ in }
  }

  /** What the window shows, from the editor's own stores; nil without a document open. */
  static func content(_ kind: PanelWindow) -> AnyView? {
    kind == .timeline ? SwiftTimeline.current?.windowContent() :
                        SwiftScenePanels.current?.windowContent(kind)
  }

  static func setOpen(_ kind: PanelWindow, _ open: Bool) {
    if kind == .timeline { SwiftTimeline.current?.setWindowOpen(open) }
    else { SwiftScenePanels.current?.setWindow(kind, open: open) }
  }
}

/** The panels as SwiftUI windows, which the person places around the editor. */
final class PanelSceneDelegate: NSObject, UIHostingSceneDelegate {
  static var rootScene: some Scene {
    panel(.properties, size: CGSize(width: 420, height: 760))
    panel(.outliner, size: CGSize(width: 360, height: 640))
    /* Wide and low, under the viewport. */
    panel(.timeline, size: CGSize(width: 1100, height: 96))
  }

  private static func panel(_ kind: PanelWindow, size: CGSize) -> some Scene {
    Window(kind.title, id: kind.rawValue) {
      PanelWindowContent(kind: kind)
    }
    .defaultSize(size)
    .windowResizability(.contentMinSize)
  }
}

struct PanelWindowContent: View {
  let kind: PanelWindow
  @Environment(\.dismissWindow) private var dismissWindow

  var body: some View {
    Group {
      if let content = PanelWindows.content(kind) {
        content
      }
    }
    .frame(minWidth: kind == .timeline ? 520 : 320, minHeight: kind == .timeline ? 64 : 420)
    .environment(\.panelInWindow, true)
    .tint(PanelStyle.tint)
    .onAppear {
      guard PanelWindows.content(kind) != nil else {
        /* Restored without a document open: there is nothing to show. */
        dismissWindow(id: kind.rawValue)
        return
      }
      PanelWindows.close[kind] = { dismissWindow(id: kind.rawValue) }
      PanelWindows.setOpen(kind, true)
    }
    .onDisappear {
      PanelWindows.close[kind] = nil
      PanelWindows.setOpen(kind, false)
    }
  }
}
#endif
