/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The Properties and Outliner panels, viewport shading and tool settings, in SwiftUI. UIKit
 * keeps placing the panels; SwiftUI draws them.
 */

import SwiftUI
import UIKit

@MainActor
final class SwiftScenePanels {
  var menuProvider: ((NSDictionary) -> UIMenu?)? {
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

  private var shadingVisible = false

  init(overlay: UIView) {
    self.overlay = overlay
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

  /** Whether Properties and the Outliner show, and a call when either changes. */
  var inspectorShown: Bool { showInspector }
  var hierarchyShown: Bool { showHierarchy }
  var panelsDidChange: (() -> Void)?

  func toggleInspector() {
#if os(visionOS)
    toggleWindow(.properties)
    return
#endif
    overlay?.endEditing(true)
    showInspector.toggle()
    if showInspector && layoutRect.width < 700 { showHierarchy = false }
    applyVisibility()
  }

  func toggleHierarchy() {
#if os(visionOS)
    toggleWindow(.outliner)
    return
#endif
    overlay?.endEditing(true)
    showHierarchy.toggle()
    if showHierarchy && layoutRect.width < 700 { showInspector = false }
    applyVisibility()
  }

  func dismiss() {
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
#if !os(visionOS)
    animate(inspectorPanel, visible: showInspector, leading: false)
    animate(hierarchyPanel, visible: showHierarchy, leading: true)
#endif
    Engine.setting("native_panels", "\((showInspector ? 1 : 0) | (showHierarchy ? 2 : 0))")
    layout(in: layoutRect)
    panelsDidChange?()
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

  func layout(in rect: CGRect) {
    adoptHosts()
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

  func updateSnapshot(_ snapshot: Any?) {
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

  /** Viewport shading as a SwiftUI popover's content shows it: Blender sends its settings while
   * it is open. */
  func shadingContent(done: @escaping () -> Void) -> some View {
    NavigationStack {
      InspectorForm(store: shading)
        .tint(PanelStyle.tint)
        .navigationTitle("Viewport Shading")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done", action: done) } }
    }
    /* A popover's size; a sheet on a phone, which it fills. */
    .frame(idealWidth: 400, idealHeight: 640)
    .presentationDetents([.medium, .large])
  }

  func setShadingShown(_ shown: Bool) {
    guard shown != shadingVisible else { return }
    shadingVisible = shown
    if shown { shading.requestProperties() }
    else { ColorPickerSession.active?.close() }
    Engine.setting("native_shading_visible", shown ? "1" : "0")
  }

  /** \} */

  /* -------------------------------------------------------------------- */
  /** \name Tool settings
   * \{ */

  /** The tool's settings for the toolbar. Returns whether the tool has any. */
  func updateToolSettingsStores(_ settings: JSON, acknowledgement ack: NSNumber) -> Bool {
    let snapshot: JSON = ["inspector": settings, "ack": ack]
    toolStrip.update(snapshot: snapshot)
    toolPopover.update(snapshot: snapshot)
    return !toolStrip.sections.allSatisfy { $0.rows.isEmpty }
  }

  var toolStripStore: InspectorStore { toolStrip }

  /** All the tool's settings, in a popover from the toolbar. */
  func toolSettingsContent(done: @escaping () -> Void) -> some View {
    NavigationStack {
      InspectorForm(store: toolPopover)
        .tint(PanelStyle.tint)
        .navigationTitle("Tool Settings")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done", action: done) } }
    }
    /* A popover's size; a sheet on a phone, which it fills. */
    .frame(idealWidth: 320, idealHeight: 420)
    .presentationDetents([.medium, .large])
  }

  /** \} */
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
