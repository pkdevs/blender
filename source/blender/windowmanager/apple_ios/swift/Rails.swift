/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The rails along the document window's leading edge: Blender's tools, and in paint and sculpt
 * modes the brushes above them, sharing one column.
 *
 * A rail shows only its active item until opened. A tap opens it, wide with titles, and picking
 * closes it again; holding an item opens it the same way to slide to an item and release. Dragged
 * up, a rail docks open at the middle of the window, where it stays until its handle closes it.
 * Tools in a group show the others in a menu while the list is open. A label beside the rail
 * names an item while a pointer or the Pencil hovers it, and briefly after choosing one.
 */

import SwiftUI
import UIKit

/* -------------------------------------------------------------------- */
/** \name Items
 * \{ */

/** A tool or brush on a rail. */
struct RailItem: Identifiable {
  let id: String
  let title: String
  let image: UIImage?
  /** Shown while chosen, with the list open; Blender has white artwork for some tools. */
  let selectedImage: UIImage?
  /** A gap before it, between two groups of tools or catalogs of brushes. */
  let gapBefore: Bool
  /** The other tools of its group, to choose from a menu. */
  let group: [RailItem]
  /** Where a group's menu separates its sections, by index into `group`. */
  let groupBreaks: Set<Int>

  var grouped: Bool { group.count > 1 }

  @MainActor
  static func tools(_ tools: [JSON?], mode: String) -> [RailItem] {
    var items: [RailItem] = []
    var gap = false
    for tool in tools {
      guard let tool else {
        gap = !items.isEmpty
        continue
      }
      guard let item = makeTool(tool, mode: mode, gapBefore: gap) else { continue }
      items.append(item)
      gap = false
    }
    return items
  }

  @MainActor
  private static func makeTool(_ tool: JSON, mode: String, gapBefore: Bool) -> RailItem? {
    guard let id = tool.string("id"), !id.isEmpty, let title = tool.string("title"), !title.isEmpty,
          tool.string("symbol") != nil
    else { return nil }
    var group: [RailItem] = []
    var breaks: Set<Int> = []
    for child in (tool["children"] as? [Any]) ?? [] {
      guard let child = child as? JSON else {
        breaks.insert(group.count)
        continue
      }
      if let item = makeTool(child, mode: mode, gapBefore: false) { group.append(item) }
    }
    return RailItem(id: id, title: title, image: toolImage(tool, mode: mode, selected: false),
                    selectedImage: toolImage(tool, mode: mode, selected: true), gapBefore: gapBefore,
                    group: group, groupBreaks: breaks)
  }

  /** Blender's colored tool artwork, or its monochrome icon, or a symbol. */
  @MainActor
  static func toolImage(_ tool: JSON, mode: String, selected: Bool) -> UIImage? {
    if let icon = tool.string("icon") {
      if let colored = UIImage(named: (selected ? "blender.tool.selected." : "blender.tool.color.") + icon) {
        return colored.withRenderingMode(.alwaysOriginal)
      }
      if let image = UIImage(named: "blender.tool." + icon) {
        return image.withRenderingMode(.alwaysTemplate)
      }
    }
    let id = tool.string("id") ?? ""
    let prefix = selected ? "selected.tool." : "tool."
    if let image = wm_apple_ios_icon("\(prefix)\(mode).\(id)", nil) { return image }
    let symbol = tool.string("symbol") ?? ""
    return wm_apple_ios_icon(prefix + id, symbol.isEmpty ? nil : symbol) ??
      UIImage(systemName: "questionmark.circle")
  }

