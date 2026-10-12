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
  /** Closed, the brushes rest at the top of their edge rather than above the navigation gizmo. */
  var restsAtTop = false
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
  /** The closed brushes' other places, from where they rest: docked open halfway down their
   * edge, and their other resting place, at the top or the bottom. */
  var brushStops: (middle: CGFloat, other: CGFloat) = (0, 0)
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
          /* The selection slides to a new tool or brush, however it was chosen. */
          .animation(animation, value: active)
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
    /* Only on the closed rail: on an open list, even a drag that does nothing takes the finger
     * from the scroll view, which then cannot scroll. */
    .simultaneousGesture(dockDrag, including: state.listVisible ? .subviews : .all)
    .accessibilityIdentifier(brush ? "blender.brushbar" : "blender.toolbar")
  }

  private func itemView(_ item: RailItem) -> some View {
    let selected = state.listVisible && item.id == active
    return RailItemView(item: item, selected: selected,
                        emphasized: state.listVisible && state.hovered == item.id,
                        showsTitle: state.expanded, brush: brush, namespace: namespace)
      .onTapGesture { tap(item) }
      .simultaneousGesture(pressSelect(item), including: state.listVisible ? .subviews : .all)
      .accessibilityAddTraits(.isButton)
      .accessibilityAction { tap(item) }
    .contentShape(.hoverEffect, Capsule())
    /* The system's own highlight under a hovering Pencil or pointer. */
    .hoverEffect(.highlight)
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
      /* A tap on the closed rail opens it to pick another; the brushes open docked, as they
       * start. */
      label.hide()
      withAnimation(animation) {
        if brush { state.docked = true } else { state.expanded = true }
      }
      return
    }
    /* Opened by a tap, picking closes it again; a docked list stays open. */
    pick(item, closing: state.expanded && !state.docked)
  }

  /** A hold opens the closed rail to slide to another item. An open list scrolls instead, and
   * a group's own menu takes the hold there. */
  private func canBeginPress(on item: RailItem) -> Bool {
    !state.listVisible
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
        if brush {
          /* The closed brushes go to whichever place is nearest where they are let go, carried
           * on by a flick: docked open halfway down, or resting at the top or the bottom. */
          let landing = value.translation.height + velocity * 0.15
          let stops: [CGFloat] = [0, brushStops.middle, brushStops.other]
          let nearest = stops.min { abs($0 - landing) < abs($1 - landing) } ?? 0
          if nearest == brushStops.middle {
            dock(true)
            return
          }
          withAnimation(.spring(response: 0.34, dampingFraction: 0.82)) {
            if nearest == brushStops.other { state.restsAtTop.toggle() }
            state.dragOffset = 0
          }
          return
        }
        /* Whichever resting place is nearer. */
        var docked = abs(value.translation.height + dockDistance) < abs(value.translation.height)
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

/** Mirrors the controls held while drawing to the right, and the brushes to the left, for
 * drawing with the left hand. */
let leftHandedInterfaceKey = "BlenderLeftHandedInterface"

struct ToolRails: View {
  let model: ShellModel
  @State private var tools = RailState()
  /** The brushes start docked open, halfway down their edge. */
  @State private var brushes: RailState = {
    let state = RailState()
    state.docked = true
    return state
  }()
  @State private var label = RailLabel()
  @State private var intent = SelectIntentState()
  /** While asset libraries reload, the brush rail stays rather than flickering away. */
  @State private var brushesAvailable = false
  @AppStorage(leftHandedInterfaceKey) private var leftHanded = false

  var body: some View {
    GeometryReader { proxy in
      /* Below the top bar, which the window's safe area does not include. */
      let top = max(model.safeAreaInsets.top, model.topBarBottom - proxy.frame(in: .global).minY)
      let insets = UIEdgeInsets(top: top, left: 0, bottom: model.safeAreaInsets.bottom, right: 0)
      let safe = CGRect(origin: .zero, size: proxy.size).inset(by: insets)
      let toolItems = RailItem.tools(model.tools, mode: model.mode)
      let brushItems = RailItem.brushes(model.brushes)
      let content = SidebarContent(brush: model.brush, buttons: model.direction?.bool("hidden") != true)
      let color = (model.brush?["color"] as? [NSNumber])?.map(\.doubleValue)
      let gizmo = model.navigationGizmo
      let gizmoTop = gizmo.count == 3 ? CGFloat(gizmo[1]) * proxy.size.height - CGFloat(gizmo[2]) * proxy.size.width :
                                        safe.maxY - 100
      let layout = RailLayout(safe: safe, tray: model.bottomTray, tools: tools,
                              brushes: brushesAvailable ? brushes : nil, brushCount: brushItems.count,
                              sidebar: content,
                              colorWell: color != nil, leftHanded: leftHanded, gizmoTop: gizmoTop)
      ZStack(alignment: .topLeading) {
        if let color {
          BrushColorWell(color: color)
            .position(layout.colorCenter)
            .transition(.opacity.combined(with: .scale(scale: 0.8)))
        }
        if brushesAvailable {
          Rail(items: brushItems, active: brushes.active(model.brushes?.string("active") ?? ""),
               state: brushes, label: label, width: layout.brushWidth, height: layout.brushHeight,
               brush: true, dockDistance: 0, brushStops: layout.brushStops) { item in
            WM_apple_ios_ui_brush_activate(item.id)
          }
          .position(layout.brushCenter)
          .transition(.opacity)
        }
        Rail(items: toolItems, active: tools.active(model.activeTool), state: tools, label: label,
             width: layout.toolWidth, height: layout.toolHeight, brush: false,
             dockDistance: layout.dockDistance) { item in
          WM_apple_ios_ui_tool_set_by_id(item.id)
        }
        .position(layout.toolCenter)
        if !content.isEmpty {
          PaintSidebar(state: intent, direction: model.direction, brush: model.brush, content: content,
                       leading: layout.sidebarLeading)
            .position(layout.sidebarCenter)
            /* An open tool list takes the edge: the sidebar steps aside until it closes. */
            .offset(x: layout.sidebarTucked ? (layout.sidebarLeading ? -96 : 96) : 0)
            .opacity(layout.sidebarTucked ? 0 : 1)
            .allowsHitTesting(!layout.sidebarTucked)
            .transition(.opacity.combined(with: .scale(scale: 0.9)))
        }
        railLabel(safe: safe, layout: layout)
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
    .ignoresSafeArea(edges: .vertical)
  }

  @ViewBuilder
  private func railLabel(safe: CGRect, layout: RailLayout) -> some View {
    if let text = label.text {
      let bounds = safe.insetBy(dx: 8, dy: 8)
      /* Beside the rail, towards the viewport, level with the item, within the window. */
      let brush = label.rail === brushes
      let toward = brush && !layout.brushLeading ? -1.0 : 1.0
      let edge = brush ? layout.brushCenter.x + toward * layout.brushWidth / 2 :
                         layout.toolCenter.x + layout.toolWidth / 2
      let x = edge + toward * 10
      let width = max(44, toward > 0 ? bounds.maxX - x : x - bounds.minX)
      Text(text)
        .font(.subheadline)
        .foregroundStyle(.primary)
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .panelGlass(in: Capsule())
        .frame(maxWidth: width, alignment: toward > 0 ? .leading : .trailing)
        .frame(width: width, alignment: toward > 0 ? .leading : .trailing)
        .position(x: x + toward * width / 2, y: min(max(label.anchor.midY, bounds.minY + 18), bounds.maxY - 18))
        .allowsHitTesting(false)
        .accessibilityIdentifier("blender.toolbar.tool-name")
        .transition(.opacity.combined(with: .offset(x: -5 * toward)))
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Held + and -
 *
 * Held while tapping or dragging with the Pencil, as Shift and Ctrl would be. Where Blender
 * selects, + adds to the selection and - subtracts from it while held, for taps as well as box,
 * lasso and circle selection, and neither is on at rest.
 * Where Blender strokes brushes, they are the stroke's direction instead, and one is always on: a
 * tap picks it (the brush's own Direction, such as Inflate or Deflate, where it has one) and
 * holding the other strokes the other way while held.
 * \{ */

enum SelectIntent: Int {
  case none = 0, add = 1, subtract = 2
}

@MainActor
@Observable
final class SelectIntentState {
  /** Blender's stroke direction in modes that paint with brushes, or none where it selects. */
  struct Strokes: Equatable {
    /** The brush has a Direction of its own, which taps set. */
    var brush: Bool
    var inverted: Bool
    var addName: String
    var subtractName: String
  }

  /** The one shown on. */
  private(set) var active: SelectIntent = .none
  private(set) var strokes: Strokes?
  @ObservationIgnored private var held: SelectIntent = .none
  @ObservationIgnored private var pressed = Date.distantPast
  /** Without a Direction of its own, strokes go the other way while this is set. */
  @ObservationIgnored private var strokesInverted = false
  /** A direction just tapped, shown until Blender's brush follows. */
  @ObservationIgnored private var pendingInverted: (value: Bool, until: Date)?

  func update(strokes json: JSON?) {
    let next = json.map {
      Strokes(brush: $0.bool("brush"), inverted: $0.bool("inverted"),
              addName: $0.string("add") ?? "Add", subtractName: $0.string("subtract") ?? "Subtract")
    }
    if let pending = pendingInverted, next?.inverted == pending.value || Date() > pending.until {
      pendingInverted = nil
    }
    if (next == nil) != (strokes == nil) {
      /* Selecting and stroking each start at rest. */
      held = .none
      strokesInverted = false
      pendingInverted = nil
    }
    guard next != strokes else { return }
    strokes = next
    apply()
  }

  func press(_ intent: SelectIntent) {
    held = intent
    pressed = Date()
    apply()
    UIImpactFeedbackGenerator(style: .light).impactOccurred()
  }

  func release(_ intent: SelectIntent) {
    guard held == intent else { return }
    held = .none
    if strokes != nil && Date().timeIntervalSince(pressed) < 0.3 {
      choose(intent)
    }
    apply()
  }

  /** What a tap does when stroking: pick the direction. */
  func choose(_ intent: SelectIntent) {
    if let strokes {
      let inverted = intent == .subtract
      if strokes.brush {
        pendingInverted = (inverted, Date().addingTimeInterval(1))
        Engine.setting("brush_direction", inverted ? "subtract" : "add")
      }
      else {
        strokesInverted = inverted
      }
    }
    apply()
  }

  func clear() {
    held = .none
    strokesInverted = false
    apply()
  }

  private func apply() {
    let intent: SelectIntent
    if let strokes {
      let inverted = strokes.brush ? (pendingInverted?.value ?? strokes.inverted) : strokesInverted
      let resting: SelectIntent = inverted ? .subtract : .add
      active = held == .none ? resting : held
      /* The brush strokes its own way at rest, once it has the direction just tapped. */
      if held != .none { intent = held }
      else if strokes.brush { intent = pendingInverted == nil ? .none : resting }
      else { intent = strokesInverted ? .subtract : .none }
    }
    else {
      active = held
      intent = held
    }
    WM_apple_ios_ui_select_intent_set(Int32(intent.rawValue))
  }
}

/** What the sidebar beside the viewport holds: the brush's size and strength, and the + and -.
 * Where Blender selects, it is only the + and -. */
struct SidebarContent: Equatable {
  var size = false
  var strength = false
  var buttons = false

  init(brush: JSON?, buttons: Bool) {
    size = brush?.dict("size") != nil
    strength = brush?.dict("strength") != nil
    self.buttons = buttons
  }

  var isEmpty: Bool { !size && !strength && !buttons }
  /** Only the + and -: small enough to sit above the tool rail. */
  var buttonsOnly: Bool { buttons && !size && !strength }

  let sliderHeight: CGFloat = 148
  static let buttonsHeight: CGFloat = 52 + 4 + 52

  var height: CGFloat {
    let parts: [CGFloat] = [size ? sliderHeight : 0, strength ? sliderHeight : 0,
                            buttons ? Self.buttonsHeight : 0].filter { $0 > 0 }
    return parts.isEmpty ? 0 : parts.reduce(0, +) + CGFloat(parts.count - 1) * 8 + 12
  }
}

/** One control on the side of the viewport, Procreate-style: size and strength, and the + and -
 * below them, nearest the hand. */
struct PaintSidebar: View {
  let state: SelectIntentState
  let direction: JSON?
  let brush: JSON?
  let content: SidebarContent
  /** On the leading edge: value labels open towards the viewport, on the other side. */
  let leading: Bool
  static let width: CGFloat = 64

  @State private var pressing: SelectIntent?
  /** One selection between the + and -, sliding from one to the other as the rails' does. */
  @Namespace private var selection

  var body: some View {
    VStack(spacing: 8) {
      if content.size, let size = brush?.dict("size") {
        let pixels = size.bool("pixels")
        SidebarSlider(key: "size", title: size.string("label") ?? "Radius", symbol: "circle.dashed",
                      json: size, logarithmic: true, height: content.sliderHeight, leading: leading,
                      unit: pixels ? "px" : "m", preview: .size(pixels: pixels),
                      format: { pixels ? String(Int($0.rounded())) : String(format: "%.3g", $0) })
      }
      if content.strength, let strength = brush?.dict("strength") {
        SidebarSlider(key: "strength", title: "Strength", symbol: "drop.fill", json: strength,
                      logarithmic: false, height: content.sliderHeight, leading: leading, unit: "%",
                      preview: .strength, format: { String(Int(($0 * 100).rounded())) })
      }
      if content.buttons {
        VStack(spacing: 4) {
          button(.add, symbol: "plus", label: state.strokes?.addName ?? "Add to Selection")
          button(.subtract, symbol: "minus", label: state.strokes?.subtractName ?? "Subtract from Selection")
        }
        .transition(.opacity.combined(with: .scale(scale: 0.9)))
      }
    }
    .padding(6)
    .frame(width: Self.width, height: content.height)
    .panelGlass(.clear, in: RoundedRectangle(cornerRadius: 32, style: .continuous))
    .animation(UIAccessibility.isReduceMotionEnabled ? nil : .spring(response: 0.32, dampingFraction: 0.84),
               value: state.active)
    .animation(UIAccessibility.isReduceMotionEnabled ? nil : .spring(response: 0.2, dampingFraction: 0.7),
               value: pressing)
    .onChange(of: direction.map { $0 as NSDictionary }, initial: true) { _, json in
      state.update(strokes: json as? JSON)
    }
    .onChange(of: content.buttons) { _, shown in
      if !shown { state.clear() }
    }
    .onDisappear { state.clear() }
    .accessibilityElement(children: .contain)
    .accessibilityIdentifier("blender.sidebar")
  }

  private func button(_ intent: SelectIntent, symbol: String, label: String) -> some View {
    let on = state.active == intent
    return Image(systemName: symbol)
      .font(.system(size: 20, weight: .semibold))
      .foregroundStyle(on ? Color.white : Color.primary)
      .frame(maxWidth: .infinity, minHeight: 52, maxHeight: 52)
      .contentTransition(.symbolEffect(.replace))
      .background {
        if on {
          Capsule()
            .fill(.clear)
            .panelGlass(.tinted(.blue), in: Capsule())
            .matchedGeometryEffect(id: "selection", in: selection)
            .transition(.opacity.combined(with: .scale(scale: 0.8)))
        }
      }
      .scaleEffect(pressing == intent ? 0.94 : 1)
      .contentShape(Capsule())
      .contentShape(.hoverEffect, Capsule())
      .hoverEffect(.highlight)
      /* Never completes, so it reports the finger landing and lifting, however long it stays. */
      .onLongPressGesture(minimumDuration: .infinity, maximumDistance: .infinity, perform: {}) { down in
        pressing = down ? intent : nil
        if down {
          state.press(intent)
        }
        else {
          state.release(intent)
        }
      }
      .accessibilityElement()
      .accessibilityLabel(label)
      .accessibilityAddTraits(on ? [.isButton, .isSelected] : .isButton)
      .accessibilityAction { if state.strokes != nil { state.choose(intent) } }
      .accessibilityIdentifier("blender.select-intent.\(intent == .add ? "add" : "subtract")")
  }
}

/** Send one of the brush's values to Blender; \a commit ends a drag with an undo step. */
@MainActor
private func sendBrushValue(_ key: String, _ value: Any, commit: Bool = false) {
  let encoded: String
  if let color = value as? [Double] {
    encoded = "[" + color.map { String($0) }.joined(separator: ",") + "]"
  }
  else {
    encoded = String(value as? Double ?? 0)
  }
  /* The key first: Blender keeps only the newest of a drag's values for each key. */
  Engine.setting("native_brush", "{\"key\":\"\(key)\",\"value\":\(encoded),\"commit\":\(commit)}")
}

/**
 * A tall slider in the sidebar. Dragging moves the value from where it is, not to the finger;
 * sliding the finger out sideways while dragging moves it more finely. A tap types a number.
 */
struct SidebarSlider: View {
  let key: String
  let title: String
  let symbol: String
  let json: JSON
  /** Along a curve, so small sizes get as much of the slider as large ones. */
  let logarithmic: Bool
  let height: CGFloat
  let leading: Bool
  let unit: String
  /** What dragging shows beside the slider, towards the viewport. */
  enum Preview {
    /** The brush at its size on screen, for a size in pixels. */
    case size(pixels: Bool)
    /** A dab filled as strongly as the brush paints. */
    case strength
  }
  let preview: Preview
  let format: (Double) -> String

  @Environment(\.displayScale) private var displayScale

  private struct Drag {
    var position: Double
    var lastY: CGFloat
    var moved = false
  }

  @State private var drag: Drag?
  /** The value set here, shown until Blender's snapshot has it. */
  @State private var local: (value: Double, until: Date)?
  @State private var entering = false
  @State private var entry = ""

  private var low: Double { json.double("min") ?? 0 }
  private var high: Double { max(low + 1e-6, json.double("max") ?? 1) }

  private var value: Double {
    let blender = json.double("value") ?? low
    if let local, drag != nil || (Date() < local.until && abs(local.value - blender) > 1e-6) {
      return local.value
    }
    return blender
  }

  private func position(_ value: Double) -> Double {
    let t = logarithmic && low > 0 ? log(max(value, low) / low) / log(high / low) : (value - low) / (high - low)
    return min(1, max(0, t))
  }

  private func value(at position: Double) -> Double {
    logarithmic && low > 0 ? low * pow(high / low, position) : low + (high - low) * position
  }

  var body: some View {
    let width: CGFloat = 52
    let t = position(value)
    let fill = width + (height - width) * t
    ZStack(alignment: .bottom) {
      Capsule().fill(Color.primary.opacity(0.08))
      Capsule()
        .fill(Color.accentColor.opacity(drag != nil ? 1 : 0.85))
        .frame(height: fill)
      Text(format(value))
        .font(.system(size: 13, weight: .semibold).monospacedDigit())
        .foregroundStyle(.white)
        .lineLimit(1)
        .minimumScaleFactor(0.6)
        .frame(width: width - 8, height: width)
    }
    .overlay(alignment: .top) {
      Image(systemName: symbol)
        .font(.system(size: 13, weight: .semibold))
        .foregroundStyle(fill > height - 30 ? Color.white : Color.secondary)
        .padding(.top, 12)
        .allowsHitTesting(false)
    }
    .frame(width: width, height: height)
    .overlay(alignment: leading ? .leading : .trailing) {
      if drag?.moved == true {
        /* As Procreate's: the setting, and below it the brush as it will paint. */
        VStack(spacing: 12) {
          VStack(spacing: 2) {
            Text(title)
              .font(.caption)
              .foregroundStyle(.secondary)
            Text("\(format(value)) \(unit)")
              .font(.headline.monospacedDigit())
          }
          previewDab
        }
        .padding(16)
        .frame(minWidth: 112, minHeight: 112)
        .panelGlass(in: RoundedRectangle(cornerRadius: 24, style: .continuous))
        .fixedSize()
        .offset(x: (leading ? 1 : -1) * (width + 18))
        .allowsHitTesting(false)
        .transition(.opacity.combined(with: .scale(scale: 0.92)))
      }
    }
    .contentShape(Capsule())
    .gesture(
      DragGesture(minimumDistance: 0)
        .onChanged { gesture in
          var current = drag ?? Drag(position: position(value), lastY: gesture.startLocation.y)
          let dy = gesture.location.y - current.lastY
          current.lastY = gesture.location.y
          if !current.moved, abs(gesture.translation.height) + abs(gesture.translation.width) > 4 {
            current.moved = true
          }
          guard current.moved else {
            drag = current
            return
          }
          /* Out to the side, the finger moves the value more finely. */
          let side = max(0, abs(gesture.location.x - width / 2) - width)
          let fine = 1 / (1 + Double(side) / 60)
          let before = current.position
          current.position = min(1, max(0, current.position - Double(dy / (height - width)) * fine))
          drag = current
          tick(from: before, to: current.position)
          let next = value(at: current.position)
          local = (next, .distantFuture)
          sendBrushValue(key, next)
        }
        .onEnded { _ in
          let moved = drag?.moved == true
          drag = nil
          if moved, let local {
            self.local = (local.value, Date().addingTimeInterval(1))
            sendBrushValue(key, local.value, commit: true)
          }
          else {
            entry = format(value)
            entering = true
          }
        })
    .animation(.easeOut(duration: 0.15), value: drag?.moved == true)
    .alert(title, isPresented: $entering) {
      TextField(title, text: $entry)
        .keyboardType(.decimalPad)
      Button("Cancel", role: .cancel) {}
      Button("Set") { setTyped() }
    } message: {
      Text(unit.isEmpty ? "" : "In \(unit == "%" ? "percent" : unit == "px" ? "pixels" : "meters").")
    }
    .accessibilityElement()
    .accessibilityLabel(title)
    .accessibilityValue("\(format(value)) \(unit)")
    .accessibilityAdjustableAction { direction in
      let step = direction == .increment ? 0.05 : -0.05
      let next = value(at: min(1, max(0, t + step)))
      local = (next, Date().addingTimeInterval(1))
      sendBrushValue(key, next, commit: true)
    }
    .accessibilityIdentifier("blender.sidebar.\(key)")
  }

  /** The brush as it will paint: its size on screen, or how strongly. */
  @ViewBuilder
  private var previewDab: some View {
    switch preview {
      case .size(let pixels):
        if pixels {
          /* Blender's pixels are the screen's; very large brushes are shown as large as fits. */
          let diameter = min(320, max(4, CGFloat(value) / displayScale))
          ZStack {
            Circle().fill(Color.white.opacity(0.18))
            Circle().stroke(Color.black.opacity(0.5), lineWidth: 3)
            Circle().stroke(Color.white, lineWidth: 1.5)
          }
          .frame(width: diameter, height: diameter)
        }
      case .strength:
        ZStack {
          Circle().fill(Color.white.opacity(0.05 + 0.85 * min(1, value)))
          Circle().stroke(Color.black.opacity(0.5), lineWidth: 3)
          Circle().stroke(Color.white, lineWidth: 1.5)
        }
        .frame(width: 48, height: 48)
    }
  }

  /** A tick at either end and passing the default. */
  private func tick(from before: Double, to after: Double) {
    let marks = [0, 1, json.double("default").map(position)].compactMap { $0 }
    if marks.contains(where: { ($0 - before) * ($0 - after) < 0 || ($0 == after && before != after) }) {
      UISelectionFeedbackGenerator().selectionChanged()
    }
  }

  private func setTyped() {
    guard var typed = Double(entry.replacingOccurrences(of: ",", with: ".")) else { return }
    if unit == "%" { typed /= 100 }
    let hard = json.double("max").map { max($0, typed) } ?? typed
    typed = max(low, min(hard, typed))
    local = (typed, Date().addingTimeInterval(1))
    sendBrushValue(key, typed, commit: true)
  }
}

/** The brush's color, above the brushes, in modes that paint colors. */
struct BrushColorWell: View {
  let color: [Double]
  static let size: CGFloat = 64

  var body: some View {
    let binding = Binding<Color>(
      get: {
        Color(.sRGB, red: color.count > 0 ? color[0] : 0, green: color.count > 1 ? color[1] : 0,
              blue: color.count > 2 ? color[2] : 0)
      },
      set: { picked in
        let resolved = UIColor(picked)
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        resolved.getRed(&r, green: &g, blue: &b, alpha: &a)
        sendBrushValue("color", [Double(r), Double(g), Double(b)].map { min(1, max(0, $0)) })
      })
    ColorPicker("Color", selection: binding, supportsOpacity: false)
      .labelsHidden()
      .scaleEffect(1.5)
      .frame(width: Self.size, height: Self.size)
      .panelGlass(.clear, in: Circle())
      .accessibilityIdentifier("blender.brush-color")
  }
}


/** \} */

/**
 * Where everything rests around the viewport. The tools sit in the leading bottom corner. The
 * sidebar the other hand holds (the brush's size and strength, and the + and -) sits halfway down
 * the leading edge, or the trailing one for the left hand; where Blender selects it is only the
 * + and -, resting above the tool rail. The color well and brushes run down from the top of the
 * other edge, above the navigation gizmo. Compact rails are 64 points; an open list takes the room
 * its edge has.
 */
struct RailLayout: Equatable {
  var toolWidth: CGFloat
  var toolHeight: CGFloat
  var brushWidth: CGFloat
  var brushHeight: CGFloat
  /** The tool rail's top edge, in the window. */
  var toolTop: CGFloat
  /** How far a docked tool rail rises from the bottom resting place. */
  var dockDistance: CGFloat
  var toolCenter: CGPoint
  var brushCenter: CGPoint
  var colorCenter: CGPoint
  var sidebarCenter: CGPoint
  var sidebarLeading: Bool
  /** Out of the way of an open tool list, or without the room to show. */
  var sidebarTucked: Bool
  var brushLeading: Bool
  /** See #Rail.brushStops. */
  var brushStops: (middle: CGFloat, other: CGFloat)

  static func == (a: RailLayout, b: RailLayout) -> Bool {
    a.toolWidth == b.toolWidth && a.toolHeight == b.toolHeight && a.brushWidth == b.brushWidth &&
      a.brushHeight == b.brushHeight && a.toolTop == b.toolTop && a.dockDistance == b.dockDistance &&
      a.toolCenter == b.toolCenter && a.brushCenter == b.brushCenter && a.colorCenter == b.colorCenter &&
      a.sidebarCenter == b.sidebarCenter && a.sidebarLeading == b.sidebarLeading &&
      a.sidebarTucked == b.sidebarTucked && a.brushLeading == b.brushLeading &&
      a.brushStops == b.brushStops
  }

  @MainActor
  init(safe: CGRect, tray: CGFloat, tools: RailState, brushes: RailState?, brushCount: Int,
       sidebar: SidebarContent, colorWell: Bool, leftHanded: Bool, gizmoTop: CGFloat)
  {
    let compact: CGFloat = 64, gap: CGFloat = 10, topInset: CGFloat = 16, bottomInset: CGFloat = 12
    let inset: CGFloat = 14
    sidebarLeading = !leftHanded
    brushLeading = leftHanded
    /* Leave a usable lane for the quick actions beside the gizmo, even in a narrow window. */
    let expandedWidth = min(240, max(64, safe.width - 284))

    /* The + and - alone join the tool column: above the closed rail, or in the corner below an
     * open or docked one, which rises to make room. */
    let pill = sidebar.buttonsOnly && sidebarLeading
    let below = pill && tools.listVisible
    let reserve = below ? sidebar.height + gap : 0
    let height = max(0, safe.height - tray - reserve)
    let maxList = min(max(64, (safe.height - reserve) * 0.58), max(64, safe.height - reserve - 112))
    toolWidth = tools.expanded ? expandedWidth : compact
    toolHeight = tools.listVisible ? maxList : compact
    /* Docking centers the rail; otherwise it rests at the bottom. */
    let bottom = (tools.docked ? max(bottomInset, (height - toolHeight) / 2) : bottomInset) + tray + reserve
    toolTop = safe.maxY - bottom - toolHeight
    /* The middle resting place of a compact tool rail, measured from the bottom one. */
    dockDistance = max(0, (safe.height - tray - compact) / 2 - bottomInset)
    toolCenter = CGPoint(x: safe.minX + inset + toolWidth / 2, y: toolTop + toolHeight / 2)

    /* The color well above the brushes, on their edge between the top bar and the navigation
     * gizmo, or the tool rail when they share its edge: an open list halfway down, closed ones
     * resting above the gizmo, or at the top where they were dragged. */
    let top = safe.minY + topInset
    let trailingBottom = min(gizmoTop - gap, safe.maxY - tray - bottomInset)
    let columnBottom = brushLeading ? toolTop - gap : trailingBottom
    let colorHeight = colorWell ? BrushColorWell.size + gap : 0
    brushWidth = brushes?.expanded == true ? expandedWidth : compact
    let groupTop: CGFloat
    if brushes?.listVisible == true {
      /* As tall as its brushes (the handle, then 52 points each, 12 apart), as the room allows. */
      let list = 44 + 12 + CGFloat(brushCount) * 52 + CGFloat(max(0, brushCount - 1)) * 12
      brushHeight = max(compact, min(list, columnBottom - top - colorHeight))
      groupTop = top + max(0, (columnBottom - top - colorHeight - brushHeight) / 2)
    }
    else {
      brushHeight = compact
      let group = colorHeight + (brushes != nil ? compact : 0)
      groupTop = brushes?.restsAtTop == true ? top : max(top, columnBottom - group)
    }
    let edgeX = { (width: CGFloat, leading: Bool) in
      leading ? safe.minX + inset + width / 2 : safe.maxX - inset - width / 2
    }
    let restTop = top + colorHeight + compact / 2
    let restBottom = max(top, columnBottom - colorHeight - compact) + colorHeight + compact / 2
    let resting = brushes?.restsAtTop == true ? restTop : restBottom
    brushStops = ((top + columnBottom) / 2 - resting, (brushes?.restsAtTop == true ? restBottom : restTop) - resting)
    colorCenter = CGPoint(x: edgeX(compact, brushLeading), y: groupTop + BrushColorWell.size / 2)
    brushCenter = CGPoint(x: edgeX(brushWidth, brushLeading), y: groupTop + colorHeight + brushHeight / 2)

    /* The sidebar, halfway down its edge where it fits, clear of what sits below it. */
    let h = sidebar.height
    let x = edgeX(PaintSidebar.width, sidebarLeading)
    if pill {
      let y = below ? safe.maxY - tray - bottomInset - h : max(top, toolTop - gap - h)
      sidebarCenter = CGPoint(x: x, y: y + h / 2)
      sidebarTucked = false
    }
    else {
      let lowest = sidebarLeading ? toolTop - gap : trailingBottom
      let y = max(top, min(safe.midY - h / 2, lowest - h))
      sidebarCenter = CGPoint(x: x, y: y + h / 2)
      sidebarTucked = (sidebarLeading && tools.listVisible) || lowest - top < h
    }
  }
}

/** \} */
