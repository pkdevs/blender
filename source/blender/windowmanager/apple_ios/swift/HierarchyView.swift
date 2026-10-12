/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The Outliner: the scene's collections and objects as a tree. Tap to select, touch and hold or
 * swipe for actions, drag rows into a collection or onto an object to parent them, and use
 * Select mode to act on several at once, as in Mail and Files.
 */

import SwiftUI
import UIKit

@MainActor
final class HierarchyStore: ObservableObject {
  @Published private(set) var nodes: [JSON] = []
  @Published private(set) var byKey: [String: JSON] = [:]
  @Published var query = ""
  @Published var editing = false
  @Published private(set) var status = ""

  let send: (JSON) -> Void
  var menuProvider: ((NSDictionary) -> UIMenu?)?

  init(send: @escaping (JSON) -> Void) {
    self.send = send
  }

  func update(nodes incoming: [JSON], status: String) {
    self.status = status
    guard !(incoming as NSArray).isEqual(nodes as NSArray) else { return }
    var keyed: [String: JSON] = [:]
    for node in incoming { if let key = node.string("key") { keyed[key] = node } }
    withMotion(.smooth(duration: 0.22)) {
      nodes = incoming
      byKey = keyed
    }
  }

  /** Rows in outline order; a search shows the matches and the collections leading to them. */
  var visible: [JSON] {
    var matches: Set<String>?
    if !query.isEmpty {
      var found = Set<String>()
      for node in nodes where (node.string("name") ?? "").localizedStandardContains(query) {
        var key = node.string("key")
        while let current = key, !current.isEmpty, !found.contains(current) {
          found.insert(current)
          key = byKey[current]?.string("parent")
        }
      }
      matches = found
    }
    guard let matches else { return nodes }
    return nodes.filter { matches.contains($0.string("key") ?? "") }
  }

  var selectedObjects: [JSON] {
    var seen = Set<String>()
    return nodes.filter { node in
      guard node.string("kind") == "object", node.bool("selected"), let id = node.string("id") else {
        return false
      }
      return seen.insert(id).inserted
    }
  }

  /** The rows an action is about: the selection when the row is part of it. */
  func targets(_ node: JSON) -> [JSON] {
    let selected = selectedObjects
    if node.string("kind") == "object", selected.contains(where: { jsonEqual($0["id"], node["id"]) }) {
      return selected
    }
    return [node]
  }

  func allow(_ nodes: [JSON], _ action: String) -> Bool {
    !nodes.isEmpty && nodes.allSatisfy { $0.strings("actions").contains(action) }
  }

  func act(_ action: String, _ nodes: [JSON], value: Any? = nil) {
    guard allow(nodes, action) else { return }
    var request: JSON = ["action": action, "kind": nodes.first?.string("kind") ?? "object"]
    if nodes.count == 1 { request["id"] = nodes[0].string("id") ?? "" }
    else { request["ids"] = ids(nodes) }
    if let value { request["value"] = value }
    send(request)
  }

  private func ids(_ nodes: [JSON]) -> [String] {
    var seen = Set<String>()
    return nodes.compactMap { $0.string("id") }.filter { seen.insert($0).inserted }
  }

  /** Whether rows may move into or under `target`: not into themselves or their own children. */
  func canMove(_ moving: [JSON], to target: JSON?) -> Bool {
    guard let target, allow(moving, "move") else { return false }
    let kind = moving.first?.string("kind")
    for node in moving {
      if node.string("kind") != kind || jsonEqual(node["id"], target["id"]) { return false }
      /* Keys are paths of the rows above: a target inside a moved row is one of its children. */
      for row in nodes where jsonEqual(row["id"], node["id"]) {
        if (target.string("key") ?? "").hasPrefix((row.string("key") ?? "") + "/") { return false }
      }
    }
    if target.string("kind") == "collection" { return target.bool("accepts") }
    /* Objects drop onto objects to be parented; collections only into collections. */
    return kind == "object" && target.string("kind") == "object" && target.bool("enabled")
  }

  func move(_ moving: [JSON], to target: JSON) {
    guard canMove(moving, to: target) else { return }
    send(["action": target.string("kind") == "collection" ? "move" : "parent",
          "kind": moving.first?.string("kind") ?? "object", "ids": ids(moving),
          "target": target.string("id") ?? ""])
    Haptics.impact()
  }

  func nodes(withIDs ids: [String]) -> [JSON] {
    ids.compactMap { id in nodes.first { $0.string("id") == id } }
  }
}

struct HierarchyPanel: View {
  @ObservedObject var store: HierarchyStore
  let close: () -> Void
  @State private var rename: RenameRequest?
  @State private var deleting: [JSON] = []
  @State private var naming = false
  @State private var collectionName = "Collection"
  @Environment(\.panelInWindow) private var inWindow

