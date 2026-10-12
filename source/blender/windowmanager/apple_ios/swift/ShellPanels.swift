/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The document window's panels: Properties, the Outliner, the timeline and the asset pane, fed
 * from Blender's snapshots. The window shows them; this keeps the model in step with them.
 */

import SwiftUI
import UIKit

@MainActor
final class ShellPanels {
  let model: ShellModel
  let panels = SwiftScenePanels()
  let timeline = SwiftTimeline()
  let assets = SwiftAssets()
  let compile = CompileProgress()

  init(model: ShellModel) {
    self.model = model
    panels.menuProvider = { wm_apple_ios_property_menu($0 as! [AnyHashable: Any]) }
    panels.panelsDidChange = { [weak self] in
      guard let self else { return }
      withAnimation(SwiftTimeline.animation) {
        self.model.inspectorShown = self.panels.inspectorShown
        self.model.hierarchyShown = self.panels.hierarchyShown
      }
    }
    timeline.visibilityDidChange = { [weak self] in
      guard let self else { return }
      model.timelineShown = timeline.visible
    }
    assets.visibilityDidChange = { [weak self] in
      guard let self else { return }
      model.assetsShown = assets.visible
    }
    assets.clientPoint = { [weak model] point in wm_apple_ios_window_pixels(model?.window, point) }
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
