/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Controls Blender draws from several buttons: data-block pickers, lists such as shape keys and
 * vertex groups, the material preview and grouped operator menus.
 */

import SwiftUI
import UIKit

struct CompositeRow: View, Equatable {
  let store: InspectorStore
  let property: JSON

  /** A snapshot that leaves the property as it was leaves the row alone. */
  static func == (a: CompositeRow, b: CompositeRow) -> Bool {
    a.store === b.store && jsonDictEqual(a.property, b.property)
  }

  var body: some View {
    switch property.string("type") {
      case "ID": DataPicker(store: store, property: property)
      case "LIST": ListOutliner(store: store, property: property)
      case "PREVIEW": PreviewRow(store: store, property: property)
      case "OPERATOR_MENU": OperatorMenuRow(store: store, property: property)
      default: EmptyView()
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Rename
 * \{ */

/** A name prompt shared by data-blocks and list items. */
struct RenameRequest: Identifiable {
  let id = UUID()
  let title: String
  let name: String
  let commit: (String) -> Void
}

struct RenamePrompt: ViewModifier {
  @Binding var request: RenameRequest?
  @State private var text = ""

  func body(content: Content) -> some View {
    content.alert(request?.title ?? "Rename", isPresented: Binding(
      get: { request != nil }, set: { if !$0 { request = nil } }))
    {
      TextField("Name", text: $text)
        .autocorrectionDisabled()
        .textInputAutocapitalization(.never)
      Button("Cancel", role: .cancel) { request = nil }
      Button("Rename") {
        let name = text.trimmingCharacters(in: .whitespaces)
        if let request, !name.isEmpty, name != request.name { request.commit(name) }
        request = nil
      }
      .keyboardShortcut(.defaultAction)
    }
    .onChange(of: request?.id) { _, _ in text = request?.name ?? "" }
  }
}

extension View {
  func renamePrompt(_ request: Binding<RenameRequest?>) -> some View {
    modifier(RenamePrompt(request: request))
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Data-block picker
 * \{ */

func usersText(_ item: JSON) -> String {
  var parts: [String] = []
  let users = item.int("users") ?? -1
  if users > 1 { parts.append("\(users) users") }
  else if item.has("users") && users == 0 { parts.append("No users") }
  if item.bool("linked") { parts.append("Linked") }
  return parts.joined(separator: " · ")
}

/** The data-block in use as a card: preview, name and users. Tapping browses; the menu holds
 * New, Duplicate, Rename, Fake User, Make Single User and Unlink. */
struct DataPicker: View {
  let store: InspectorStore
  let property: JSON
  @State private var browsing = false
  @State private var rename: RenameRequest?

  private var has: Bool { !(property.string("value") ?? "").isEmpty }
  private var type: String { property.string("id_type") ?? "" }
  private var editable: Bool { property.bool("editable") }

  var body: some View {
    HStack(spacing: 6) {
      Button {
        browsing = true
      } label: {
        HStack(spacing: 12) {
          PreviewThumbnail(path: property.string("preview"),
                           fallback: BlenderIcon.image(property.string("icon")), size: 52)
          VStack(alignment: .leading, spacing: 2) {
            Text(has ? property.string("name") ?? "" : "No \(type)")
              .font(.headline)
              .lineLimit(1)
              .truncationMode(.middle)
              .foregroundStyle(has ? .primary : .secondary)
            Text(has ? (usersText(property).isEmpty ? type : usersText(property)) : "Choose or create one")
              .font(.subheadline)
              .foregroundStyle(.secondary)
          }
          Spacer(minLength: 0)
          Image(systemName: "chevron.up.chevron.down")
            .font(.caption)
            .foregroundStyle(.tertiary)
        }
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .disabled(!editable || property.array("options").isEmpty)
      .accessibilityLabel("\(type), \(has ? property.string("name") ?? "" : "None")")
      .accessibilityHint("Shows the data-blocks to choose from.")
      .popover(isPresented: $browsing) {
        DataBrowser(store: store, property: property) { browsing = false }
          .frame(minWidth: 380, minHeight: min(560, 150 + 60 * CGFloat(property.array("options").count)))
      }

      if !has && property.bool("can_new") {
        Button {
          store.request(property, extra: ["id_action": "new"])
        } label: {
          Label("New", systemImage: "plus")
        }
        .buttonStyle(.bordered)
        .buttonBorderShape(.capsule)
        .disabled(!editable)
        .accessibilityLabel("New \(type)")
      }

      actionsMenu
    }
    .frame(minHeight: 64)
    .renamePrompt($rename)
  }

  @ViewBuilder
  private var actionsMenu: some View {
    let canRename = has && !property.bool("linked")
    let any = property.bool("can_new") || canRename || property.has("fake_user") ||
      property.bool("can_single_user") || property.bool("can_unlink")
    if any {
      Menu {
        Section {
          if property.bool("can_new") {
            Button {
              store.request(property, extra: ["id_action": "new"])
            } label: {
              Label(has ? "Duplicate" : "New \(type)", systemImage: has ? "plus.square.on.square" : "plus")
            }
          }
          if canRename {
            Button {
              rename = RenameRequest(title: "Rename \(type)", name: property.string("name") ?? "") { name in
                store.request(property, extra: ["id_action": "rename", "value": name])
              }
            } label: {
              Label("Rename…", systemImage: "pencil")
            }
          }
        }
        Section {
          if property.has("fake_user") {
            Toggle(isOn: Binding(get: { property.bool("fake_user") },
                                 set: { _ in store.request(property, extra: ["id_action": "fake_user"]) }))
            {
              Label("Fake User", systemImage: "shield")
              Text("Keep even when nothing uses it")
            }
          }
          if property.bool("can_single_user") {
            Button {
              store.request(property, extra: ["id_action": "single_user"])
            } label: {
              Label("Make Single User", systemImage: "person")
              Text("Used by \((property.int("users") ?? 1) - 1) others")
            }
          }
        }
        if property.bool("can_unlink") {
          Section {
            Button {
              store.request(property, extra: ["id_action": "unlink"])
            } label: {
              Label("Unlink", systemImage: "xmark.circle")
            }
          }
        }
      } label: {
        Image(systemName: "ellipsis.circle")
          .font(.title3)
          .frame(width: 40, height: 44)
      }
      .tint(.primary)
      .disabled(!editable)
      .accessibilityLabel("\(type) Actions")
    }
  }
}

/** Choose a data-block, like Blender's ID search popup: searchable, with previews. */
struct DataBrowser: View {
  let store: InspectorStore
  let property: JSON
  let done: () -> Void
  @State private var query = ""

  var body: some View {
    let type = property.string("id_type") ?? ""
    let options = property.array("options").filter {
      query.isEmpty || ($0.string("label") ?? "").localizedStandardContains(query)
    }
    NavigationStack {
      ScrollViewReader { reader in
        List(Array(options.enumerated()), id: \.offset) { index, option in
          let current = jsonEqual(option["value"], property["value"])
          Button {
            store.request(property, extra: ["id_action": "select", "value": option["value"] ?? ""])
            done()
          } label: {
            HStack(spacing: 12) {
              PreviewThumbnail(path: option.string("preview"),
                               fallback: BlenderIcon.image(property.string("icon")), size: 44)
              VStack(alignment: .leading) {
                Text(option.string("label") ?? "").foregroundStyle(.primary)
                if !usersText(option).isEmpty {
                  Text(usersText(option)).font(.caption).foregroundStyle(.secondary)
                }
              }
              Spacer()
              if current { Image(systemName: "checkmark").foregroundStyle(.tint) }
            }
          }
          .id(index)
          .accessibilityAddTraits(current ? .isSelected : [])
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .onAppear {
          if let index = options.firstIndex(where: { jsonEqual($0["value"], property["value"]) }) {
            reader.scrollTo(index, anchor: .center)
          }
        }
      }
      .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .always))
      .navigationTitle(type)
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        if property.bool("can_new") {
          ToolbarItem(placement: .primaryAction) {
            Button {
              store.request(property, extra: ["id_action": "new"])
              done()
            } label: {
              Image(systemName: "plus")
            }
            .accessibilityLabel("New \(type)")
          }
        }
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lists
 *
 * Shape keys, vertex groups, UV maps, material slots and Blender's other lists, as small
 * outliners: tap to make an item active, tap the active item's name to rename it, drag the
 * handle to reorder, touch and hold for its actions. What Blender draws beside an item's name,
 * such as a shape key's value or a vertex group's lock, stays on its row.
 * \{ */

/** Lists longer than this show the rows around the active one and a page with all of them. */
private let inlineListRows = 40

struct ListOutliner: View {
  let store: InspectorStore
  let property: JSON
  @State private var showingAll = false

  private var rows: [JSON] { property.array("rows") }
  private var active: Int? { (store.value(property) as? NSNumber)?.intValue }

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      if rows.isEmpty {
        Text("None")
          .font(.subheadline)
          .foregroundStyle(.tertiary)
          .frame(maxWidth: .infinity, minHeight: 40, alignment: .leading)
          .padding(.leading, 10)
      }
      else {
        let shown = shownRows
        ListRows(store: store, property: property, rows: shown.rows, offset: shown.offset)
      }
      HStack {
        if rows.count > inlineListRows {
          Button {
            showingAll = true
          } label: {
            Label("Show All \(rows.count)", systemImage: "list.bullet")
              .font(.subheadline)
              .frame(minHeight: 32)
          }
          .buttonStyle(.borderless)
          .popover(isPresented: $showingAll) {
            ListPage(store: store, property: property)
              .frame(minWidth: 360, minHeight: 520)
          }
        }
        Spacer(minLength: 0)
        ListToolbar(store: store, property: property)
      }
      .padding(.bottom, 4)
    }
  }

  /** The rows around the active one, so the active item is always visible. */
  private var shownRows: (rows: [JSON], offset: Int) {
    guard rows.count > inlineListRows else { return (rows, 0) }
    let position = rows.firstIndex { $0.int("index") == active } ?? 0
    let start = max(0, min(rows.count - inlineListRows, position - inlineListRows / 2))
    return (Array(rows[start..<start + inlineListRows]), start)
  }
}

/** Add, remove and the list's other actions, together in one compact capsule. */
struct ListToolbar: View {
  let store: InspectorStore
  let property: JSON

  var body: some View {
    let editable = property.bool("editable")
    let extras = property.array("extras")
    let hasMenu = !extras.isEmpty || property.has("menu")
    if property.has("add") || property.has("remove") || hasMenu {
      HStack(spacing: 0) {
        HStack(spacing: 0) {
          if let add = property.dict("add") {
            toolButton("plus", add)
          }
          if let remove = property.dict("remove") {
            toolButton("minus", remove)
          }
          if hasMenu {
            Menu {
              ForEach(Array(extras.enumerated()), id: \.offset) { _, op in
                Button {
                  runOperator(op)
                } label: {
                  Label {
                    Text(op.string("label") ?? "")
                  } icon: {
                    if let image = BlenderIcon.image(op.string("icon")) { Image(uiImage: image) }
                  }
                }
              }
              if let menu = property.string("menu") {
                Section { BlenderMenuItems(route: BlenderMenus.route(menu, tab: property.string("tab"))) }
              }
            } label: {
              Image(systemName: "ellipsis")
                .frame(width: 40, height: 32)
            }
            .prefetchingBlenderMenu(BlenderMenus.route(property.string("menu") ?? "", tab: property.string("tab")))
            .tint(.primary)
            .disabled(!editable)
            .accessibilityLabel("More")
          }
        }
        .font(.subheadline)
        .foregroundStyle(.primary)
        .background(PanelStyle.field, in: Capsule())
      }
    }
  }

  private func toolButton(_ symbol: String, _ op: JSON) -> some View {
    Button {
      runOperator(op)
      Haptics.selection()
    } label: {
      Image(systemName: symbol)
        .frame(width: 40, height: 32)
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .disabled(!property.bool("editable") || (op.has("editable") && !op.bool("editable")))
    .accessibilityLabel(op.string("label") ?? symbol)
    .help(op.string("label") ?? "")
  }

  private func runOperator(_ op: JSON) {
    store.request(["key": op.string("key") ?? "", "panel": property.string("panel") ?? "",
                   "tab": property.string("tab") ?? ""], extra: ["type": "OPERATOR"])
  }
}

struct ListRows: View {
  let store: InspectorStore
  let property: JSON
  let rows: [JSON]
  var offset = 0
  @State private var dragging: Int?
  @State private var dragOffset: CGFloat = 0
  @State private var rowHeight: CGFloat = 40
  @State private var renaming: Int?
  @State private var renameText = ""
  @FocusState private var renameFocused: Bool

  private var active: Int? { (store.value(property) as? NSNumber)?.intValue }
  private var editable: Bool { property.bool("editable") }
  private var reorder: Bool { property.bool("reorder") && editable && property.array("rows").count > 1 }

  var body: some View {
    VStack(spacing: 2) {
      ForEach(Array(rows.enumerated()), id: \.element.listIdentity) { position, item in
        row(item, position: position)
          .offset(y: offsetFor(position))
          .zIndex(dragging == position ? 1 : 0)
          .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { rowHeight = $0 + 2 }
      }
    }
    .motionAware(.smooth(duration: 0.2), value: targetPosition)
  }

  private var targetPosition: Int? {
    guard let dragging else { return nil }
    return max(0, min(rows.count - 1, dragging + Int((dragOffset / rowHeight).rounded())))
  }

  private func offsetFor(_ position: Int) -> CGFloat {
    guard let dragging, let target = targetPosition else { return 0 }
    if position == dragging { return dragOffset }
    if position > dragging && position <= target { return -rowHeight }
    if position < dragging && position >= target { return rowHeight }
    return 0
  }

  @ViewBuilder
  private func row(_ item: JSON, position: Int) -> some View {
    let index = item.int("index") ?? position
    let isActive = index == active
    let label = (item.string("label") ?? "").isEmpty ? "Unnamed" : item.string("label")!
    HStack(spacing: 10) {
      Group {
        if let preview = PreviewCache.image(item.string("preview")) {
          Image(uiImage: preview).resizable().scaledToFill()
            .frame(width: 28, height: 28)
            .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
        }
        else if let icon = BlenderIcon.image(item.string("icon")) {
          Image(uiImage: icon).resizable().scaledToFit().frame(width: 22, height: 22)
            .foregroundStyle(.primary)
        }
      }
      if renaming == index {
        TextField("Name", text: $renameText)
          .focused($renameFocused)
          .autocorrectionDisabled()
          .textInputAutocapitalization(.never)
          .submitLabel(.done)
          .onSubmit { commitRename(item) }
          .onChange(of: renameFocused) { _, focused in if !focused { commitRename(item) } }
      }
      else {
        Text(label)
          .lineLimit(1)
          .truncationMode(.middle)
          .frame(maxWidth: .infinity, alignment: .leading)
          .layoutPriority(1)
          .contentShape(Rectangle())
          .hoverEffect(.highlight)
          .onTapGesture { tap(item, index: index, isActive: isActive) }
      }
      ForEach(Array(item.array("controls").enumerated()), id: \.offset) { _, control in
        RowControl(store: store, control: listControl(control))
      }
      if reorder {
        Image(systemName: "line.3.horizontal")
          .foregroundStyle(.tertiary)
          .frame(width: 36, height: 36)
          .contentShape(Rectangle())
          .gesture(DragGesture(minimumDistance: 2)
            .onChanged { value in
              if dragging == nil {
                dragging = position
                Haptics.impact()
              }
              let before = targetPosition
              dragOffset = value.translation.height
              if targetPosition != before { Haptics.selection() }
            }
            .onEnded { _ in finishDrag() })
          .accessibilityLabel("Reorder \(label)")
          .accessibilityActions {
            if position + offset > 0 {
              Button("Move Up") { move(from: position + offset, to: position + offset - 1) }
            }
            if position + offset < property.array("rows").count - 1 {
              Button("Move Down") { move(from: position + offset, to: position + offset + 1) }
            }
          }
      }
    }
    .padding(.leading, 10)
    .padding(.trailing, reorder ? 0 : 10)
    .frame(minHeight: 40)
    .background {
      RoundedRectangle(cornerRadius: 8, style: .continuous)
        .fill(isActive ? PanelStyle.activeRow : .clear)
    }
    .scaleEffect(dragging == position ? 1.02 : 1)
    .shadow(color: .black.opacity(dragging == position ? 0.25 : 0), radius: 8, y: 3)
    .contentShape(.contextMenuPreview, RoundedRectangle(cornerRadius: 8))
    .contextMenu { rowMenu(item, index: index, position: position + offset, label: label) }
    .accessibilityElement(children: .contain)
    .accessibilityLabel(label)
    .accessibilityAddTraits(isActive ? [.isSelected, .isButton] : .isButton)
    .accessibilityAction { tap(item, index: index, isActive: false) }
  }

  @ViewBuilder
  private func rowMenu(_ item: JSON, index: Int, position: Int, label: String) -> some View {
    if item.has("rename_key") {
      Button {
        startRename(item, index: index)
      } label: {
        Label("Rename", systemImage: "pencil")
      }
    }
    if reorder {
      Section {
        if position > 0 {
          Button { move(from: position, to: 0) } label: { Label("Move to Top", systemImage: "arrow.up.to.line") }
          Button { move(from: position, to: position - 1) } label: { Label("Move Up", systemImage: "arrow.up") }
        }
        let count = property.array("rows").count
        if position < count - 1 {
          Button { move(from: position, to: position + 1) } label: { Label("Move Down", systemImage: "arrow.down") }
          Button { move(from: position, to: count - 1) } label: { Label("Move to Bottom", systemImage: "arrow.down.to.line") }
        }
      }
    }
    if let remove = property.dict("remove"), editable {
      Section {
        Button(role: .destructive) {
          if index != active { store.edit(property, value: index) }
          store.request(["key": remove.string("key") ?? "", "panel": property.string("panel") ?? "",
                         "tab": property.string("tab") ?? ""], extra: ["type": "OPERATOR"])
        } label: {
          Label("Delete", systemImage: "trash")
        }
      }
    }
  }

  private func tap(_ item: JSON, index: Int, isActive: Bool) {
    guard editable else { return }
    if isActive {
      if item.has("rename_key") { startRename(item, index: index) }
      return
    }
    store.edit(property, value: index)
    Haptics.selection()
  }

  private func startRename(_ item: JSON, index: Int) {
    renameText = item.string("label") ?? ""
    renaming = index
    renameFocused = true
  }

  private func commitRename(_ item: JSON) {
    guard renaming != nil else { return }
    renaming = nil
    let name = renameText.trimmingCharacters(in: .whitespaces)
    guard !name.isEmpty, name != item.string("label"), let key = item.string("rename_key") else { return }
    store.request(["key": key, "panel": property.string("panel") ?? "", "tab": property.string("tab") ?? ""],
                  extra: ["type": "STRING", "value": name])
  }

  private func finishDrag() {
    guard let from = dragging, let to = targetPosition else { return }
    dragging = nil
    dragOffset = 0
    if from != to { move(from: from + offset, to: to + offset) }
  }

  private func move(from: Int, to: Int) {
    guard from != to else { return }
    store.request(property, extra: ["list_action": "move", "from": from, "to": to])
    Haptics.impact()
  }

  /** Row controls are edited in the list's panel and tab. */
  private func listControl(_ control: JSON) -> JSON {
    var result = control
    result["panel"] = property.string("panel") ?? ""
    result["tab"] = property.string("tab") ?? ""
    return result
  }
}

private extension Dictionary where Key == String, Value == Any {
  /** Rows are identified by index and name, so a rename or reorder animates in place. */
  var listIdentity: String { "\(self["index"] ?? "")" }
}

/** A control drawn on a list row: an icon toggle (mute, lock) or a compact value. */
struct RowControl: View {
  let store: InspectorStore
  let control: JSON

  var body: some View {
    switch control.string("type") {
      case "BOOLEAN":
        let on = (store.value(control) as? NSNumber)?.boolValue ?? false
        Button {
          store.edit(control, value: !on)
          Haptics.selection()
        } label: {
          rowToggleImage(shown: shownOn(on))
            .frame(width: 32, height: 32)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!control.bool("editable"))
        .accessibilityLabel(control.string("label") ?? control.string("property") ?? "")
        .accessibilityValue(shownOn(on) ? "On" : "Off")
      case "INT", "FLOAT":
        NumberField(store: store, property: control, compact: true, significant: 3)
          .frame(width: 56)
      case "ENUM":
        EnumControl(store: store, property: control, compact: true)
      default:
        EmptyView()
    }
  }

  /** Blender draws some toggles inverted (a shape key's checkbox is on while it is not muted):
   * the icon Blender reported says how the reported value looks, a pending edit flips it. */
  private func shownOn(_ on: Bool) -> Bool {
    let icon = control.string("icon") ?? ""
    let reported = (control["value"] as? NSNumber)?.boolValue ?? false
    let drawnOn: Bool
    switch icon {
      case "CHECKBOX_HLT", "LOCKED", "RESTRICT_RENDER_OFF", "RESTRICT_VIEW_OFF", "HIDE_OFF",
           "RESTRICT_SELECT_OFF":
        drawnOn = true
      case "CHECKBOX_DEHLT", "UNLOCKED", "RESTRICT_RENDER_ON", "RESTRICT_VIEW_ON", "HIDE_ON",
           "RESTRICT_SELECT_ON":
        drawnOn = false
      default:
        drawnOn = reported
    }
    return on == reported ? drawnOn : !drawnOn
  }

  /** Blender's checkbox becomes a checkmark; other icons show their on and off states. */
  @ViewBuilder
  private func rowToggleImage(shown: Bool) -> some View {
    let icon = control.string("icon") ?? ""
    if icon.isEmpty || icon == "NONE" || icon.hasPrefix("CHECKBOX") {
      Image(systemName: shown ? "checkmark.circle.fill" : "circle")
        .foregroundStyle(shown ? PanelStyle.tint : .secondary)
    }
    else if let image = BlenderIcon.image(shownIcon(icon, shown)) {
      Image(uiImage: image)
        .resizable()
        .scaledToFit()
        .frame(width: 16, height: 16)
        .foregroundStyle(shown ? .primary : .tertiary)
    }
  }

  /** The icon for the state shown, so a pending toggle shows its new icon at once. */
  private func shownIcon(_ icon: String, _ shown: Bool) -> String {
    let pairs = [("LOCKED", "UNLOCKED"), ("RESTRICT_RENDER_OFF", "RESTRICT_RENDER_ON"),
                 ("RESTRICT_VIEW_OFF", "RESTRICT_VIEW_ON"), ("HIDE_OFF", "HIDE_ON"),
                 ("RESTRICT_SELECT_OFF", "RESTRICT_SELECT_ON")]
    for (on, off) in pairs where icon == on || icon == off { return shown ? on : off }
    return icon
  }
}

/** Every item of a long list, searchable. */
struct ListPage: View {
  let store: InspectorStore
  let property: JSON
  @State private var query = ""

  var body: some View {
    let rows = property.array("rows").filter {
      query.isEmpty || ($0.string("label") ?? "").localizedStandardContains(query)
    }
    NavigationStack {
      ScrollView {
        VStack(alignment: .leading, spacing: 8) {
          ListToolbar(store: store, property: property)
          ListRows(store: store, property: property, rows: rows)
        }
        .padding(16)
      }
      .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .always))
      .navigationTitle("\(property.array("rows").count) Items")
      .navigationBarTitleDisplayMode(.inline)
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Preview and operator menus
 * \{ */

/** The Preview panel's render, floating in the section, with its shapes on a glass capsule. */
struct PreviewRow: View {
  let store: InspectorStore
  let property: JSON

  var body: some View {
    VStack(spacing: 0) {
      Group {
        if let image = PreviewCache.image(property.string("preview")) {
          Image(uiImage: image).resizable().scaledToFit()
        }
        else {
          ProgressView()
        }
      }
      .frame(width: 250, height: 250)
      .accessibilityLabel("Preview of \(property.string("label") ?? "")")
      if let shape = property.dict("shape") {
        HStack(spacing: 2) {
          ForEach(Array(shape.array("options").enumerated()), id: \.offset) { _, option in
            let selected = jsonEqual(option["value"], store.value(shape))
            Button {
              store.edit(shape, value: option["value"] ?? "")
              Haptics.selection()
            } label: {
              Group {
                if let image = BlenderIcon.image(option.string("icon")) {
                  Image(uiImage: image).resizable().scaledToFit().frame(width: 18, height: 18)
                }
                else {
                  Text(option.string("label") ?? "")
                }
              }
              .foregroundStyle(selected ? PanelStyle.tint : .secondary)
              .frame(width: 36, height: 34)
              .background(selected ? Color.accentColor.opacity(0.2) : .clear, in: Capsule())
            }
            .buttonStyle(.plain)
            .disabled(!shape.bool("editable"))
            .accessibilityLabel(option.string("label") ?? "")
            .help(option.string("label") ?? "")
          }
        }
        .padding(5)
        .panelGlass(in: Capsule())
      }
    }
    .frame(maxWidth: .infinity)
  }
}

/** An operator's choices as a menu with Blender's groups, such as Add Object Constraint. */
struct OperatorMenuRow: View {
  let store: InspectorStore
  let property: JSON

  var body: some View {
    Menu {
      ForEach(Array(groups.enumerated()), id: \.offset) { _, group in
        Section(group.heading) {
          ForEach(Array(group.options.enumerated()), id: \.offset) { _, option in
            Button {
              store.request(property, extra: ["value": option["value"] ?? ""])
            } label: {
              Label {
                Text(option.string("label") ?? "")
              } icon: {
                if let image = BlenderIcon.image(option.string("icon")) { Image(uiImage: image) }
              }
            }
          }
        }
      }
    } label: {
      Label {
        Text(property.string("label") ?? "")
      } icon: {
        IconView(name: property.string("icon"), symbol: "plus")
      }
      .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
    }
    .disabled(!property.bool("editable"))
  }

  private var groups: [(heading: String, options: [JSON])] {
    var result: [(heading: String, options: [JSON])] = []
    var heading = ""
    var items: [JSON] = []
    for option in property.array("options") {
      if let next = option.string("heading") {
        if !items.isEmpty { result.append((heading, items)) }
        heading = next
        items = []
        continue
      }
      items.append(option)
    }
    if !items.isEmpty { result.append((heading, items)) }
    return result
  }
}

/** \} */
