/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The document window's panels: Properties, the Outliner and the asset pane. Their
 * content is SwiftUI; they keep the glass UIKit draws, whose vibrancy SwiftUI's glass does not
 * give, and their own presentation. This layer places them, between the toolbar and the controls
 * resting on the bottom edge, and tells the window how much of the bottom the asset pane and
 * timeline take. The window shows the timeline itself.
 */

import SwiftUI
import UIKit

@MainActor
final class ShellOverlay: UIView {
  let model: ShellModel
  private(set) var panels: SwiftScenePanels!
  private(set) var timeline: SwiftTimeline!
  private(set) var assets: SwiftAssets!
  private var compileIndicator: NSObject?

  init(model: ShellModel) {
    self.model = model
    super.init(frame: CGRect(x: 0, y: 0, width: 100, height: 100))
    backgroundColor = .clear
    /* Each adds its views to this layer, and presents from the controller it belongs to. */
    panels = SwiftScenePanels(overlay: self)
    /* The window shows the timeline itself, as #TimelinePanel. */
    timeline = SwiftTimeline()
    assets = SwiftAssets()
    panels.menuProvider = { wm_apple_ios_property_menu($0 as! [AnyHashable: Any]) }
    panels.panelsDidChange = { [weak self] in
      guard let self else { return }
      model.inspectorShown = panels.inspectorShown
      model.hierarchyShown = panels.hierarchyShown
    }
    timeline.visibilityDidChange = { [weak self] in self?.trayChanged() }
    assets.visibilityDidChange = { [weak self] in self?.trayChanged() }
    assets.clientPoint = { [weak model] point in wm_apple_ios_window_pixels(model?.window, point) }
    compileIndicator = wm_apple_ios_compile_indicator(self)
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

  /** Touches between the panels reach Blender's viewport and the controls below. */
  override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
    let hit = super.hitTest(point, with: event)
    return hit === self ? nil : hit
  }

  func detach() {
    if let compileIndicator { wm_apple_ios_compile_indicator_stop(compileIndicator) }
  }

  /** The asset pane or timeline came or went, or changed height: everything resting on the
   * bottom edge moves with it. */
  private func trayChanged() {
    model.timelineShown = timeline.visible
    model.assetsShown = assets.visible
    UIView.animate(withDuration: UIAccessibility.isReduceMotionEnabled ? 0 : 0.3, delay: 0,
                   usingSpringWithDamping: 0.9, initialSpringVelocity: 0,
                   options: [.beginFromCurrentState, .allowUserInteraction])
    {
      self.setNeedsLayout()
      self.layoutIfNeeded()
    }
  }

  override func layoutSubviews() {
    super.layoutSubviews()
    let safe = bounds.inset(by: model.safeAreaInsets)
    /* The bottom tray: the open asset pane along the edge and the timeline just above it. */
    let trayShown = !model.desktop && !model.interfaceHidden
    let assetsTray = assets.visible && trayShown ? AssetsPanel.height(compact: model.compact) + 8 : 0
#if os(visionOS)
    let timelineTray: CGFloat = 0 /* A window of its own. */
#else
    let timelineTray = timeline.visible && trayShown ? timeline.height + 8 : 0
#endif
    let tray = assetsTray + timelineTray
    if abs(tray - model.bottomTray) > 0.5 {
      withAnimation(SwiftTimeline.animation) { model.bottomTray = tray }
    }
    if bounds.height > 0 && !model.desktop {
      /* Blender keeps its own overlays, such as the navigation gizmo, above the tray. */
      let trayTop = safe.maxY - 6 - (tray > 0 ? tray - 8 : 0)
      WM_apple_ios_ui_bottom_tray_set(tray > 0 ? Float((bounds.height - trayTop) / bounds.height) : 0)
    }
    let panelTop = max(safe.minY, model.contentTop)
    let panelBottom = min(safe.maxY - 64 - tray, keyboardLayoutGuide.layoutFrame.minY)
    panels.layout(in: CGRect(x: safe.minX, y: panelTop, width: safe.width, height: max(0, panelBottom - panelTop)))
  }

  /** Blender's panels, timeline and asset snapshots, whichever changed. */
  func pullSnapshots() {
    if let timelineSnapshot = takeJSON(WM_apple_ios_ui_timeline_take_json()) { timeline.updateSnapshot(timelineSnapshot) }
    if let assetsSnapshot = takeJSON(WM_apple_ios_ui_assets_take_json()) { assets.updateSnapshot(assetsSnapshot) }
    if let panelsSnapshot = takeJSON(WM_apple_ios_ui_panels_take_json()) { panels.updateSnapshot(panelsSnapshot) }
  }

  private func takeJSON(_ json: UnsafeMutablePointer<CChar>?) -> Any? {
    guard let json else { return nil }
    let data = Data(bytes: json, count: strlen(json))
    WM_apple_ios_ui_toolbar_free_json(json)
    return try? JSONSerialization.jsonObject(with: data)
  }
}

/** The panels' layer in the window. */
struct ShellOverlayView: UIViewRepresentable {
  let overlay: ShellOverlay

  func makeUIView(context: Context) -> ShellOverlay { overlay }
  func updateUIView(_ overlay: ShellOverlay, context: Context) { overlay.setNeedsLayout() }
}
