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

  /** Whether rows sit under this one. */
  func hasChildren(_ node: JSON) -> Bool {
    let key = node.keyString
    return nodes.contains { $0.string("parent") == key }
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
  /** The row whose swipe actions show, and how far it is pulled. */
  @State private var swiped: String?
  @State private var swipeOffset: CGFloat = 0

  private static func selected(_ nodes: [JSON]) -> Set<String> {
    Set(nodes.filter { $0.string("kind") == "object" && $0.bool("selected") }.compactMap { $0.string("key") })
  }

  /** The rows in outline order, without those under closed rows; a search shows its matches. */
  private var rows: [JSON] {
    let nodes = store.visible
    guard store.query.isEmpty else { return nodes }
    var hiddenUnder: [String] = []
    return nodes.filter { node in
      let key = node.keyString
      if hiddenUnder.contains(where: { key.hasPrefix($0 + "/") }) { return false }
      if collapsed.contains(key) { hiddenUnder.append(key) }
      return true
    }
  }

  var body: some View {
    ScrollView {
      LazyVStack(spacing: 2) {
        ForEach(rows, id: \.keyString) { node in
          SwipeRow(id: node.keyString, swiped: $swiped, offset: $swipeOffset,
                   leading: swipeActions(node, ["duplicate", "rename"]),
                   trailing: swipeActions(node, ["visibility", "delete"])) {
            row(node)
          }
          .transition(.opacity)
        }
      }
      .padding(.horizontal, 8)
      .padding(.vertical, 4)
    }
#if !os(visionOS)
    .scrollDismissesKeyboard(.immediately)
#endif
    /* Published before it changes: the new nodes are the value sent. */
    .onReceive(store.$nodes) { nodes in selection = Self.selected(nodes) }
    .onChange(of: store.editing) { closeSwipe() }
    .accessibilityIdentifier("blender.hierarchy.outline")
  }

  private var animation: Animation? { UIAccessibility.isReduceMotionEnabled ? nil : .smooth(duration: 0.25) }

  private func closeSwipe() {
    withAnimation(animation) {
      swiped = nil
      swipeOffset = 0
    }
  }

  /** A tap selects the object; in Select mode it adds it to or takes it from the selection. */
  private func tap(_ node: JSON) {
    if swiped != nil {
      closeSwipe()
      return
    }
    let key = node.keyString
    guard node.string("kind") == "object", node.bool("enabled"), let id = node.string("id") else { return }
    if store.editing {
      let on = !selection.contains(key)
      if on { selection.insert(key) } else { selection.remove(key) }
      store.send(["action": "select", "id": id, "extend": true, "value": on])
    }
    else {
      selection = [key]
      store.send(["action": "select", "id": id, "extend": false, "value": true])
    }
    Haptics.selection()
  }

  private func toggle(_ key: String) {
    withAnimation(animation) {
      if collapsed.contains(key) { collapsed.remove(key) } else { collapsed.insert(key) }
    }
  }

  private func row(_ node: JSON) -> some View {
    let key = node.keyString
    let collection = node.string("kind") == "collection"
    let enabled = node.bool("enabled")
    /* Select mode marks the selection with its check circles instead. */
    let selected = selection.contains(key) && !store.editing
    let open = !store.query.isEmpty || !collapsed.contains(key)
    return HStack(spacing: 6) {
      if store.editing {
        Image(systemName: selection.contains(key) ? "checkmark.circle.fill" : "circle")
          .font(.title3)
          .foregroundStyle(selection.contains(key) ? Color.accentColor : .secondary)
          .opacity(!collection && enabled ? 1 : 0)
          .transition(.move(edge: .leading).combined(with: .opacity))
      }
      Color.clear.frame(width: CGFloat(14 * max(0, node.int("depth") ?? 0)), height: 1)
      Group {
        if store.hasChildren(node) {
          Button { toggle(key) } label: {
            Image(systemName: "chevron.right")
              .font(.footnote.weight(.semibold))
              .rotationEffect(.degrees(open ? 90 : 0))
              .frame(width: 22, height: 36)
              .contentShape(Rectangle())
          }
          .buttonStyle(.plain)
          .foregroundStyle(selected ? Color.white : .secondary)
          .accessibilityLabel(open ? "Collapse" : "Expand")
        }
        else {
          Color.clear
        }
      }
      .frame(width: 22)
      Group {
        if let image = BlenderIcon.image(node.string("icon"), themed: !selected) {
          Image(uiImage: image).resizable().scaledToFit()
        }
        else {
          Image(systemName: collection ? "folder" : "cube")
        }
      }
      .frame(width: 22, height: 22)
      Text(node.string("name") ?? "")
        .foregroundStyle(selected ? Color.white : enabled ? .primary : .secondary)
        .lineLimit(1)
      Spacer(minLength: 0)
      if node.bool("hidden") {
        Button { store.act("visibility", [node], value: false) } label: {
          Image(systemName: "eye.slash").frame(width: 36, height: 36)
        }
        .buttonStyle(.plain)
        .foregroundStyle(selected ? Color.white : .secondary)
        .accessibilityLabel("Show in viewport")
      }
    }
    .padding(.horizontal, 8)
    .frame(minHeight: 40)
    /* The selection as a rounded rectangle, as in a sidebar such as Notes'; a drag's target lit up
     * the same way. */
    .background(
      RoundedRectangle(cornerRadius: 10, style: .continuous)
        .fill(selected ? Color.accentColor : dropTarget == key ? Color.accentColor.opacity(0.3) : .clear))
    .contentShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
    .onTapGesture { tap(node) }
    .contextMenu { menu(store.targets(store.byKey[key] ?? node)) }
    /* Touch and hold lifts the row; moving it drags it into a collection or onto an object. */
    .onDrag {
      let rows = store.targets(store.byKey[key] ?? node)
      guard store.allow(rows, "move") else { return NSItemProvider() }
      return NSItemProvider(object: DraggedRows.payload(rows) as NSString)
    } preview: {
      /* The row as it looks, with its own icon. */
      HStack(spacing: 8) {
        if let image = BlenderIcon.image(node.string("icon"), themed: true) {
          Image(uiImage: image).resizable().scaledToFit().frame(width: 22, height: 22)
        }
        Text(node.string("name") ?? "")
      }
      .padding(.horizontal, 14)
      .padding(.vertical, 8)
      .background(.regularMaterial, in: Capsule())
    }
    .onDrop(of: [.plainText], isTargeted: Binding(get: { dropTarget == key }, set: { over in
      if over { dropTarget = key } else if dropTarget == key { dropTarget = nil }
    })) { providers in
      guard let provider = providers.first else { return false }
      _ = provider.loadObject(ofClass: NSString.self) { payload, _ in
        guard let payload = payload as? String else { return }
        DispatchQueue.main.async {
          guard let target = store.byKey[key] else { return }
          store.move(DraggedRows.keys(payload).compactMap { store.byKey[$0] }, to: target)
        }
      }
      return true
    }
    .accessibilityElement(children: .combine)
    .accessibilityAddTraits(selection.contains(key) ? [.isButton, .isSelected] : .isButton)
    .accessibilityIdentifier("blender.hierarchy." + (node.string("id") ?? ""))
  }

  /** The actions a swipe shows, as Blender allows them for the row now. */
  private func swipeActions(_ node: JSON, _ actions: [String]) -> [SwipeAction] {
    let node = store.byKey[node.keyString] ?? node
    let hidden = node.bool("hidden")
    return actions.filter { store.allow([node], $0) }.map { action in
      switch action {
        case "delete":
          return SwipeAction(title: "Delete", symbol: "trash", tint: .red) { delete([node]) }
        case "duplicate":
          return SwipeAction(title: "Duplicate", symbol: "plus.square.on.square", tint: .blue) {
            store.act("duplicate", [node])
          }
        case "visibility":
          return SwipeAction(title: hidden ? "Show" : "Hide", symbol: hidden ? "eye" : "eye.slash", tint: .gray) {
            store.act("visibility", [node], value: !hidden)
          }
        default:
          return SwipeAction(title: "Rename", symbol: "pencil", tint: .gray) { rename(node) }
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

struct SwipeAction {
  let title: String
  let symbol: String
  let tint: Color
  let run: () -> Void
}

/** A row that a sideways swipe pulls aside to show its actions, as a list's rows do. One row is
 * open at a time. */
struct SwipeRow<Content: View>: View {
  let id: String
  @Binding var swiped: String?
  @Binding var offset: CGFloat
  let leading: [SwipeAction]
  let trailing: [SwipeAction]
  @ViewBuilder let content: Content
  @State private var dragStart: CGFloat?

  private let buttonWidth: CGFloat = 104

  private var shown: CGFloat { swiped == id ? offset : 0 }

  var body: some View {
    ZStack {
      HStack(spacing: 6) {
        if shown > 0 { buttons(leading) }
        Spacer(minLength: 0)
        if shown < 0 { buttons(trailing) }
      }
      content
        .offset(x: shown)
    }
    .clipped()
    .simultaneousGesture(swipe)
  }

  private func buttons(_ actions: [SwipeAction]) -> some View {
    ForEach(actions, id: \.title) { action in
      Button {
        withAnimation(.smooth(duration: 0.25)) {
          swiped = nil
          offset = 0
        }
        action.run()
      } label: {
        Label(action.title, systemImage: action.symbol)
          .font(.subheadline.weight(.medium))
          .lineLimit(1)
          .foregroundStyle(.white)
          .frame(width: buttonWidth - 6, height: 36)
          .background(action.tint, in: Capsule())
      }
      .buttonStyle(.plain)
      .transition(.opacity)
    }
  }

  private var swipe: some Gesture {
    DragGesture(minimumDistance: 18)
      .onChanged { value in
        /* Sideways only; an upward or downward drag scrolls the list. */
        guard dragStart != nil || abs(value.translation.width) > abs(value.translation.height) * 1.5 else { return }
        if dragStart == nil {
          dragStart = shown
          if swiped != id {
            swiped = id
            offset = 0
          }
        }
        let leadingLimit = leading.isEmpty ? 0 : CGFloat(leading.count) * buttonWidth + 20
        let trailingLimit = trailing.isEmpty ? 0 : CGFloat(trailing.count) * buttonWidth + 20
        offset = max(-trailingLimit, min(leadingLimit, (dragStart ?? 0) + value.translation.width))
      }
      .onEnded { _ in
        guard dragStart != nil else { return }
        dragStart = nil
        let target: CGFloat
        if offset > buttonWidth / 2 { target = CGFloat(leading.count) * buttonWidth }
        else if offset < -buttonWidth / 2 { target = -CGFloat(trailing.count) * buttonWidth }
        else { target = 0 }
        withAnimation(.smooth(duration: 0.25)) {
          offset = target
          if target == 0 { swiped = nil }
        }
      }
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