  /** A navigation view of its own, as in Files and Mail: its title, search, and the toolbar with
   * Select and Add, and in Select mode the actions on the selection along the bottom. */
  var body: some View {
    NavigationStack {
      outline
        .safeAreaInset(edge: .bottom) {
          if !store.status.isEmpty {
            Text(store.status)
              .font(.footnote)
              .foregroundStyle(.red)
              .padding(12)
          }
        }
        .navigationTitle("Outliner")
        .navigationBarTitleDisplayMode(.inline)
        .searchable(text: $store.query, placement: .navigationBarDrawer(displayMode: .always))
        .toolbar {
          ToolbarItem(placement: .topBarLeading) {
            Button(store.editing ? "Done" : "Select") {
              withMotion { store.editing.toggle() }
            }
            .fontWeight(store.editing ? .semibold : .regular)
            .accessibilityIdentifier("blender.hierarchy.select")
          }
          ToolbarItem(placement: .topBarTrailing) {
            /* Blender's Add menu, built when it opens. */
            UIMenuButton(title: nil, image: UIImage(systemName: "plus"),
                         accessibilityLabel: "Add Collection or Object", tint: PanelStyle.uiTint) { addMenu }
              .frame(width: 44, height: 44)
              .accessibilityIdentifier("blender.hierarchy.add")
          }
          if !inWindow {
            ToolbarItem(placement: .topBarTrailing) {
              Button("Close Outliner", systemImage: "xmark", action: close)
                .keyboardShortcut(.cancelAction)
            }
          }
          if store.editing {
            ToolbarItemGroup(placement: .bottomBar) { selectionActions }
          }
        }
        .toolbarBackgroundVisibility(.hidden, for: .navigationBar, .bottomBar)
    }
    .renamePrompt($rename)
    .alert(deleteTitle, isPresented: Binding(get: { !deleting.isEmpty }, set: { if !$0 { deleting = [] } })) {
      Button("Cancel", role: .cancel) { deleting = [] }
      Button("Delete", role: .destructive) {
        store.act("delete", deleting)
        deleting = []
      }
    } message: {
      Text(deleteMessage)
    }
    .alert("New Collection", isPresented: $naming) {
      TextField("Collection name", text: $collectionName)
        .textInputAutocapitalization(.never)
      Button("Cancel", role: .cancel) {}
      Button("Create") {
        store.send(["action": "collection_new", "value": collectionName])
      }
      .disabled(collectionName.trimmingCharacters(in: .whitespaces).isEmpty)
    }
  }

  private var addMenu: UIMenu {
    let collection = UIAction(title: "New Collection", image: BlenderIcon.image("OUTLINER_COLLECTION"),
                              identifier: UIAction.Identifier("blender.hierarchy.newCollection")) { _ in
      collectionName = "Collection"
      naming = true
    }
    var children: [UIMenuElement] = [collection]
    if let objects = store.menuProvider?(["label": "Add Object", "menu": "VIEW3D_MT_add",
                                          "icon": "OBJECT_DATA"] as NSDictionary)
    {
      children.append(objects)
    }
    return UIMenu(children: children)
  }

  private var deleteTitle: String {
    if deleting.count > 1 { return "Delete \(deleting.count) Objects?" }
    return "Delete \u{201C}\(deleting.first?.string("name") ?? "")\u{201D}?"
  }

  private var deleteMessage: String {
    if deleting.count == 1 && deleting[0].string("kind") == "collection" {
      return "Its objects and collections move to the parent collection. You can undo this change."
    }
    return deleting.count > 1 ? "They will be removed from this scene. You can undo this change." :
      "The object will be removed from this scene. You can undo this change."
  }

  private var outline: some View {
    OutlineList(store: store, rename: promptRename, delete: { deleting = $0 })
  }

  private func promptRename(_ node: JSON) {
    let collection = node.string("kind") == "collection"
    rename = RenameRequest(title: collection ? "Rename Collection" : "Rename Object",
                           name: node.string("name") ?? "") { name in
      store.act("rename", [node], value: name)
    }
  }

  @ViewBuilder
  private var selectionActions: some View {
    let nodes = store.selectedObjects
    let hidden = !nodes.isEmpty && nodes.allSatisfy { $0.bool("hidden") }
    Button("Duplicate", systemImage: "plus.square.on.square") { store.act("duplicate", nodes) }
      .disabled(!store.allow(nodes, "duplicate"))
    Spacer()
    Button(hidden ? "Show" : "Hide", systemImage: hidden ? "eye" : "eye.slash") {
      store.act("visibility", nodes, value: !hidden)
    }
    .disabled(!store.allow(nodes, "visibility"))
    Spacer()
    Menu("Move to Collection", systemImage: "folder") { MoveToCollectionMenu(store: store, nodes: nodes) }
      .disabled(nodes.isEmpty)
    Spacer()
    Button("Delete", systemImage: "trash", role: .destructive) { deleting = nodes }
      .disabled(!store.allow(nodes, "delete"))
  }
}

