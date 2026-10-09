/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Test harness for the SwiftUI panels (`intern/ghost/intern/swift`): the production panels over
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
  var toolGroupChanged: ((UIBarButtonItemGroup?) -> Void)?
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
    panels.updateSnapshot(snapshot as NSDictionary)
    if let tool = fixtures.dict("tool") {
      toolGroupChanged?(panels.updateToolSettings(tool as NSDictionary,
                                                  acknowledgement: NSNumber(value: ack)))
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name App
 * \{ */

final class ViewportView: UIView {
  override func draw(_ rect: CGRect) {
    UIColor(white: 0.24, alpha: 1).setFill()
    UIRectFill(rect)
    UIColor(white: 0.3, alpha: 1).setStroke()
    let grid = UIBezierPath()
    for x in stride(from: 0, through: rect.width, by: 40) {
      grid.move(to: CGPoint(x: x, y: 0))
      grid.addLine(to: CGPoint(x: x, y: rect.height))
    }
    for y in stride(from: 0, through: rect.height, by: 40) {
      grid.move(to: CGPoint(x: 0, y: y))
      grid.addLine(to: CGPoint(x: rect.width, y: y))
    }
    grid.stroke()
  }
}

final class HarnessController: UIViewController {
  var panels: SwiftScenePanels!
  let overlay = UIView()
  let scenario = UserDefaults.standard.string(forKey: "scenario") ?? "object"

  override func loadView() {
    view = ViewportView()
    view.contentMode = .redraw
  }

  override func viewDidLoad() {
    super.viewDidLoad()
    overlay.frame = view.bounds
    overlay.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    view.addSubview(overlay)
    let engine = FakeEngine.shared
    engine.load()
    panels = SwiftScenePanels(overlay: overlay) { name in BlenderIcon.image(name) }
    panels.menuProvider = { property in
      let title = property["label"] as? String ?? ""
      return UIMenu(title: title, children: [
        UIAction(title: "Copy Shape Key", image: nil) { _ in engine.log(["menu": "copy"]) },
        UIAction(title: "Delete All Shape Keys", image: UIImage(systemName: "trash"),
                 attributes: .destructive) { _ in engine.log(["menu": "delete_all"]) },
      ])
    }
    engine.panels = panels
    engine.tab = UserDefaults.standard.string(forKey: "tab") ?? "OBJECT"
    let shading = UIBarButtonItem(image: UIImage(systemName: "circle.lefthalf.filled"), style: .plain,
                                  target: self, action: #selector(showShading(_:)))
    shading.accessibilityIdentifier = "harness.shading"
    navigationItem.leadingItemGroups = [UIBarButtonItemGroup.fixedGroup(items: [panels.hierarchyButton])]
    navigationItem.trailingItemGroups = [UIBarButtonItemGroup.fixedGroup(items: [shading, panels.inspectorButton])]
    navigationItem.title = "Blender"
    /* Center item groups, where Blender's tool settings sit, need an editor-style bar. */
    navigationItem.style = .editor
    engine.toolGroupChanged = { [weak self] group in
      guard let self, let group, !self.navigationItem.centerItemGroups.contains(group) else { return }
      self.navigationItem.centerItemGroups = [group]
    }
    engine.publish()
  }

  override func viewDidAppear(_ animated: Bool) {
    super.viewDidAppear(animated)
    switch scenario {
      case "hierarchy":
        tap(panels.hierarchyButton)
      case "both":
        tap(panels.hierarchyButton)
        tap(panels.inspectorButton)
      case "shading":
        showShading(navigationItem.trailingItemGroups.first?.barButtonItems.first as Any)
      case "tool":
        break
      default:
        tap(panels.inspectorButton)
    }
  }

  private func tap(_ item: UIBarButtonItem) {
    if let action = item.action { UIApplication.shared.sendAction(action, to: item.target, from: item, for: nil) }
  }

  @objc func showShading(_ sender: Any) {
    guard let item = sender as? UIBarButtonItem else { return }
    panels.presentShading(from: item, controller: self)
  }

  override func viewDidLayoutSubviews() {
    super.viewDidLayoutSubviews()
    panels.layout(in: view.bounds.inset(by: view.safeAreaInsets))
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
    let navigation = UINavigationController(rootViewController: HarnessController())
    window.rootViewController = navigation
    window.tintColor = .systemBlue
    window.overrideUserInterfaceStyle = UserDefaults.standard.string(forKey: "appearance") == "light" ?
      .light : .dark
    window.makeKeyAndVisible()
    self.window = window
  }
}

/** \} */