  @MainActor
  static func brushes(_ brushes: JSON?) -> [RailItem] {
    var items: [RailItem] = []
    var catalog: String?
    for brush in brushes?.array("items") ?? [] {
      guard let id = brush.string("id"), !id.isEmpty, let title = brush.string("title") else { continue }
      /* Catalogs are separated the way the tool rail separates groups. */
      let brushCatalog = brush.string("catalog") ?? ""
      let image = wm_apple_ios_brush_image(brush.string("preview") ?? "", 36)
      items.append(RailItem(id: id, title: title, image: image, selectedImage: image,
                            gapBefore: catalog != nil && catalog != brushCatalog, group: [],
                            groupBreaks: []))
      catalog = brushCatalog
    }
    return items
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name State
 * \{ */

@MainActor
@Observable
final class RailState {
  /** Opened by a tap or a hold: wide, with titles, until an item is picked. */
  var expanded = false
  /** Docked open at the middle of the window. */
  var docked = false
  /** Holding an item to slide to another and release. */
  var pressSelecting = false
  /** The item under the finger while sliding. */
  var hovered: String?
  /** The rail following a drag, before it docks. */
  var dragOffset: CGFloat = 0
  /** A hold ended over an item: its tap, which ends with it, is not one. */
  @ObservationIgnored var suppressTap = false
  /** Chosen here, until Blender's snapshot shows it. */
  var pending: String?
  @ObservationIgnored var pendingUntil = Date.distantPast
  /** Each item's frame in the window, to tell which one is under a finger and to place the label
   * beside it. */
  private(set) var frames: [String: CGRect] = [:]

  func setFrame(_ frame: CGRect?, for id: String) {
    if frames[id] != frame { frames[id] = frame }
  }

  var listVisible: Bool { expanded || docked }

  func active(_ blender: String) -> String {
    if let pending, pending != blender, Date() < pendingUntil { return pending }
    return blender
  }

  func choose(_ id: String) {
    pending = id
    pendingUntil = Date().addingTimeInterval(1)
  }

  func item(at point: CGPoint) -> String? {
    frames.first { $0.value.insetBy(dx: -10, dy: -6).contains(point) }?.key
  }
}

/** The label beside the rails naming an item. */
@MainActor
@Observable
final class RailLabel {
  var text: String?
  /** The item it names, and its rail, which it stays beside as the rail opens and closes. */
  private(set) var item = ""
  private(set) weak var rail: RailState?
  /** Shown after choosing, rather than while hovering, and hiding by itself. */
  private(set) var transient = false
  @ObservationIgnored private var generation = 0

  var anchor: CGRect { rail?.frames[item] ?? .zero }

  func show(_ text: String, item: String, rail: RailState, transient: Bool) {
    generation += 1
    self.text = text
    self.item = item
    self.rail = rail
    self.transient = transient
    guard transient else { return }
    let current = generation
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.9) { [weak self] in
      guard let self, self.generation == current else { return }
      self.hide()
    }
  }

  func hide() {
    generation += 1
    text = nil
    transient = false
  }

  /** The pointer left an item: only a hover label goes. */
  func hoverEnded() {
    if !transient { hide() }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name A rail
 * \{ */

struct RailItemView: View {
  let item: RailItem
  let selected: Bool
  let emphasized: Bool
  let showsTitle: Bool
  /** Brushes' previews sit closer to the edges than tool icons. */
  let brush: Bool
  let namespace: Namespace.ID

  var body: some View {
    let image = selected ? item.selectedImage ?? item.image : item.image
    HStack(spacing: showsTitle ? 10 : 0) {
      if let image {
        Image(uiImage: image)
          .renderingMode(image.renderingMode == .alwaysOriginal ? .original : .template)
          .font(.system(size: 20))
      }
      if showsTitle {
        Text(item.title)
          .font(.system(size: 17))
          .lineLimit(1)
          .truncationMode(.tail)
        Spacer(minLength: 0)
      }
    }
    .foregroundStyle(selected ? Color.white : Color.primary)
    .padding(.leading, showsTitle ? (brush ? 8 : 12) : (brush ? 6 : 8))
    .padding(.trailing, showsTitle ? 14 : (brush ? 6 : 8))
    .frame(maxWidth: .infinity, minHeight: 52, maxHeight: 52)
    .background {
      if selected {
        /* The selection slides from item to item. */
        Capsule()
          .fill(.clear)
          .panelGlass(.tinted(.blue), in: Capsule())
          .matchedGeometryEffect(id: "selection", in: namespace)
      }
      else if emphasized {
        Capsule().fill(.clear).panelGlass(in: Capsule())
      }
    }
    .overlay(alignment: .bottomTrailing) {
      if item.grouped {
        Image(systemName: "arrowtriangle.down.fill")
          .resizable()
          .frame(width: 7, height: 7)
          .foregroundStyle(selected ? Color.white : Color.secondary)
          .padding(6)
          .accessibilityHidden(true)
      }
    }
    .contentShape(Capsule())
  }
}

struct Rail: View {
  let items: [RailItem]
  let active: String
  let state: RailState
  let label: RailLabel
  let width: CGFloat
  let height: CGFloat
  /** Brushes rather than tools. */
  let brush: Bool
  /** Where the rail would rest at the middle and at the bottom, to dock it by its drag. */
  let dockDistance: CGFloat
  let choose: (RailItem) -> Void
  @Namespace private var namespace

  private var animation: Animation? {
    UIAccessibility.isReduceMotionEnabled ? nil : .spring(response: 0.32, dampingFraction: 0.84)
  }

  /** The compact rail shows the active item, or the first while none is active. */
  private var shown: [RailItem] {
    if state.listVisible { return items }
    return [items.first { $0.id == active } ?? items.first].compactMap { $0 }
  }

  var body: some View {
    VStack(spacing: 0) {
      if state.listVisible {
        handle
      }
      ScrollViewReader { scroller in
        ScrollView(.vertical, showsIndicators: false) {
          /* Evenly spaced, as wide apart as the gaps between groups. */
          VStack(spacing: 12) {
            ForEach(shown) { item in
              itemView(item)
                .id(item.id)
            }
          }
          .padding(6)
        }
        .scrollDisabled(!state.listVisible)
        .scrollBounceBehavior(.basedOnSize)
        .onChange(of: state.listVisible) { _, visible in
          if visible { scroller.scrollTo(active, anchor: .center) }
        }
      }
    }
    .frame(width: width, height: height)
    .clipShape(RoundedRectangle(cornerRadius: 32, style: .continuous))
    .panelGlass(.clear, in: RoundedRectangle(cornerRadius: 32, style: .continuous))
    .offset(y: state.dragOffset)
    /* The finger on the rail: dragging the closed rail up or down docks it, and holding an item
     * slides to another. An open list scrolls instead. */
    .simultaneousGesture(dockDrag)
    .accessibilityIdentifier(brush ? "blender.brushbar" : "blender.toolbar")
  }

  private func itemView(_ item: RailItem) -> some View {
    let selected = state.listVisible && item.id == active
    return RailItemView(item: item, selected: selected,
                        emphasized: state.listVisible && state.hovered == item.id,
                        showsTitle: state.expanded, brush: brush, namespace: namespace)
      .onTapGesture { tap(item) }
      .simultaneousGesture(pressSelect(item))
      .accessibilityAddTraits(.isButton)
      .accessibilityAction { tap(item) }
    .onGeometryChange(for: CGRect.self) { $0.frame(in: .global) } action: { state.setFrame($0, for: item.id) }
    .onDisappear { state.setFrame(nil, for: item.id) }
    .onHover { inside in
      if inside { label.show(item.title, item: item.id, rail: state, transient: false) }
      else { label.hoverEnded() }
    }
    .contextMenu {
      if item.grouped && state.listVisible && !state.pressSelecting { groupMenu(item) }
    }
    .accessibilityLabel(item.title)
    .accessibilityIdentifier(item.id)
    .accessibilityAddTraits(item.id == active ? .isSelected : [])
    .accessibilityHint(item.grouped ? "Touch and hold for related tools" :
                       brush ? "Touch and hold to choose from all brushes" : "")
  }

  /** A group's tools, while the list is open; an empty menu is not shown. */
  @ViewBuilder
  private func groupMenu(_ item: RailItem) -> some View {
    ForEach(Array(groupSections(item).enumerated()), id: \.offset) { _, section in
      Section {
        ForEach(section) { child in
          Button {
            pick(child, closing: false)
          } label: {
            Label {
              Text(child.title)
            } icon: {
              if let image = child.image { Image(uiImage: image) }
            }
          }
        }
      }
    }
  }

  private func groupSections(_ item: RailItem) -> [[RailItem]] {
    var sections: [[RailItem]] = [[]]
    for (index, child) in item.group.enumerated() {
      if item.groupBreaks.contains(index) && !sections[sections.count - 1].isEmpty { sections.append([]) }
      sections[sections.count - 1].append(child)
    }
    return sections
  }

  /* -------------------------------------------------------------------- */
  /* Choosing */

  private func pick(_ item: RailItem, closing: Bool) {
    state.choose(item.id)
    choose(item)
    if closing && state.expanded {
      withAnimation(animation) { state.expanded = false }
    }
    /* The label names it, beside where it ends up. */
    label.show(item.title, item: item.id, rail: state, transient: true)
  }

  private func tap(_ item: RailItem) {
    if state.suppressTap {
      state.suppressTap = false
      return
    }
    if !state.listVisible {
      /* A tap on the closed rail opens it to pick another. */
      label.hide()
      withAnimation(animation) { state.expanded = true }
      return
    }
    /* Opened by a tap, picking closes it again; a docked list stays open. */
    pick(item, closing: state.expanded && !state.docked)
  }

  /** A hold opens the rail where it may: a group's own menu takes the hold while the list is
   * open, and an open list does not open again. */
  private func canBeginPress(on item: RailItem) -> Bool {
    !state.expanded && !(state.listVisible && item.grouped)
  }

  /** Holding an item opens the rail; the finger slides to another and chooses it on release. */
  private func pressSelect(_ item: RailItem) -> some Gesture {
    LongPressGesture(minimumDuration: 0.22, maximumDistance: 10)
      .sequenced(before: DragGesture(minimumDistance: 0, coordinateSpace: .global))
      .onChanged { value in
        guard case .second(true, let drag) = value else { return }
        if !state.pressSelecting {
          guard canBeginPress(on: item) else { return }
          state.pressSelecting = true
          state.dragOffset = 0
          state.hovered = item.id
          label.hide()
          withAnimation(animation) { state.expanded = true }
        }
        if let drag, let id = state.item(at: drag.location) { state.hovered = id }
      }
      .onEnded { value in
        guard state.pressSelecting, case .second(true, let drag) = value else { return }
        pressEnded(at: drag?.location)
      }
  }

  private func pressEnded(at point: CGPoint?) {
    state.pressSelecting = false
    /* The item's tap ends with the hold: it is not one. */
    state.suppressTap = true
    DispatchQueue.main.async { state.suppressTap = false }
    let id = point.flatMap { state.item(at: $0) } ?? state.hovered
    state.hovered = nil
    if let item = items.first(where: { $0.id == id }) {
      pick(item, closing: true)
    }
    else {
      withAnimation(animation) { state.expanded = false }
    }
  }

  /* -------------------------------------------------------------------- */
  /* Docking */

  private func collapse() {
    label.hide()
    state.pressSelecting = false
    state.hovered = nil
    withAnimation(.spring(response: 0.34, dampingFraction: 0.82)) {
      state.expanded = false
      state.docked = false
      state.dragOffset = 0
    }
  }

  private func dock(_ docked: Bool) {
    label.hide()
    state.pressSelecting = false
    state.hovered = nil
    withAnimation(.spring(response: 0.34, dampingFraction: 0.82)) {
      state.expanded = false
      state.docked = docked
      state.dragOffset = 0
    }
  }

  private var dockDrag: some Gesture {
    DragGesture(minimumDistance: 10, coordinateSpace: .global)
      .onChanged { value in
        guard !state.listVisible, !state.pressSelecting,
              abs(value.translation.height) > abs(value.translation.width) * 0.4
        else { return }
        state.dragOffset = value.translation.height
      }
      .onEnded { value in
        guard !state.listVisible, !state.pressSelecting, state.dragOffset != 0 else { return }
        state.suppressTap = true
        DispatchQueue.main.async { state.suppressTap = false }
        let velocity = value.velocity.height
        var docked: Bool
        if brush {
          /* The brush rail sits above the tool rail: dock it on a clear upward drag. */
          docked = value.translation.height < -40
        }
        else {
          /* Whichever resting place is nearer. */
          docked = abs(value.translation.height + dockDistance) < abs(value.translation.height)
        }
        if velocity < -520 { docked = true }
        else if velocity > 520 { docked = false }
        dock(docked)
      }
  }

  /** At the top of an open list: tap or pull it down to close the list; pull a temporarily
   * opened tool list up to dock it. */
  private var handle: some View {
    Button(action: collapse) {
      Capsule()
        .fill(Color.secondary)
        .frame(width: 28, height: 5)
        .frame(maxWidth: .infinity, minHeight: 44, maxHeight: 44)
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .help(brush ? "Collapse Brushes" : "Collapse Toolbar")
    .accessibilityLabel(brush ? "Collapse Brushes" : "Collapse Toolbar")
    .accessibilityIdentifier(brush ? "blender.brushbar.handle" : "blender.toolbar.handle")
    .highPriorityGesture(
      DragGesture(minimumDistance: 8, coordinateSpace: .global)
        .onChanged { value in
          let y = value.translation.height
          let canRaise = !brush && !state.docked
          state.dragOffset = y < 0 && canRaise ? max(-dockDistance, y) : min(32, max(0, y * 0.3))
        }
        .onEnded { value in
          let y = value.translation.height
          let velocity = value.velocity.height
          if !brush && !state.docked && dockDistance > 0 &&
            (-y > dockDistance * 0.5 || (y < -8 && velocity < -420))
          {
            /* Keep the list open, now docked rather than a temporary peek. */
            dock(true)
          }
          else if y > 36 || (y > 8 && velocity > 420) {
            collapse()
          }
          else {
            withAnimation(.easeOut(duration: 0.2)) { state.dragOffset = 0 }
          }
        })
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The column
 * \{ */

struct ToolRails: View {
  let model: ShellModel
  @State private var tools = RailState()
  @State private var brushes = RailState()
  @State private var label = RailLabel()
  /** While asset libraries reload, the brush rail stays rather than flickering away. */
  @State private var brushesAvailable = false

  var body: some View {
    GeometryReader { proxy in
      let safe = CGRect(origin: .zero, size: proxy.size).inset(by: model.safeAreaInsets)
      let toolItems = RailItem.tools(model.tools, mode: model.mode)
      let brushItems = RailItem.brushes(model.brushes)
      let layout = RailLayout(safe: safe, tray: 0, tools: tools, brushes: brushesAvailable ? brushes : nil)
      ZStack(alignment: .topLeading) {
        if brushesAvailable {
          Rail(items: brushItems, active: brushes.active(model.brushes?.string("active") ?? ""),
               state: brushes, label: label, width: layout.brushWidth, height: layout.brushHeight,
               brush: true, dockDistance: 0) { item in
            WM_apple_ios_ui_brush_activate(item.id)
          }
          .position(x: safe.minX + 14 + layout.brushWidth / 2,
                    y: layout.toolTop - 10 - layout.brushHeight / 2)
          .transition(.opacity)
        }
        Rail(items: toolItems, active: tools.active(model.activeTool), state: tools, label: label,
             width: layout.toolWidth, height: layout.toolHeight, brush: false,
             dockDistance: layout.dockDistance) { item in
          WM_apple_ios_ui_tool_set_by_id(item.id)
        }
        .position(x: safe.minX + 14 + layout.toolWidth / 2, y: layout.toolTop + layout.toolHeight / 2)
        railLabel(safe: safe, railRight: safe.minX + 14 + max(layout.toolWidth, brushesAvailable ? layout.brushWidth : 0))
      }
      .frame(width: proxy.size.width, height: proxy.size.height, alignment: .topLeading)
      .animation(UIAccessibility.isReduceMotionEnabled ? nil : .spring(response: 0.32, dampingFraction: 0.84),
                 value: layout)
      .onChange(of: layout.toolWidth, initial: true) { _, width in model.toolRailWidth = width }
      .onChange(of: model.brushes.map { $0 as NSDictionary }, initial: true) { _, snapshot in
        let brushes = snapshot as? JSON
        let loading = brushes?.bool("loading") ?? false
        let available = !(brushes?.array("items").isEmpty ?? true) || (loading && brushesAvailable)
        if available != brushesAvailable {
          if !available {
            self.brushes.expanded = false
            self.brushes.pressSelecting = false
            self.brushes.hovered = nil
          }
          withAnimation(.easeInOut(duration: 0.24)) { brushesAvailable = available }
        }
      }
    }
    .ignoresSafeArea()
  }

  @ViewBuilder
  private func railLabel(safe: CGRect, railRight: CGFloat) -> some View {
    if let text = label.text {
      let bounds = safe.insetBy(dx: 8, dy: 8)
      let x = railRight + 10
      let width = max(44, bounds.maxX - x)
      /* Beside the rail, level with the item, within the window. */
      Text(text)
        .font(.subheadline)
        .foregroundStyle(.primary)
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .panelGlass(in: Capsule())
        .frame(maxWidth: width, alignment: .leading)
        .frame(width: width, alignment: .leading)
        .position(x: x + width / 2, y: min(max(label.anchor.midY, bounds.minY + 18), bounds.maxY - 18))
        .allowsHitTesting(false)
        .accessibilityIdentifier("blender.toolbar.tool-name")
        .transition(.opacity.combined(with: .offset(x: -5)))
    }
  }
}

/** Where the rails rest in their column: compact rails are 64 points, an open list takes the
 * rest of the column, and two open lists split it. */
struct RailLayout: Equatable {
  var toolWidth: CGFloat
  var toolHeight: CGFloat
  var brushWidth: CGFloat
  var brushHeight: CGFloat
  /** The tool rail's top edge, in the window. */
  var toolTop: CGFloat
  /** How far a docked tool rail rises from the bottom resting place. */
  var dockDistance: CGFloat

  @MainActor
  init(safe: CGRect, tray: CGFloat, tools: RailState, brushes: RailState?) {
    let compact: CGFloat = 64, gap: CGFloat = 10, topInset: CGFloat = 16, bottomInset: CGFloat = 12
    let height = max(0, safe.height - tray)
    /* Leave a usable lane for the quick actions beside the gizmo, even in a narrow window. */
    let expandedWidth = min(240, max(64, safe.width - 284))
    let maxList = min(max(64, safe.height * 0.58), max(64, safe.height - 112))
    toolWidth = tools.expanded ? expandedWidth : compact
    brushWidth = brushes?.expanded == true ? expandedWidth : compact
    toolHeight = compact
    brushHeight = compact
    if let brushes {
      let column = max(2 * compact, height - topInset - bottomInset - gap)
      if tools.listVisible && brushes.listVisible {
        toolHeight = max(compact, min(maxList, floor(column / 2)))
        brushHeight = toolHeight
      }
      else if tools.listVisible {
        toolHeight = max(compact, min(maxList, column - compact))
      }
      else if brushes.listVisible {
        brushHeight = max(compact, min(maxList, column - compact))
      }
    }
    else if tools.listVisible {
      toolHeight = maxList
    }
    /* Docking either rail centers the pair; otherwise it rests at the bottom. */
    let group = toolHeight + (brushes != nil ? gap + brushHeight : 0)
    let centered = tools.docked || brushes?.docked == true
    let bottom = (centered ? max(bottomInset, (height - group) / 2) : bottomInset) + tray
    toolTop = safe.maxY - bottom - toolHeight
    /* The middle resting place of a compact tool rail, measured from the bottom one. */
    dockDistance = max(0, (height - compact) / 2 - bottomInset)
  }
}

/** \} */
