/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Blender's own menus, such as Add, a modifier's or the context menu, as SwiftUI menus. Their
 * items come from Blender, which builds them from the scene, so they are asked for ahead of time,
 * a few levels deep, and again only when Blender says what they show may have changed. A menu
 * whose items come back the same is left alone, and items keep their identity, so a change
 * updates only the items that changed. Choosing an item runs it in Blender.
 */

import SwiftUI
import UIKit

struct BlenderMenuItem: Identifiable, Equatable {
  /** Its place in Blender's menus, which stays the same as the menu changes around it. */
  var id: String { route }
  let title: String
  let icon: String
  /** Where the item is in Blender's menus, to open it as a submenu or run it. */
  let route: String
  let submenu: Bool
  let disabled: Bool
  let checked: Bool
  /** A new section starts at this item. */
  let sectionStart: Bool
}

@MainActor
@Observable
final class BlenderMenus {
  static let shared = BlenderMenus()

  /** Each menu's items, by its route. */
  private(set) var items: [String: [BlenderMenuItem]] = [:]
  @ObservationIgnored private var loading = Set<String>()
  /** What the menus were last asked for with: other modes and selections list other items. */
  @ObservationIgnored private var context: String?

  /** The route of a menu by Blender's name, such as `VIEW3D_MT_add`, optionally in a Properties
   * tab. */
  static func route(_ menu: String, tab: String? = nil) -> String {
    var route: [String: Any] = ["root": menu, "path": []]
    if let tab, !tab.isEmpty { route["propertiesTab"] = tab }
    return encode(route)
  }

  /** The context menu for what is selected. */
  static let contextRoute = route("context")

  private static func encode(_ route: Any) -> String {
    guard let data = try? JSONSerialization.data(withJSONObject: route, options: .sortedKeys) else { return "" }
    return String(decoding: data, as: UTF8.self)
  }

  /** Ask Blender for a menu and the submenus under it, `depth` levels deep, unless they are known
   * for `context` already. */
  func prefetch(_ route: String, depth: Int = 3, context: String) {
    if context != self.context {
      self.context = context
      /* Keep showing the last items until the new ones arrive. */
      loading.removeAll()
      stale = Set(items.keys)
    }
    load(route, depth: depth)
  }

  @ObservationIgnored private var stale = Set<String>()

  private func load(_ route: String, depth: Int) {
    guard depth > 0, !loading.contains(route), items[route] == nil || stale.contains(route) else {
      for item in items[route] ?? [] where item.submenu && !item.disabled { load(item.route, depth: depth - 1) }
      return
    }
    loading.insert(route)
    Task {
      let list = await Self.request(route)
      loading.remove(route)
      stale.remove(route)
      guard let list else { return }
      if items[route] != list { items[route] = list }
      for item in list where item.submenu && !item.disabled { load(item.route, depth: depth - 1) }
    }
  }

  /** Run an item in Blender. */
  func run(_ item: BlenderMenuItem) {
    _ = item.route.withCString { WM_apple_ios_ui_menu_request($0, true) }
  }

  private static func request(_ route: String) async -> [BlenderMenuItem]? {
    let request = route.withCString { WM_apple_ios_ui_menu_request($0, false) }
    /* Blender answers between frames; it may be busy for a while, as when compiling shaders. */
    for _ in 0..<2400 {
      if let json = WM_apple_ios_ui_menu_take_result(request) {
        let data = Data(bytes: json, count: strlen(json))
        free(json)
        let list = (try? JSONSerialization.jsonObject(with: data)) as? [Any] ?? []
        return parse(list)
      }
      try? await Task.sleep(for: .milliseconds(25))
    }
    return nil
  }