/* -------------------------------------------------------------------- */
/** \name Outline
 *
 * The tree as a SwiftUI list: tap to select, Select mode or a two-finger swipe to select several
 * as in Mail, swipe and touch and hold for actions, and drag rows into a collection or onto an
 * object.
 * \{ */

struct OutlineList: View {
  @ObservedObject var store: HierarchyStore
  let rename: (JSON) -> Void
  let delete: ([JSON]) -> Void
  /** Shown at once, until Blender's snapshot confirms it. */
  @State private var selection = Set<String>()
  /** The row a drag is over, which takes the dragged rows. */
  @State private var dropTarget: String?
  /** Rows are open until closed, as in Blender's Outliner. */
  @State private var collapsed = Set<String>()

  private static func selected(_ nodes: [JSON]) -> Set<String> {
    Set(nodes.filter { $0.string("kind") == "object" && $0.bool("selected") }.compactMap { $0.string("key") })
  }

  var body: some View {
    List(selection: Binding(get: { selection }, set: choose)) {
      outline(OutlineItem.tree(store.visible))
    }
    .listStyle(.plain)
    .scrollContentBackground(.hidden)
    .environment(\.editMode, .constant(store.editing ? .active : .inactive))
    /* Published before it changes: the new nodes are the value sent. */
    .onReceive(store.$nodes) { nodes in selection = Self.selected(nodes) }
    .accessibilityIdentifier("blender.hierarchy.outline")
  }

  private func outline(_ items: [OutlineItem]) -> AnyView {
    AnyView(ForEach(items) { item in
      if let children = item.children {
        DisclosureGroup(isExpanded: Binding(
          get: { !store.query.isEmpty || !collapsed.contains(item.id) },
          set: { open in if open { collapsed.remove(item.id) } else { collapsed.insert(item.id) } }))
        {
          outline(children)
        } label: {
          row(item.node)
        }
      }
      else {
        row(item.node)
      }
    })
  }

  /** Selecting a row selects its object in Blender: one with a tap, several in Select mode. */
  private func choose(_ keys: Set<String>) {
    let added = keys.subtracting(selection), removed = selection.subtracting(keys)
    selection = keys
    if !store.editing {
      guard let key = added.first ?? keys.first, let id = store.byKey[key]?.string("id") else { return }
      store.send(["action": "select", "id": id, "extend": false, "value": true])
      Haptics.selection()
      return
    }
    for key in added { if let id = store.byKey[key]?.string("id") { store.send(["action": "select", "id": id, "extend": true, "value": true]) } }
    for key in removed { if let id = store.byKey[key]?.string("id") { store.send(["action": "select", "id": id, "extend": true, "value": false]) } }
  }

