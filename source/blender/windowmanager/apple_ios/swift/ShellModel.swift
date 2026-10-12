/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The document window's state, from the toolbar snapshot Blender publishes: its mode, tools,
 * quick actions, brushes and undo. Each part changes on its own, so the views showing one part
 * do not redraw when another changes. Edits go to Blender's inbox; the next snapshot confirms
 * them.
 */

import SwiftUI
import UIKit

@MainActor
@Observable
final class ShellModel {
  /** Blender's window, `GHOST_WindowIOS *`, until Blender closes it. */
  @ObservationIgnored var window: UnsafeMutableRawPointer?

  /** The editing mode, such as `object`, `mesh_edit` or `sculpt`. */
  private(set) var mode = ""
  /** Blender's tools for the mode, in groups: `nil` separates two groups. */
  private(set) var tools: [JSON?] = []
  private(set) var activeTool = ""
  private(set) var actions: [JSON] = []
  /** Changes when what Blender's menus show may have: the active undo step moves with every
   * change an operator makes, once per action. */
  private(set) var menuContext = ""
  private(set) var brushes: JSON?
  private(set) var canUndo = false
  /** Adjust Last Operation: the last operator's settings, or none. */
  private(set) var redo: JSON?
  /** Blender's navigation gizmo: its center and radius, in fractions of the window's width and
   * height (center) and width (radius). */
  private(set) var navigationGizmo: [Double] = []
  /** The modes the active object has, as Blender's header lists them, and the current one. */
  private(set) var modes: [JSON] = []
  private(set) var objectMode = "OBJECT"
  /** The viewport's shading: `wireframe`, `solid`, `material` or `rendered`. */
  private(set) var shading = "solid"
  /** In mesh edit mode: `VERT`, `EDGE` or `FACE`. */
  private(set) var meshSelectMode = "VERT"
  /** Transform orientation, pivot, snapping and proportional editing. */
  private(set) var transformSettings: [String: String] = [:]
  /** The viewport's overlays, X-Ray, gizmos, statistics and wireframe toggles. */
  private(set) var viewport: JSON?
  /** The active tool's settings, which the top bar shows, and whether it has any. */
  private(set) var toolSettingsShown = false
  /** Blender's own interface is showing, as on a desktop computer. */
  private(set) var desktop = false
  /** Blender's viewport has shown its first frame. */
  private(set) var viewportReady = false
  private(set) var documentsAvailable = false
  /** The window's safe area, without the toolbar's: controls laid out over the whole window,
   * such as the view pie, keep to it. */
  var safeAreaInsets = UIEdgeInsets.zero
  /** The asset pane and timeline along the bottom: their height, which the controls resting on
   * the bottom edge rise above. */
  var bottomTray: CGFloat = 0
  /** Which panels show. */
  var inspectorShown = false
  var hierarchyShown = false
  var timelineShown = false
  var assetsShown = false
  /** Full screen: everything but the button that brings it back is hidden. */
  var interfaceHidden = false
  /** The tool rail's width, which the controls beside it keep clear of. */
  var toolRailWidth: CGFloat = 64
  /** The room the toolbar leaves for the tool settings strip between its other items. */
  var toolStripRoom: CGFloat = 320
  /** A narrow window, such as a phone's: the toolbar keeps its most used items and puts the rest
   * in one menu. */
  var compact = false
  /** The viewport shading and tool settings popovers are open. */
  var shadingOpen = false
  var toolSettingsOpen = false
  /** One finger uses the active tool rather than orbiting the view. */
  var fingerUsesTool = false

  /** Parts of the snapshot that other objects show: the render sheet, settings and tool settings,
   * which says whether the tool has any. */
  @ObservationIgnored var onRender: ((Any?) -> Void)?
  @ObservationIgnored var onPreferences: ((Any?) -> Void)?
  @ObservationIgnored var onToolSettings: ((JSON, NSNumber) -> Bool)?

  init(window: UnsafeMutableRawPointer?) {
    self.window = window
  }

  /** Assign only a changed value: observers redraw on every assignment. */
  private func set<Value>(_ keyPath: ReferenceWritableKeyPath<ShellModel, Value>, _ value: Value,
                          equal: (Value, Value) -> Bool)
  {
    if !equal(self[keyPath: keyPath], value) { self[keyPath: keyPath] = value }
  }

  private func set<Value: Equatable>(_ keyPath: ReferenceWritableKeyPath<ShellModel, Value>,
                                     _ value: Value)
  {
    set(keyPath, value, equal: ==)
  }

  /** The toolbar snapshot, Blender's latest or a copy of it. */
  func update(toolbar: JSON) {
    set(\.mode, toolbar.string("mode") ?? "")
    set(\.tools, ((toolbar["tools"] as? [Any]) ?? []).map { $0 as? JSON },
        equal: { jsonEqual($0.map { $0 as Any? ?? NSNull() } as NSArray, $1.map { $0 as Any? ?? NSNull() } as NSArray) })
    set(\.activeTool, toolbar.string("active") ?? "")
    set(\.actions, toolbar.array("actions"), equal: jsonListEqual)
    set(\.brushes, toolbar.dict("brushes"), equal: { jsonEqual($0, $1) })
    set(\.canUndo, toolbar.bool("canUndo"))
    set(\.menuContext, "\(toolbar.string("objectMode") ?? "")|\(toolbar.string("menuState") ?? "")")
    set(\.redo, toolbar.dict("redo"), equal: { jsonEqual($0, $1) })
    let gizmo = (toolbar["navigationGizmo"] as? [NSNumber])?.map(\.doubleValue) ?? []
    if gizmo.count == 3 { set(\.navigationGizmo, gizmo) }
    let modes = toolbar.array("modes")
    if !modes.isEmpty { set(\.modes, modes, equal: jsonListEqual) }
    set(\.objectMode, toolbar.string("objectMode") ?? "OBJECT")
    if let shading = toolbar.string("shading") { set(\.shading, shading) }
    if let select = toolbar.string("meshSelectMode") { set(\.meshSelectMode, select) }
    if let settings = toolbar["settings"] as? [String: String] { set(\.transformSettings, settings) }
    set(\.viewport, toolbar.dict("viewport"), equal: { jsonEqual($0, $1) })
    onRender?(toolbar["render"])
    onPreferences?(toolbar["preferences"])
    if let settings = onToolSettings?(toolbar.dict("toolSettings") ?? [:],
                                      toolbar["propertiesAck"] as? NSNumber ?? 0)
    {
      set(\.toolSettingsShown, settings)
    }
    if toolbar.has("desktop") {
      set(\.desktop, toolbar.bool("desktop"))
    }
  }

  /** Take Blender's toolbar snapshot when it changed, or copy it when `copy`. */
  func pullToolbar(copy: Bool = false) {
    guard let json = copy ? WM_apple_ios_ui_toolbar_copy_json() : WM_apple_ios_ui_toolbar_take_json()
    else { return }
    let data = Data(bytes: json, count: strlen(json))
    WM_apple_ios_ui_toolbar_free_json(json)
    if let toolbar = try? JSONSerialization.jsonObject(with: data) as? JSON {
      update(toolbar: toolbar)
    }
  }

  /** Read without waiting for Blender: the presentation mailbox and the launcher. */
  func refreshWindowState() {
    if !viewportReady && wm_apple_ios_window_presented_frame(window) {
      viewportReady = true
    }
    set(\.documentsAvailable, wm_apple_ios_documents_available())
  }
}