  private static func parse(_ list: [Any]) -> [BlenderMenuItem] {
    var result: [BlenderMenuItem] = []
    var sectionStart = false
    for entry in list {
      guard let item = entry as? JSON else { continue }
      if item.bool("separator") {
        sectionStart = !result.isEmpty
        continue
      }
      guard let title = item.string("title"), !title.isEmpty, let route = item["route"] as? JSON else { continue }
      result.append(BlenderMenuItem(title: title, icon: item.string("icon") ?? "",
                                    route: encode(route), submenu: item.bool("submenu"),
                                    disabled: item.bool("disabled"), checked: item.bool("checked"),
                                    sectionStart: sectionStart))
      sectionStart = false
    }
    return result
  }
}

/** Changes when what Blender's menus show may have, from the toolbar snapshot. */
extension EnvironmentValues {
  @Entry var blenderMenuContext = ""
}

/** A Blender menu's items, in sections as Blender separates them. */
struct BlenderMenuItems: View {
  let route: String
  private let menus = BlenderMenus.shared

  var body: some View {
    if let items = menus.items[route] {
      if items.isEmpty {
        Button("No Available Commands") {}.disabled(true)
      }
      ForEach(sections(items), id: \.first?.id) { section in
        Section {
          ForEach(section) { item in entry(item) }
        }
      }
    }
    else {
      Button("Loading…") {}.disabled(true)
    }
  }

  private func sections(_ items: [BlenderMenuItem]) -> [[BlenderMenuItem]] {
    var result: [[BlenderMenuItem]] = []
    for item in items {
      if item.sectionStart || result.isEmpty { result.append([item]) }
      else { result[result.count - 1].append(item) }
    }
    return result
  }

  @ViewBuilder
  private func entry(_ item: BlenderMenuItem) -> some View {
    if item.submenu && !item.disabled {
      Menu {
        BlenderMenuItems(route: item.route)
      } label: {
        label(item)
      }
    }
    else if item.checked {
      Toggle(isOn: Binding(get: { true }, set: { _ in menus.run(item) })) { label(item) }
        .disabled(item.disabled)
    }
    else {
      Button { menus.run(item) } label: { label(item) }
        .disabled(item.disabled)
    }
  }

  private func label(_ item: BlenderMenuItem) -> some View {
    Label {
      Text(item.title)
    } icon: {
      if !item.icon.isEmpty, item.icon != "NONE", item.icon != "BLANK1", let image = BlenderIcon.image(item.icon) {
        Image(uiImage: image)
      }
    }
  }
}

extension View {
  /** Ask for the menu at `route` ahead of time, again as the mode or selection changes. */
  func prefetchingBlenderMenu(_ route: String) -> some View {
    modifier(BlenderMenuPrefetch(route: route))
  }
}

private struct BlenderMenuPrefetch: ViewModifier {
  let route: String
  @Environment(\.blenderMenuContext) private var context

  func body(content: Content) -> some View {
    content.onChange(of: context, initial: true) { _, context in
      BlenderMenus.shared.prefetch(route, context: context)
    }
  }
}

/** A property's menu, such as Add Modifier or a material slot's, with its title and icon. */
struct BlenderMenuButton: View {
  let property: JSON

  var body: some View {
    let route = BlenderMenus.route(property.string("menu") ?? "", tab: property.string("tab"))
    Menu {
      BlenderMenuItems(route: route)
    } label: {
      Label {
        Text(property.string("label") ?? "")
      } icon: {
        if let image = BlenderIcon.image(property.string("icon")) { Image(uiImage: image) }
      }
    }
    .disabled(!property.bool("editable"))
    .prefetchingBlenderMenu(route)
  }
}

/** Every action for what is selected, as Blender's right-click menu lists them. */
struct ContextMenuButton: View {
  var body: some View {
    Menu {
      BlenderMenuItems(route: BlenderMenus.contextRoute)
    } label: {
      Image(systemName: "ellipsis")
        .frame(width: 44, height: 44)
        .contentShape(Rectangle())
    }
    .tint(.primary)
    .prefetchingBlenderMenu(BlenderMenus.contextRoute)
    .accessibilityLabel("Actions")
    .accessibilityIdentifier("blender.context-menu")
  }
}
