/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Test harness for the SwiftUI panels (`source/blender/windowmanager/apple_ios/swift`): the production panels over
 * a stand-in viewport, fed by a small engine that answers the native requests from fixtures the
 * way Blender does (see `test_swiftui_panels.py`). Requests are logged for the test to check.
 * Test-only: never part of the application.
 */

import SwiftUI
import UIKit

/* -------------------------------------------------------------------- */
/** \name Engine inbox stand-ins
 * \{ */

@_cdecl("WM_apple_ios_ui_setting_enqueue")
func harnessSettingEnqueue(_ setting: UnsafePointer<CChar>, _ value: UnsafePointer<CChar>) {
  let name = String(cString: setting)
  let text = String(cString: value)
  DispatchQueue.main.async { FakeEngine.shared.receive(name, text) }
}

@_cdecl("WM_apple_ios_ui_operator_enqueue")
func harnessOperatorEnqueue(_ name: UnsafePointer<CChar>) {
  FakeEngine.shared.log(["setting": "operator", "value": String(cString: name)])
}

@_cdecl("WM_apple_ios_ui_asset_drag")
func harnessAssetDrag(_ phase: Int32, _ asset: UnsafePointer<CChar>?, _ x: Int32, _ y: Int32) {
  FakeEngine.shared.log(["setting": "asset_drag", "value": ["phase": Int(phase),
                                                             "id": asset.map { String(cString: $0) } ?? ""]])
}

@_cdecl("wm_apple_ios_document_url")
func harnessDocumentURL() -> NSURL? { nil }

@_cdecl("GHOST_ios_engine_continue_work")
func harnessContinueWork(_ continues: Bool) {
  FakeEngine.shared.log(["setting": "continue_work", "value": continues])
}

