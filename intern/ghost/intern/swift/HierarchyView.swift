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
  @Published var collapsed = Set<String>()
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
    collapsed.formIntersection(keyed.keys)
    withMotion(.smooth(duration: 0.22)) {
      nodes = incoming
      byKey = keyed
    }
  }

  private var parents: Set<String> {
    Set(nodes.compactMap { $0.string("parent") }.filter { !$0.isEmpty })
  }

  func hasChildren(_ node: JSON) -> Bool {
    guard let key = node.string("key") else { return false }
    return parents.contains(key)
  }

  /** Rows in outline order, without those under collapsed rows; a search shows the matches
   * and the collections leading to them. */
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
    var result: [JSON] = []
    var hiddenUnder: [String] = []
    for node in nodes {
      let key = node.string("key") ?? ""
      if let matches, !matches.contains(key) { continue }
      if hiddenUnder.contains(where: { key.hasPrefix($0 + "/") }) { continue }
      result.append(node)
      if matches == nil && collapsed.contains(key) { hiddenUnder.append(key) }
    }
    return result
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

  func toggle(_ node: JSON) {
    guard let key = node.string("key") else { return }
    withMotion(.smooth(duration: 0.22)) {
      if collapsed.contains(key) { collapsed.remove(key) }
      else { collapsed.insert(key) }
    }
  }

  func select(_ node: JSON) {
    guard node.string("kind") == "object", node.bool("enabled"), let id = node.string("id") else { return }
    let extend = editing
    let value = extend ? !node.bool("selected") : true
    send(["action": "select", "id": id, "extend": extend, "value": value])
    Haptics.selection()
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

  var body: some View {
    VStack(spacing: 0) {
      PanelHeader(title: "Outliner", close: close) {
        Button(store.editing ? "Done" : "Select") {
          withMotion { store.editing.toggle() }
        }
        /* A system button's title size. */
        .font(.system(size: 15, weight: store.editing ? .semibold : .regular))
        .accessibilityIdentifier("blender.hierarchy.select")
        UIMenuButton(title: nil, image: UIImage(systemName: "plus"),
                     accessibilityLabel: "Add Collection or Object", tint: PanelStyle.uiTint) { addMenu }
          .frame(width: 44, height: 44)
          .accessibilityIdentifier("blender.hierarchy.add")
      }
      SearchField(text: $store.query)
        .frame(height: 52)
        .padding(.horizontal, 4)
        .padding(.top, 3)
        .padding(.bottom, 2)
      outline
      if store.editing {
        selectionBar
          .transition(.move(edge: .bottom).combined(with: .opacity))
      }
      if !store.status.isEmpty {
        Text(store.status)
          .font(.footnote)
          .foregroundStyle(.red)
          .padding(12)
      }
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
    let rows = store.visible
    return List {
      ForEach(Array(rows.enumerated()), id: \.element.hierarchyKey) { index, node in
        HierarchyRow(store: store, node: node)
          .listRowInsets(EdgeInsets(top: 0, leading: 32, bottom: 0, trailing: 22))
          .listRowBackground(rowBackground(rows, index))
          .listRowSeparator(.hidden)
          .swipeActions(edge: .trailing, allowsFullSwipe: false) { trailingSwipe(node) }
          .swipeActions(edge: .leading, allowsFullSwipe: false) { leadingSwipe(node) }
          .contextMenu { menu(store.targets(node)) }
          .draggable(store.targets(node).compactMap { $0.string("id") }.joined(separator: "\n")) {
            Label(node.string("name") ?? "", systemImage: "cube")
              .padding(8)
              .glassEffect(.regular, in: Capsule())
          }
          .dropDestination(for: String.self) { drags, _ in
            /* Only rows of this Outliner match; other text dropped here does nothing. */
            let moving = store.nodes(withIDs: drags.flatMap { $0.components(separatedBy: "\n") })
            guard store.canMove(moving, to: node) else { return false }
            store.move(moving, to: node)
            return true
          }
      }
    }
    .listStyle(.plain)
    .scrollContentBackground(.hidden)
    .environment(\.defaultMinListRowHeight, 44)
    .scrollDismissesKeyboard(.immediately)
    .accessibilityIdentifier("blender.hierarchy.outline")
  }

  private func isSelected(_ node: JSON?) -> Bool {
    guard let node else { return false }
    return node.bool("selected") && node.string("kind") == "object"
  }

  /** Selected rows next to each other share one highlight, as in a sidebar list. */
  @ViewBuilder
  private func rowBackground(_ rows: [JSON], _ index: Int) -> some View {
    if isSelected(rows[index]) {
      let above = index > 0 && isSelected(rows[index - 1])
      let below = index + 1 < rows.count && isSelected(rows[index + 1])
      UnevenRoundedRectangle(topLeadingRadius: above ? 0 : 22, bottomLeadingRadius: below ? 0 : 22,
                             bottomTrailingRadius: below ? 0 : 22, topTrailingRadius: above ? 0 : 22,
                             style: .continuous)
        .fill(PanelStyle.selection)
        .padding(.horizontal, 16)
    }
    else {
      Color.clear
    }
  }

  @ViewBuilder
  private func trailingSwipe(_ node: JSON) -> some View {
    if store.allow([node], "delete") {
      Button(role: .destructive) { deleting = [node] } label: { Label("Delete", systemImage: "trash") }
    }
    if store.allow([node], "visibility") {
      let hidden = node.bool("hidden")
      Button { store.act("visibility", [node], value: !hidden) } label: {
        Label(hidden ? "Show" : "Hide", systemImage: hidden ? "eye" : "eye.slash")
      }
      .tint(.gray)
    }
  }

  @ViewBuilder
  private func leadingSwipe(_ node: JSON) -> some View {
    if store.allow([node], "duplicate") {
      Button { store.act("duplicate", [node]) } label: {
        Label("Duplicate", systemImage: "plus.square.on.square")
      }
      .tint(.blue)
    }
    if store.allow([node], "rename") {
      Button { promptRename(node) } label: { Label("Rename", systemImage: "pencil") }
        .tint(.gray)
    }
  }

  private func promptRename(_ node: JSON) {
    let collection = node.string("kind") == "collection"
    rename = RenameRequest(title: collection ? "Rename Collection" : "Rename Object",
                           name: node.string("name") ?? "") { name in
      store.act("rename", [node], value: name)
    }
  }

  @ViewBuilder
  private func menu(_ nodes: [JSON]) -> some View {
    let several = nodes.count > 1
    Section {
      if !several && store.allow(nodes, "rename") {
        Button { promptRename(nodes[0]) } label: { Label("Rename", systemImage: "pencil") }
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
    }
    moveMenu(nodes)
    if store.allow(nodes, "delete") {
      Section {
        Button(role: .destructive) { deleting = nodes } label: {
          Label(several ? "Delete \(nodes.count) Objects" : "Delete", systemImage: "trash")
        }
      }
    }
  }

  /** Collections the rows can move into, indented like the Outliner. */
  @ViewBuilder
  private func moveMenu(_ nodes: [JSON]) -> some View {
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

  private var selectionBar: some View {
    let nodes = store.selectedObjects
    let hidden = !nodes.isEmpty && nodes.allSatisfy { $0.bool("hidden") }
    return HStack {
      Button { store.act("duplicate", nodes) } label: { Image(systemName: "plus.square.on.square") }
        .disabled(!store.allow(nodes, "duplicate"))
        .accessibilityLabel("Duplicate")
      Spacer()
      Button { store.act("visibility", nodes, value: !hidden) } label: {
        Image(systemName: hidden ? "eye" : "eye.slash")
      }
      .disabled(!store.allow(nodes, "visibility"))
      .accessibilityLabel(hidden ? "Show" : "Hide")
      Spacer()
      Menu { moveMenu(nodes) } label: { Image(systemName: "folder") }
        .disabled(nodes.isEmpty)
        .accessibilityLabel("Move to Collection")
      Spacer()
      Button(role: .destructive) { deleting = nodes } label: { Image(systemName: "trash") }
        .tint(.red)
        .disabled(!store.allow(nodes, "delete"))
        .accessibilityLabel("Delete")
    }
    .font(.title3)
    .padding(.horizontal, 24)
    .frame(height: 50)
    .accessibilityIdentifier("blender.hierarchy.selectionBar")
  }
}

struct HierarchyRow: View {
  @ObservedObject var store: HierarchyStore
  let node: JSON

  var body: some View {
    let selected = node.bool("selected") && node.string("kind") == "object"
    let collection = node.string("kind") == "collection"
    let key = node.string("key") ?? ""
    let expanded = !store.collapsed.contains(key) || !store.query.isEmpty
    HStack(spacing: 0) {
      if store.editing && !collection {
        Image(systemName: selected ? "checkmark.circle.fill" : "circle")
          .foregroundStyle(selected ? Color.white : .secondary)
          .font(.title3)
          .padding(.trailing, 6)
          .transition(.move(edge: .leading).combined(with: .opacity))
      }
      Color.clear.frame(width: CGFloat(16 * max(0, node.int("depth") ?? 0)), height: 1)
      Group {
        if store.hasChildren(node) {
          Button {
            store.toggle(node)
          } label: {
            Image(systemName: "chevron.right")
              .font(.system(size: 11, weight: .semibold))
              .rotationEffect(.degrees(expanded ? 90 : 0))
              .frame(width: 20, height: 28)
              .contentShape(Rectangle())
          }
          .buttonStyle(.plain)
          .accessibilityLabel(expanded ? "Collapse" : "Expand")
        }
        else {
          Color.clear.frame(width: 20, height: 28)
        }
      }
      .foregroundStyle(selected ? Color.white : .secondary)
      /* As in Blender's Outliner, category icons keep their theme colors, selected too. */
      Group {
        if let image = BlenderIcon.image(node.string("icon"), themed: true) {
          Image(uiImage: image).resizable().scaledToFit()
        }
        else {
          Image(systemName: collection ? "folder" : "cube")
        }
      }
      .frame(width: 22, height: 22)
      .foregroundStyle(selected ? Color.white : .primary)
      .padding(.leading, 8)
      .padding(.trailing, 10)
      Text(node.string("name") ?? "")
        .lineLimit(2)
        .fontWeight(node.bool("active") ? .bold : .regular)
        .foregroundStyle(selected ? Color.white : node.bool("enabled") ? .primary : .secondary)
        .frame(maxWidth: .infinity, alignment: .leading)
      if node.bool("hidden") {
        Button {
          store.act("visibility", [node], value: false)
        } label: {
          Image(systemName: "eye.slash")
            .frame(width: 36, height: 36)
        }
        .buttonStyle(.plain)
        .padding(.leading, 6)
        .foregroundStyle(selected ? Color.white.opacity(0.8) : .secondary)
        .accessibilityLabel("Show in viewport")
      }
    }
    .frame(minHeight: 44)
    .contentShape(Rectangle())
    .onTapGesture { store.select(node) }
    .accessibilityElement(children: .combine)
    .accessibilityAddTraits(selected ? [.isSelected, .isButton] : .isButton)
    .accessibilityValue(node.bool("active") ? "Active" : "")
    .accessibilityIdentifier("blender.hierarchy." + (node.string("id") ?? ""))
  }
}

private extension Dictionary where Key == String, Value == Any {
  var hierarchyKey: String { string("key") ?? "" }
}

/** The system's minimal search bar, as the Outliner has always used. */
struct SearchField: UIViewRepresentable {
  @Binding var text: String

  func makeUIView(context: Context) -> UISearchBar {
    let bar = UISearchBar()
    bar.searchBarStyle = .minimal
    bar.placeholder = "Search"
    bar.autocorrectionType = .no
    bar.autocapitalizationType = .none
    bar.delegate = context.coordinator
    bar.accessibilityIdentifier = "blender.hierarchy.search"
    return bar
  }

  func updateUIView(_ bar: UISearchBar, context: Context) {
    context.coordinator.text = $text
    if bar.text != text { bar.text = text }
  }

  func makeCoordinator() -> Coordinator { Coordinator(text: $text) }

  final class Coordinator: NSObject, UISearchBarDelegate {
    var text: Binding<String>

    init(text: Binding<String>) { self.text = text }

    func searchBar(_ searchBar: UISearchBar, textDidChange searchText: String) {
      text.wrappedValue = searchText
    }

    func searchBarSearchButtonClicked(_ searchBar: UISearchBar) {
      searchBar.resignFirstResponder()
    }
  }
}