  private func row(_ node: JSON) -> some View {
    let key = node.keyString
    let collection = node.string("kind") == "collection"
    let selectable = !collection && node.bool("enabled")
    return HStack(spacing: 8) {
      Group {
        if let image = BlenderIcon.image(node.string("icon"), themed: true) {
          Image(uiImage: image).resizable().scaledToFit()
        }
        else {
          Image(systemName: collection ? "folder" : "cube")
        }
      }
      .frame(width: 22, height: 22)
      Text(node.string("name") ?? "")
        .foregroundStyle(node.bool("enabled") ? .primary : .secondary)
        .lineLimit(2)
      Spacer(minLength: 0)
      if node.bool("hidden") {
        Button { store.act("visibility", [node], value: false) } label: {
          Image(systemName: "eye.slash").foregroundStyle(.secondary).frame(width: 44, height: 32)
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Show in viewport")
      }
    }
    .selectionDisabled(!selectable)
    .listRowBackground(dropTarget == key ? Color.accentColor.opacity(0.25) : nil)
    .swipeActions(edge: .leading, allowsFullSwipe: false) { swipeButtons(node, ["duplicate", "rename"]) }
    .swipeActions(edge: .trailing, allowsFullSwipe: false) { swipeButtons(node, ["delete", "visibility"]) }
    .contextMenu { menu(store.targets(node)) }
    .draggable(DraggedRows.payload(store.targets(node))) {
      Label(node.string("name") ?? "", systemImage: collection ? "folder" : "cube").padding(8)
    }
    .dropDestination(for: String.self) { items, _ in
      let moving = items.flatMap(DraggedRows.keys).compactMap { store.byKey[$0] }
      guard store.canMove(moving, to: node) else { return false }
      store.move(moving, to: node)
      return true
    } isTargeted: { over in
      if over { dropTarget = key } else if dropTarget == key { dropTarget = nil }
    }
    .accessibilityIdentifier("blender.hierarchy." + (node.string("id") ?? ""))
    .accessibilityValue(node.bool("active") ? "Active" : "")
  }

  @ViewBuilder
  private func swipeButtons(_ node: JSON, _ actions: [String]) -> some View {
    ForEach(actions.filter { store.allow([node], $0) }, id: \.self) { action in
      let hidden = node.bool("hidden")
      switch action {
        case "delete":
          Button(role: .destructive) { delete([node]) } label: { Label("Delete", systemImage: "trash") }
        case "duplicate":
          Button { store.act("duplicate", [node]) } label: { Label("Duplicate", systemImage: "plus.square.on.square") }
            .tint(.blue)
        case "visibility":
          Button { store.act("visibility", [node], value: !hidden) } label: {
            Label(hidden ? "Show" : "Hide", systemImage: hidden ? "eye" : "eye.slash")
          }
          .tint(.gray)
        default:
          Button { rename(node) } label: { Label("Rename", systemImage: "pencil") }
            .tint(.gray)
      }
    }
  }

  @ViewBuilder
  private func menu(_ nodes: [JSON]) -> some View {
    let several = nodes.count > 1
    if !several && store.allow(nodes, "rename") {
      Button { rename(nodes[0]) } label: { Label("Rename", systemImage: "pencil") }
    }
    if store.allow(nodes, "duplicate") {
      Button { store.act("duplicate", nodes) } label: {
        Label(several ? "Duplicate \(nodes.count) Objects" : "Duplicate", systemImage: "plus.square.on.square")
      }
    }
    if store.allow(nodes, "visibility") {
      let hidden = nodes.allSatisfy { $0.bool("hidden") }
      Button { store.act("visibility", nodes, value: !hidden) } label: {
        Label(hidden ? "Show" : "Hide", systemImage: hidden ? "eye" : "eye.slash")
      }
    }
    MoveToCollectionMenu(store: store, nodes: nodes)
    if store.allow(nodes, "delete") {
      Divider()
      Button(role: .destructive) { delete(nodes) } label: {
        Label(several ? "Delete \(nodes.count) Objects" : "Delete", systemImage: "trash")
      }
    }
  }
}

/** A row of the outline and the rows under it, from the flat list Blender sends. */
struct OutlineItem: Identifiable {
  let node: JSON
  var children: [OutlineItem]?
  var id: String { node.keyString }

  static func tree(_ nodes: [JSON]) -> [OutlineItem] {
    var childrenOf: [String: [JSON]] = [:]
    var keys = Set<String>()
    for node in nodes { keys.insert(node.keyString) }
    var roots: [JSON] = []
    for node in nodes {
      let parent = node.string("parent") ?? ""
      if !parent.isEmpty && keys.contains(parent) { childrenOf[parent, default: []].append(node) }
      else { roots.append(node) }
    }
    func item(_ node: JSON) -> OutlineItem {
      let children = childrenOf[node.keyString]?.map(item)
      return OutlineItem(node: node, children: children)
    }
    return roots.map(item)
  }
}

/** Rows being dragged, as text naming their keys, which only this list reads. */
enum DraggedRows {
  private static let prefix = "blender.outliner.rows\n"

  static func payload(_ nodes: [JSON]) -> String {
    prefix + nodes.compactMap { $0.string("key") }.joined(separator: "\n")
  }

  static func keys(_ payload: String) -> [String] {
    guard payload.hasPrefix(prefix) else { return [] }
    return payload.dropFirst(prefix.count).split(separator: "\n").map(String.init)
  }
}

/** Collections the rows can move into, indented like the Outliner. */
struct MoveToCollectionMenu: View {
  @ObservedObject var store: HierarchyStore
  let nodes: [JSON]

  var body: some View {
    let targets = store.nodes.filter { $0.string("kind") == "collection" && store.canMove(nodes, to: $0) }
    if !targets.isEmpty {
      Menu {
        ForEach(Array(targets.enumerated()), id: \.offset) { _, target in
          Button {
            store.move(nodes, to: target)
          } label: {
            Label {
              Text(String(repeating: "  ", count: target.int("depth") ?? 0) + (target.string("name") ?? ""))
            } icon: {
              if let image = BlenderIcon.image("OUTLINER_COLLECTION") { Image(uiImage: image) }
            }
          }
        }
      } label: {
        Label("Move to Collection", systemImage: "folder")
      }
    }
  }
}

extension Dictionary where Key == String, Value == Any {
  var keyString: String { self["key"] as? String ?? "" }
}

/** \} */
