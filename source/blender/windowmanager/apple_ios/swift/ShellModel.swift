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
  private(set) var tools: [JSON] = []
  private(set) var activeTool = ""
  private(set) var actions: [JSON] = []
  private(set) var brushes: JSON?
  private(set) var canUndo = false
  /** Adjust Last Operation: the last operator's settings, or none. */
  private(set) var redo: JSON?
  /** Blender's own interface is showing, as on a desktop computer. */
  private(set) var desktop = false
  /** Blender's viewport has shown its first frame. */
  private(set) var viewportReady = false
  private(set) var documentsAvailable = false

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
    set(\.tools, toolbar.array("tools"), equal: jsonListEqual)
    set(\.activeTool, toolbar.string("active") ?? "")
    set(\.actions, toolbar.array("actions"), equal: jsonListEqual)
    set(\.brushes, toolbar.dict("brushes"), equal: { jsonEqual($0, $1) })
    set(\.canUndo, toolbar.bool("canUndo"))
    set(\.redo, toolbar.dict("redo"), equal: { jsonEqual($0, $1) })
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