@_cdecl("WM_apple_ios_ui_command_enqueue")
func harnessCommandEnqueue(_ command: Int32) {
  FakeEngine.shared.log(["setting": "command", "value": Int(command)])
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Engine
 * \{ */

@MainActor
final class FakeEngine {
  static let shared = FakeEngine()

  var fixtures: JSON = [:]
  var tab = "OBJECT"
  var inspectorShown = false
  var shadingShown = false
  var ack: UInt64 = 0
  var hierarchy: [JSON] = []
  weak var panels: SwiftScenePanels?
  var redo: RedoStore?
  var timeline: SwiftTimeline?
  var assets: SwiftAssets?
  let logURL: URL = {
    FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("requests.jsonl")
  }()

  init() {
    try? FileManager.default.removeItem(at: logURL)
    FileManager.default.createFile(atPath: logURL.path, contents: nil)
  }

  nonisolated func log(_ entry: JSON) {
    guard let data = try? JSONSerialization.data(withJSONObject: entry),
          let handle = try? FileHandle(forWritingTo: FileManager.default.urls(
            for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("requests.jsonl"))
    else { return }
    handle.seekToEndOfFile()
    handle.write(data + Data("\n".utf8))
    try? handle.close()
  }

  func load() {
    guard let url = Bundle.main.url(forResource: "fixtures", withExtension: "json"),
          let data = try? Data(contentsOf: url),
          let object = try? JSONSerialization.jsonObject(with: data) as? JSON
    else { return }
    fixtures = object
    hierarchy = object.array("hierarchy")
  }

  private func inspector(_ tab: String) -> JSON? {
    fixtures.dict("tabs")?.dict(tab)
  }

  func receive(_ name: String, _ text: String) {
    let payload = (try? JSONSerialization.jsonObject(with: Data(text.utf8))) as? JSON
    log(["setting": name, "value": payload ?? text])
    switch name {
      case "native_panels":
        inspectorShown = (Int(text) ?? 0) & 1 != 0
        publish()
      case "native_properties_state":
        if let tab = payload?.string("tab") { self.tab = tab }
        if let expanded = payload?.strings("expanded"), var inspector = inspector(tab) {
          inspector["sections"] = inspector.array("sections").map { section in
            var section = section
            section["open"] = expanded.contains(section.string("id") ?? "") ? 1 : 0
            return section
          }
          setInspector(tab, inspector)
        }
        publish()
      case "native_shading_visible":
        shadingShown = text == "1"
        publish()
      case "native_shading_state":
        publish()
      case "native_redo_edit":
        if let payload, var redo = fixtures.dict("redo") {
          if let key = payload.string("key"), let value = payload["value"] {
            redo["properties"] = redo.array("properties").map { property in
              var property = property
              if property.string("key") == key {
                property["value"] = value
                if property.has("options") {
                  property["options"] = property.array("options").map { option in
                    var option = option
                    option["checked"] = jsonEqual(option["value"], value) ? 1 : 0
                    return option
                  }
                }
              }
              return property
            }
          }
          redo["ack"] = payload["sequence"] ?? 0
          fixtures["redo"] = redo
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { self.publish() }
      case "native_properties_edit", "native_panel_edit":
        if let payload {
          apply(payload)
          ack = max(ack, (payload["sequence"] as? NSNumber)?.uint64Value ?? 0)
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { self.publish() }
      default:
        break
    }
  }

  private func setInspector(_ tab: String, _ inspector: JSON) {
    var tabs = fixtures.dict("tabs") ?? [:]
    tabs[tab] = inspector
    fixtures["tabs"] = tabs
  }

  /** Blender's answer to an edit: the property takes the value, lists change. */
  private func apply(_ request: JSON) {
    if request.string("action") == "select", let id = request.string("id") {
      let extend = request.bool("extend")
      hierarchy = hierarchy.map { node in
        var node = node
        if node.string("kind") == "object" {
          if node.string("id") == id {
            node["selected"] = request.bool("value") ? 1 : 0
            node["active"] = 1
          }
          else {
            if !extend { node["selected"] = 0 }
            node["active"] = 0
          }
        }
        return node
      }
      return
    }
    if request.string("action") == "visibility", let id = request.string("id") {
      hierarchy = hierarchy.map { node in
        var node = node
        if node.string("id") == id { node["hidden"] = request.bool("value") ? 1 : 0 }
        return node
      }
      return
    }
    guard let key = request.string("key") else { return }
    for name in ["tool", "shading"] {
      guard var inspector = fixtures.dict(name) else { continue }
      inspector["sections"] = inspector.array("sections").map { section in
        var section = section
        section["properties"] = section.array("properties").map { edit($0, key: key, request: request) }
        return section
      }
      fixtures[name] = inspector
    }
    for tab in (fixtures.dict("tabs") ?? [:]).keys {
      guard var inspector = inspector(tab) else { continue }
      inspector["sections"] = inspector.array("sections").map { section in
        var section = section
        section["properties"] = section.array("properties").map { edit($0, key: key, request: request) }
        return section
      }
      setInspector(tab, inspector)
    }
  }

  private func edit(_ property: JSON, key: String, request: JSON) -> JSON {
    var property = property
    if property.string("type") == "LIST" {
      if property.string("key") == key {
        if request.string("list_action") == "move",
           let from = request.int("from"), let to = request.int("to")
        {
          var rows = property.array("rows")
          let row = rows.remove(at: from)
          rows.insert(row, at: to)
          property["rows"] = rows.enumerated().map { index, row in
            var row = row
            row["index"] = index
            return row
          }
          property["value"] = to
        }
        else if let value = request["value"] {
          property["value"] = value
        }
        return property
      }
      if property.dict("add")?.string("key") == key {
        var rows = property.array("rows")
        rows.append(["index": rows.count, "label": "Key \(rows.count)", "icon": "SHAPEKEY_DATA",
                     "rename_key": "rename.\(rows.count)", "controls": rows.last?["controls"] ?? []])
        property["rows"] = rows
        property["value"] = rows.count - 1
        return property
      }
      if property.dict("remove")?.string("key") == key {
        var rows = property.array("rows")
        let active = property.int("value") ?? 0
        if rows.indices.contains(active) { rows.remove(at: active) }
        property["rows"] = rows.enumerated().map { index, row in
          var row = row
          row["index"] = index
          return row
        }
        property["value"] = max(0, min(active, rows.count - 1))
        return property
      }
      property["rows"] = property.array("rows").map { row in
        var row = row
        if row.string("rename_key") == key, let value = request["value"] { row["label"] = value }
        row["controls"] = row.array("controls").map { control in
          var control = control
          if control.string("key") == key, let value = request["value"] { control["value"] = value }
          return control
        }
        return row
      }
      return property
    }
    if property.string("key") == key, let value = request["value"] {
      property["value"] = value
    }
    if var lock = property.dict("lock"), lock.string("key") == key, let value = request["value"] {
      lock["value"] = value
      property["lock"] = lock
    }
    if var mode = property.dict("group_menu"), mode.string("key") == key, let value = request["value"] {
      mode["value"] = value
      property["group_menu"] = mode
    }
    return property
  }

  func publish() {
    guard let panels else { return }
    var snapshot: JSON = ["ack": NSNumber(value: ack), "error": "", "hierarchy": hierarchy]
    if inspectorShown, var inspector = inspector(tab) {
      inspector["tabs"] = fixtures.array("tab_list")
      inspector["tab"] = tab
      inspector["id"] = "\(tab):1"
      snapshot["inspector"] = inspector
    }
    if shadingShown, let shading = fixtures.dict("shading") {
      snapshot["shading"] = shading
    }
    let lists = (snapshot.dict("inspector")?.array("sections") ?? []).flatMap { $0.array("properties") }
      .filter { $0.string("type") == "LIST" }.map { "\($0.string("key") ?? "")=\($0["value"] ?? "")" }
    log(["published": NSNumber(value: ack), "lists": lists])
    panels.updateSnapshot(snapshot as NSDictionary)
    if let shelf = fixtures.dict("assets") { assets?.updateSnapshot(shelf as NSDictionary) }
    if let frames = fixtures.dict("timeline") { timeline?.updateSnapshot(frames as NSDictionary) }
    if let redo = fixtures.dict("redo") { self.redo?.update(redo) }
    if let tool = fixtures.dict("tool") {
      _ = panels.updateToolSettingsStores(tool, acknowledgement: NSNumber(value: ack))
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name App
 * \{ */

/** A stand-in for Blender's viewport: a grid. */
struct StandInViewport: View {
  var body: some View {
    Canvas { context, size in
      context.fill(Path(CGRect(origin: .zero, size: size)), with: .color(white: 0.24))
      var grid = Path()
      for x in stride(from: 0, through: size.width, by: 40) {
        grid.move(to: CGPoint(x: x, y: 0))
        grid.addLine(to: CGPoint(x: x, y: size.height))
      }
      for y in stride(from: 0, through: size.height, by: 40) {
        grid.move(to: CGPoint(x: 0, y: y))
        grid.addLine(to: CGPoint(x: size.width, y: y))
      }
      context.stroke(grid, with: .color(white: 0.3))
    }
    .ignoresSafeArea()
  }
}

/** Blender's context menu for the Pencil menu, as the view controller resolves it. */
final class PencilItems: NSObject, GHOSTIOSPencilMenuDelegate {
  @objc(pencilMenu:requestItemsForRoute:atPoint:completion:)
  func pencilMenu(_ menu: (UIView & GHOSTIOSPencilMenu)!, requestItemsForRoute route: [AnyHashable: Any]!, at point: CGPoint,
                  completion: (([[AnyHashable: Any]]?) -> Void)!)
  {
    let names = ["Shade Smooth", "Shade Flat", "Convert To", "Set Origin", "Duplicate", "Join", "Apply",
                 "Parent", "Snap", "Hide", "Delete"]
    var items: [[AnyHashable: Any]] = [["heading": "Object"]]
    for name in names {
      var item: [AnyHashable: Any] = ["title": name, "operator": "object." + name.lowercased()]
      if name == "Delete" { item["destructive"] = true }
      if ["Convert To", "Set Origin", "Apply", "Parent", "Snap"].contains(name) { item["submenu"] = true }
      if name == "Shade Smooth" { item["checked"] = true }
      items.append(item)
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { completion(items) }
  }

  @objc(pencilMenu:didChooseItem:atPoint:)
  func pencilMenu(_ menu: (UIView & GHOSTIOSPencilMenu)!, didChooseItem item: [AnyHashable: Any]!, at point: CGPoint) {}
}

/** Which panels show, as the document window's model keeps it. */
@MainActor
@Observable
final class HarnessState {
  var hierarchyShown = false
  var inspectorShown = false
  var shadingOpen = false
  var redoOpen = false
}

/** The panels as the document window places them: Properties and the Outliner in columns beside
 * the viewport, the tool settings and shading in the toolbar, and the timeline, asset pane and
 * Adjust Last Operation along the bottom. */
struct HarnessView: View {
  let panels: SwiftScenePanels
  let timeline: SwiftTimeline
  let assets: SwiftAssets
  @ObservedObject var redo: RedoStore
  @Bindable var state: HarnessState

  var body: some View {
    NavigationStack {
      HStack(spacing: 0) {
        if state.hierarchyShown {
          HierarchyPanel(store: panels.hierarchy) { panels.setHierarchyShown(false) }
            .environment(\.panelInWindow, true)
            .frame(width: 320)
            .background(Color(uiColor: .secondarySystemBackground).ignoresSafeArea())
        }
        ZStack {
          StandInViewport()
          VStack(spacing: 8) {
            Spacer()
            if state.redoOpen {
              RedoContent(store: redo)
                .frame(height: 60)
                .panelGlass(in: RoundedRectangle(cornerRadius: 20, style: .continuous))
            }
            if timeline.store.visible { TimelinePanel(store: timeline.store) }
            if assets.store.visible { AssetsPanel(store: assets.store, compact: false) }
          }
          .padding(16)
        }
        if state.inspectorShown {
          InspectorPanel(store: panels.inspector) { panels.setInspectorShown(false) }
            .environment(\.panelInWindow, true)
            .frame(width: 336)
            .background(Color(uiColor: .secondarySystemBackground).ignoresSafeArea())
        }
      }
      .toolbar {
        ToolbarItem(placement: .topBarLeading) {
          Button("Outliner", systemImage: "sidebar.leading") { panels.toggleHierarchy() }
        }
        ToolbarItem(placement: .principal) {
          ToolStrip(store: panels.toolStripStore)
            .padding(.horizontal, 12)
            .panelGlass(in: Capsule())
        }
        ToolbarItem(placement: .topBarTrailing) {
          Button("Viewport Shading", systemImage: "circle.lefthalf.filled") { state.shadingOpen = true }
            .accessibilityIdentifier("harness.shading")
            .popover(isPresented: $state.shadingOpen) {
              panels.shadingContent { state.shadingOpen = false }
            }
            .onChange(of: state.shadingOpen) { _, open in panels.setShadingShown(open) }
        }
        ToolbarItem(placement: .topBarTrailing) {
          Button("Properties", systemImage: "sidebar.trailing") { panels.toggleInspector() }
        }
      }
      .toolbarTitleDisplayMode(.inline)
    }
  }
}

final class HarnessController: UIHostingController<HarnessView> {
  let pencilItems = PencilItems()
  var renderSheet: SwiftRender?
  var pencilMenu: UIView?
  let panels = SwiftScenePanels()
  let timeline = SwiftTimeline()
  let assets = SwiftAssets()
  let redo = RedoStore()
  let state = HarnessState()
  let scenario = UserDefaults.standard.string(forKey: "scenario") ?? "object"

  init() {
    super.init(rootView: HarnessView(panels: panels, timeline: timeline, assets: assets, redo: redo, state: state))
    let engine = FakeEngine.shared
    engine.load()
    engine.panels = panels
    engine.redo = redo
    engine.timeline = timeline
    engine.assets = assets
    engine.tab = UserDefaults.standard.string(forKey: "tab") ?? "OBJECT"
    panels.panelsDidChange = { [weak self] in
      guard let self else { return }
      state.hierarchyShown = panels.hierarchyShown
      state.inspectorShown = panels.inspectorShown
    }
    redo.close = { [weak self] in self?.state.redoOpen = false }
    engine.publish()
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

  override func viewDidAppear(_ animated: Bool) {
    super.viewDidAppear(animated)
    switch scenario {
      case "hierarchy":
        panels.setHierarchyShown(true)
      case "both":
        panels.setHierarchyShown(true)
        panels.setInspectorShown(true)
      case "shading":
        state.shadingOpen = true
      case "tool":
        break
      case "timeline", "timeline-expanded":
        timeline.toggle()
        if scenario == "timeline-expanded" { timeline.toggleExpanded() }
      case "render":
        let render = SwiftRender(presenter: self)
        renderSheet = render
        render.renderAnimation(false)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
          render.updateStatus(["id": 1, "state": "rendering", "total": 1, "done": 0, "samples": 0.42])
        }
      case "templates":
        let templates: [NSDictionary] = [
          ["id": "general", "title": "General"], ["id": "sculpt", "title": "Sculpting"],
          ["id": "2d", "title": "2D Animation"], ["id": "vfx", "title": "VFX"],
          ["id": "character", "title": "Character Animation",
           "attribution": ["title": "Ellie", "author": "Blender Studio", "license": "CC BY 4.0",
                           "licenseURL": "https://creativecommons.org/licenses/by/4.0/",
                           "sourceURL": "https://studio.blender.org", "changes": "Simplified for iPad."]],
          ["id": "mine", "title": "My Room", "personal": true]]
        let picker = TemplatePickerController(templates: { templates },
                                              manage: { _, _, _, done in done(true) },
                                              completion: { FakeEngine.shared.log(["template": $0 ?? "cancel"]) })
        picker.modalPresentationStyle = .formSheet
        present(picker, animated: false)
      case "pencil":
        let menu = SwiftPencilMenu(frame: view.bounds)
        menu.delegate = pencilItems
        view.addSubview(menu)
        pencilMenu = menu
        menu.toggle(withPencilLocation: NSValue(cgPoint: CGPoint(x: view.bounds.midX, y: view.bounds.midY)))
      case "dialog":
        let sheet = DialogSheetController(
          title: "Save changes before closing?", message: "Your changes will be lost if you don't save them.",
          toggles: ["Save modified images"], selected: [NSNumber(value: true)],
          actions: ["Save", "Don't Save", "Cancel"], roles: [0, 2, 1].map { NSNumber(value: $0) }, preferred: 0
        ) { action, toggles in FakeEngine.shared.log(["dialog": action, "toggles": NSNumber(value: toggles)]) }
        sheet.modalPresentationStyle = .formSheet
        sheet.preferredContentSize = CGSize(width: 520, height: 600)
        present(sheet, animated: false)
      case "assets":
        assets.toggle()
      case "redo":
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { self.state.redoOpen = true }
      default:
        panels.setInspectorShown(true)
    }
  }
}

@main
final class HarnessDelegate: UIResponder, UIApplicationDelegate {}

final class HarnessSceneDelegate: UIResponder, UIWindowSceneDelegate {
  var window: UIWindow?

  func scene(_ scene: UIScene, willConnectTo session: UISceneSession,
             options connectionOptions: UIScene.ConnectionOptions)
  {
    guard let scene = scene as? UIWindowScene else { return }
    let window = UIWindow(windowScene: scene)
    window.rootViewController = HarnessController()
    window.tintColor = .systemBlue
    window.overrideUserInterfaceStyle = UserDefaults.standard.string(forKey: "appearance") == "light" ?
      .light : .dark
    window.makeKeyAndVisible()
    self.window = window
  }
}

/** \} */
