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
    OutlineList(store: store, rename: promptRename, delete: { deleting = $0 })
  }

  private func promptRename(_ node: JSON) {
    let collection = node.string("kind") == "collection"
    rename = RenameRequest(title: collection ? "Rename Collection" : "Rename Object",
                           name: node.string("name") ?? "") { name in
      store.act("rename", [node], value: name)
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

/* -------------------------------------------------------------------- */
/** \name Outline
 *
 * The tree is UIKit's sidebar list, as the Outliner has always been: tap to select, a two-finger
 * swipe across rows selects several as in Mail, swipe and touch and hold for actions, and drag
 * rows into a collection or onto an object.
 * \{ */

struct OutlineList: UIViewRepresentable {
  @ObservedObject var store: HierarchyStore
  let rename: (JSON) -> Void
  let delete: ([JSON]) -> Void

  func makeCoordinator() -> Coordinator { Coordinator(store: store) }

  func makeUIView(context: Context) -> UICollectionView {
    context.coordinator.makeView()
  }

  func updateUIView(_ view: UICollectionView, context: Context) {
    let coordinator = context.coordinator
    coordinator.store = store
    coordinator.rename = rename
    coordinator.delete = delete
    coordinator.reload()
  }

  @MainActor
  final class Coordinator: NSObject, UICollectionViewDelegate, UICollectionViewDragDelegate,
                           UICollectionViewDropDelegate
  {
    var store: HierarchyStore
    var rename: (JSON) -> Void = { _ in }
    var delete: ([JSON]) -> Void = { _ in }
    private weak var view: UICollectionView?
    private var dataSource: UICollectionViewDiffableDataSource<String, String>?
    private var byKey: [String: JSON] = [:]
    private var multipleSelection = false

    init(store: HierarchyStore) {
      self.store = store
    }

    func makeView() -> UICollectionView {
      var list = UICollectionLayoutListConfiguration(appearance: .sidebar)
      list.backgroundColor = .clear
      list.trailingSwipeActionsConfigurationProvider = { [weak self] path in
        self?.swipeActions(path, leading: false)
      }
      list.leadingSwipeActionsConfigurationProvider = { [weak self] path in
        self?.swipeActions(path, leading: true)
      }
      let view = UICollectionView(frame: .zero, collectionViewLayout: UICollectionViewCompositionalLayout.list(using: list))
      view.backgroundColor = .clear
      view.delegate = self
      view.allowsMultipleSelection = true
      view.allowsMultipleSelectionDuringEditing = true
      /* Touch and hold picks rows up; more can be tapped into the drag, as in Mail and Files. */
      view.dragDelegate = self
      view.dropDelegate = self
      view.dragInteractionEnabled = true
#if !os(visionOS)
      view.keyboardDismissMode = .onDrag
#endif
      view.accessibilityIdentifier = "blender.hierarchy.outline"
      let registration = UICollectionView.CellRegistration<UICollectionViewListCell, String> {
        [weak self] cell, _, key in
        self?.configure(cell, key: key)
      }
      dataSource = UICollectionViewDiffableDataSource(collectionView: view) { view, path, key in
        view.dequeueConfiguredReusableCell(using: registration, for: path, item: key)
      }
      self.view = view
      return view
    }

    /* -------------------------------------------------------------------- */
    /* Rows */

    func reload() {
      guard let view, let dataSource else { return }
      if view.isEditing != store.editing { view.isEditing = store.editing }
      let previous = byKey
      var keyed: [String: JSON] = [:]
      for node in store.nodes { if let key = node.string("key") { keyed[key] = node } }
      byKey = keyed

      let query = store.query
      var matches = Set<String>()
      if !query.isEmpty {
        for node in store.nodes where (node.string("name") ?? "").localizedStandardContains(query) {
          var key = node.string("key")
          while let current = key, !current.isEmpty, matches.insert(current).inserted {
            key = keyed[current]?.string("parent")
          }
        }
      }
      var outline = NSDiffableDataSourceSectionSnapshot<String>()
      var included = Set<String>()
      var expanded: [String] = []
      for node in store.nodes {
        guard let key = node.string("key") else { continue }
        if !query.isEmpty && !matches.contains(key) { continue }
        let parent = node.string("parent").flatMap { included.contains($0) ? $0 : nil }
        outline.append([key], to: parent)
        included.insert(key)
        if !query.isEmpty || !store.collapsed.contains(key) { expanded.append(key) }
      }
      outline.expand(expanded)

      let current = dataSource.snapshot(for: "scene")
      var topologyChanged = current.items != outline.items ||
        Set(current.items.filter { current.isExpanded($0) }) != Set(outline.items.filter { outline.isExpanded($0) })
      if !topologyChanged {
        for key in outline.items where current.parent(of: key) != outline.parent(of: key) {
          topologyChanged = true
          break
        }
      }
      if topologyChanged || dataSource.snapshot().numberOfSections == 0 {
        dataSource.apply(outline, to: "scene",
                         animatingDifferences: !previous.isEmpty && !UIAccessibility.isReduceMotionEnabled)
        { [weak self] in self?.synchronizeSelection() }
      }
      /* Value changes must not rebuild the outline or interrupt disclosure animations. */
      for path in view.indexPathsForVisibleItems {
        guard let key = dataSource.itemIdentifier(for: path), let node = keyed[key],
              topologyChanged || !jsonEqual(previous[key], node),
              let cell = view.cellForItem(at: path) as? UICollectionViewListCell else { continue }
        configure(cell, key: key)
      }
      synchronizeSelection()
    }

    private func node(at path: IndexPath) -> JSON? {
      dataSource?.itemIdentifier(for: path).flatMap { byKey[$0] }
    }

    private func configure(_ cell: UICollectionViewListCell, key: String) {
      guard let node = byKey[key] else { return }
      let collection = node.string("kind") == "collection"
      var content = cell.defaultContentConfiguration()
      content.text = node.string("name")
      content.textProperties.numberOfLines = 2
      if node.bool("active") {
        content.textProperties.font = UIFont(descriptor: content.textProperties.font.fontDescriptor
          .withSymbolicTraits(.traitBold) ?? content.textProperties.font.fontDescriptor, size: 0)
      }
      if !node.bool("enabled") { content.textProperties.color = .secondaryLabel }
      /* As in Blender's Outliner, the hierarchy shows category icons in their theme colors. */
      content.image = BlenderIcon.image(node.string("icon"), themed: true) ??
        UIImage(systemName: collection ? "folder" : "cube")
      content.imageProperties.maximumSize = CGSize(width: 22, height: 22)
      cell.contentConfiguration = content
      cell.configurationUpdateHandler = { cell, state in
        guard let cell = cell as? UICollectionViewListCell else { return }
        var configuration = content.updated(for: state)
        var background = UIBackgroundConfiguration.listSidebarCell().updated(for: state)
        if state.isSelected {
          background.backgroundColor = .systemBlue
          configuration.textProperties.color = .white
          configuration.imageProperties.tintColor = .white
        }
        cell.contentConfiguration = configuration
        cell.backgroundConfiguration = background
      }
      cell.setNeedsUpdateConfiguration()
      cell.accessibilityIdentifier = "blender.hierarchy." + (node.string("id") ?? "")
      cell.accessibilityValue = node.bool("active") ? "Active" : ""

      /* The system's outline disclosure, as in Files and Mail. The sidebar list ignores
       * indentation levels, so an empty leading slot carries the depth indent. */
      cell.indentationLevel = 0
      var accessories: [UICellAccessory] = []
      let indent = CGFloat(16 * max(0, node.int("depth") ?? 0))
      if indent > 0 {
        accessories.append(.customView(configuration: .init(customView: UIView(), placement: .leading(),
                                                            reservedLayoutWidth: .custom(indent),
                                                            maintainsFixedSize: true)))
      }
      if store.hasChildren(node) {
        accessories.append(.outlineDisclosure(options: .init(style: .cell)) { [weak self] in
          self?.store.toggle(node)
        })
      }
      if !collection { accessories.append(.multiselect()) }
      if node.bool("hidden") {
        let show = UIButton(type: .system)
        show.tintColor = .secondaryLabel
        show.frame = CGRect(x: 0, y: 0, width: 44, height: 44)
        show.setImage(UIImage(systemName: "eye.slash"), for: .normal)
        show.accessibilityLabel = "Show in viewport"
        show.addAction(UIAction { [weak self] _ in
          self?.store.act("visibility", [node], value: false)
        }, for: .touchUpInside)
        accessories.append(.customView(configuration: .init(customView: show, placement: .trailing())))
      }
      cell.accessories = accessories
    }

    private func synchronizeSelection() {
      guard let view, let dataSource else { return }
      for path in view.indexPathsForSelectedItems ?? [] where !(node(at: path)?.bool("selected") ?? false) {
        view.deselectItem(at: path, animated: false)
      }
      for key in dataSource.snapshot(for: "scene").visibleItems where byKey[key]?.bool("selected") ?? false {
        if let path = dataSource.indexPath(for: key) {
          view.selectItem(at: path, animated: false, scrollPosition: [])
        }
      }
    }

    /* -------------------------------------------------------------------- */
    /* Selection, as in Mail: tap selects one; Select mode or a two-finger swipe selects several. */

    private var extending: Bool { multipleSelection || (view?.isEditing ?? false) }

    func collectionView(_ collectionView: UICollectionView, shouldSelectItemAt indexPath: IndexPath) -> Bool {
      guard let node = node(at: indexPath) else { return false }
      return node.string("kind") == "object" && node.bool("enabled")
    }

    func collectionView(_ collectionView: UICollectionView, didSelectItemAt indexPath: IndexPath) {
      guard self.collectionView(collectionView, shouldSelectItemAt: indexPath), let node = node(at: indexPath),
            let id = node.string("id") else { return }
      let extend = extending
      if !extend {
        for other in collectionView.indexPathsForSelectedItems ?? [] where other != indexPath {
          collectionView.deselectItem(at: other, animated: true)
        }
      }
      store.send(["action": "select", "id": id, "extend": extend, "value": true])
    }

    func collectionView(_ collectionView: UICollectionView, shouldDeselectItemAt indexPath: IndexPath) -> Bool {
      /* An ordinary tap on an already-selected object still makes it the active object. */
      if !extending { self.collectionView(collectionView, didSelectItemAt: indexPath) }
      return extending
    }

    func collectionView(_ collectionView: UICollectionView, didDeselectItemAt indexPath: IndexPath) {
      guard extending, let node = node(at: indexPath), node.string("kind") == "object",
            let id = node.string("id") else { return }
      store.send(["action": "select", "id": id, "extend": true, "value": false])
    }

    func collectionView(_ collectionView: UICollectionView,
                        shouldBeginMultipleSelectionInteractionAt indexPath: IndexPath) -> Bool
    {
      self.collectionView(collectionView, shouldSelectItemAt: indexPath)
    }

    func collectionView(_ collectionView: UICollectionView,
                        didBeginMultipleSelectionInteractionAt indexPath: IndexPath)
    {
      multipleSelection = true
      /* A two-finger swipe across rows enters Select mode, as in Mail. */
      collectionView.isEditing = true
      store.editing = true
    }

    func collectionViewDidEndMultipleSelectionInteraction(_ collectionView: UICollectionView) {
      multipleSelection = false
    }

    /* -------------------------------------------------------------------- */
    /* Actions */

    private func swipeActions(_ path: IndexPath, leading: Bool) -> UISwipeActionsConfiguration? {
      guard let node = node(at: path) else { return nil }
      var items: [UIContextualAction] = []
      for identifier in leading ? ["duplicate", "rename"] : ["delete", "visibility"] {
        guard store.allow([node], identifier) else { continue }
        let deleting = identifier == "delete", duplicate = identifier == "duplicate"
        let visibility = identifier == "visibility", hidden = node.bool("hidden")
        let title = deleting ? "Delete" : duplicate ? "Duplicate" : visibility ? (hidden ? "Show" : "Hide") : "Rename"
        let item = UIContextualAction(style: deleting ? .destructive : .normal, title: title) {
          [weak self] _, _, complete in
          guard let self else { return complete(false) }
          if deleting {
            self.delete([node])
            complete(true)
          }
          else if identifier == "rename" {
            self.rename(node)
            complete(true)
          }
          else {
            let current = self.byKey[node.string("key") ?? ""] ?? node
            self.store.act(identifier, [current], value: visibility ? !current.bool("hidden") : nil)
            complete(true)
          }
        }
        let icon = deleting ? "TRASH" : duplicate ? "DUPLICATE" : visibility ? (hidden ? "HIDE_OFF" : "HIDE_ON") : "GREASEPENCIL"
        let symbol = deleting ? "trash" : duplicate ? "plus.square.on.square" :
          visibility ? (hidden ? "eye" : "eye.slash") : "pencil"
        item.image = BlenderIcon.image(icon) ?? UIImage(systemName: symbol)
        if !deleting { item.backgroundColor = duplicate ? .systemBlue : .systemGray }
        items.append(item)
      }
      guard !items.isEmpty else { return nil }
      let actions = UISwipeActionsConfiguration(actions: items)
      actions.performsFirstActionWithFullSwipe = false
      return actions
    }

    private func moveMenu(_ nodes: [JSON]) -> UIMenu {
      let items = store.nodes.filter { $0.string("kind") == "collection" && store.canMove(nodes, to: $0) }
        .map { target in
          UIAction(title: String(repeating: "  ", count: target.int("depth") ?? 0) + (target.string("name") ?? ""),
                   image: BlenderIcon.image("OUTLINER_COLLECTION")) { [weak self] _ in
            self?.store.move(nodes, to: target)
          }
        }
      return UIMenu(title: "Move to Collection", image: UIImage(systemName: "folder"), children: items)
    }

    private func menuElements(_ nodes: [JSON]) -> [UIMenuElement] {
      let several = nodes.count > 1
      var edit: [UIMenuElement] = []
      if !several && store.allow(nodes, "rename") {
        edit.append(UIAction(title: "Rename", image: UIImage(systemName: "pencil")) { [weak self] _ in
          self?.rename(nodes[0])
        })
      }
      if store.allow(nodes, "duplicate") {
        edit.append(UIAction(title: several ? "Duplicate \(nodes.count) Objects" : "Duplicate",
                             image: UIImage(systemName: "plus.square.on.square")) { [weak self] _ in
          self?.store.act("duplicate", nodes)
        })
      }
      if store.allow(nodes, "visibility") {
        let hidden = nodes.allSatisfy { $0.bool("hidden") }
        edit.append(UIAction(title: hidden ? "Show" : "Hide",
                             image: UIImage(systemName: hidden ? "eye" : "eye.slash")) { [weak self] _ in
          self?.store.act("visibility", nodes, value: !hidden)
        })
      }
      var elements: [UIMenuElement] = []
      if !edit.isEmpty { elements.append(UIMenu(options: .displayInline, children: edit)) }
      let move = moveMenu(nodes)
      if !move.children.isEmpty { elements.append(move) }
      if store.allow(nodes, "delete") {
        let remove = UIAction(title: several ? "Delete \(nodes.count) Objects" : "Delete",
                              image: UIImage(systemName: "trash"), attributes: .destructive) { [weak self] _ in
          self?.delete(nodes)
        }
        elements.append(UIMenu(options: .displayInline, children: [remove]))
      }
      return elements
    }

    func collectionView(_ collectionView: UICollectionView,
                        contextMenuConfigurationForItemsAt indexPaths: [IndexPath],
                        point: CGPoint) -> UIContextMenuConfiguration?
    {
      guard indexPaths.count == 1, let node = node(at: indexPaths[0]) else { return nil }
      let nodes = store.targets(node)
      let elements = menuElements(nodes)
      guard !elements.isEmpty else { return nil }
      return UIContextMenuConfiguration(identifier: nil, previewProvider: nil) { _ in
        UIMenu(title: nodes.count > 1 ? "\(nodes.count) Objects" : "", children: elements)
      }
    }

    /* -------------------------------------------------------------------- */
    /* Dragging rows into a collection, or onto an object to parent them. */

    private func dragItem(_ node: JSON) -> UIDragItem {
      let item = UIDragItem(itemProvider: NSItemProvider(object: (node.string("name") ?? "") as NSString))
      item.localObject = node as NSDictionary
      return item
    }

    func collectionView(_ collectionView: UICollectionView, itemsForBeginning session: UIDragSession,
                        at indexPath: IndexPath) -> [UIDragItem]
    {
      guard let node = node(at: indexPath) else { return [] }
      let nodes = store.targets(node)
      guard store.allow(nodes, "move") else { return [] }
      return nodes.map(dragItem)
    }

    func collectionView(_ collectionView: UICollectionView, itemsForAddingTo session: UIDragSession,
                        at indexPath: IndexPath, point: CGPoint) -> [UIDragItem]
    {
      guard let node = node(at: indexPath), store.allow([node], "move"),
            let first = session.items.first?.localObject as? JSON,
            first.string("kind") == node.string("kind"),
            !session.items.contains(where: { jsonEqual(($0.localObject as? JSON)?["id"], node["id"]) })
      else { return [] }
      return [dragItem(node)]
    }

    private func nodes(in session: UIDropSession) -> [JSON] {
      let nodes = session.items.compactMap { $0.localObject as? JSON }
      return nodes.count == session.items.count ? nodes : []
    }

    func collectionView(_ collectionView: UICollectionView, canHandle session: UIDropSession) -> Bool {
      session.localDragSession != nil && !nodes(in: session).isEmpty
    }

    private func target(_ collectionView: UICollectionView, _ session: UIDropSession) -> JSON? {
      collectionView.indexPathForItem(at: session.location(in: collectionView)).flatMap(node(at:))
    }

    func collectionView(_ collectionView: UICollectionView, dropSessionDidUpdate session: UIDropSession,
                        withDestinationIndexPath destinationIndexPath: IndexPath?) -> UICollectionViewDropProposal
    {
      guard store.canMove(nodes(in: session), to: target(collectionView, session)) else {
        return UICollectionViewDropProposal(operation: .forbidden)
      }
      return UICollectionViewDropProposal(operation: .move, intent: .insertIntoDestinationIndexPath)
    }

    func collectionView(_ collectionView: UICollectionView, performDropWith coordinator: UICollectionViewDropCoordinator) {
      guard let target = target(collectionView, coordinator.session) else { return }
      store.move(nodes(in: coordinator.session), to: target)
    }
  }
}

/** \} */
