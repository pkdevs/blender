/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The Pencil's radial actions: Blender's context menu as pills on up to two rings around the
 * Pencil, chosen by moving the Pencil over one and lifting or squeezing. Raw Pencil routing owns
 * selection and release, so the pills never take touches themselves. Implements
 * `GHOSTIOSPencilMenu`; the view controller finds it by name, see `GHOST_WindowIOS.mm`.
 * All points are UIKit points in this view, never drawable pixels.
 */

#if !os(visionOS)  /* No Apple Pencil on Apple Vision Pro. */

import SwiftUI
import UIKit

/* Pills sit on up to two concentric elliptical rings around the Pencil. */
private let pillHeight: CGFloat = 30
private let pillGap: CGFloat = 6
private let innerSlotsMax = 12
private let outerSlots = 16

private struct PencilRing {
  var slots = 0
  var radiusX: CGFloat = 0
  var radiusY: CGFloat = 0
  var phase: CGFloat = 0

  func point(_ slot: Int, _ center: CGPoint) -> CGPoint {
    let angle = -Double.pi / 2 + Double(phase) + Double(slot) * 2 * Double.pi / Double(slots)
    return CGPoint(x: center.x + CGFloat(cos(angle)) * radiusX, y: center.y + CGFloat(sin(angle)) * radiusY)
  }

  func extent(_ pill: CGSize) -> CGSize { CGSize(width: 2 * radiusX + pill.width, height: 2 * radiusY + pill.height) }
}

private func pillRect(_ center: CGPoint, _ pill: CGSize) -> CGRect {
  CGRect(x: center.x - pill.width / 2, y: center.y - pill.height / 2, width: pill.width, height: pill.height)
}

/** Smallest ring of `slots` pills with the given aspect that overlaps neither itself nor
 * `obstacles` (relative to the menu center), starting the search at `start`. */
private func solveRing(_ slots: Int, aspect: CGFloat, phase: CGFloat, pill: CGSize, obstacles: [CGRect],
                       start: CGFloat) -> PencilRing
{
  var ring = PencilRing(slots: slots, phase: phase)
  var scale = start
  while scale < 2000 {
    ring.radiusX = scale * aspect
    ring.radiusY = scale
    var rects: [CGRect] = []
    var clear = true
    for i in 0..<slots where clear {
      let rect = pillRect(ring.point(i, .zero), pill).insetBy(dx: -pillGap / 2, dy: -pillGap / 2)
      clear = !rects.contains { $0.intersects(rect) } && !obstacles.contains { $0.intersects(rect) }
      rects.append(rect)
    }
    if clear { return ring }
    scale += 2
  }
  return ring
}

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

struct PencilPill: Identifiable {
  let id: Int
  let title: String
  let icon: UIImage?
  let center: CGPoint
  let size: CGSize
  let enabled: Bool
  let checked: Bool
  let destructive: Bool
  let identifier: String
}

@MainActor
final class PencilMenuModel: ObservableObject {
  @Published var pills: [PencilPill] = []
  @Published var selected: Int?
  @Published var label = ""
  @Published var center = CGPoint.zero
  @Published var visible = false
  /** Bumped per page, so a new page's pills grow out of the center. */
  @Published var page = 0
  var appearFrom = CGPoint.zero
}

struct PencilMenuContent: View {
  @ObservedObject var model: PencilMenuModel

  var body: some View {
    ZStack(alignment: .topLeading) {
      if model.visible {
        ForEach(model.pills) { pill in
          PencilPillView(pill: pill, selected: model.selected == pill.id, menuCenter: model.center)
            .id("\(model.page):\(pill.id)")
        }
        Text(model.label)
          .font(.system(size: 12, weight: .semibold))
          .multilineTextAlignment(.center)
          .lineLimit(2)
          .minimumScaleFactor(0.85)
          .padding(.horizontal, 8)
          .padding(.vertical, 4)
          .frame(width: 106, height: 44)
          .glassEffect(.regular, in: Capsule())
          .position(model.center)
      }
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .allowsHitTesting(false)
  }
}

struct PencilPillView: View {
  let pill: PencilPill
  let selected: Bool
  let menuCenter: CGPoint
  @State private var shown = false

  private var foreground: Color {
    if selected { return .white }
    if pill.destructive { return .red }
    if pill.checked { return PanelStyle.selection }
    return .primary
  }

  var body: some View {
    HStack(spacing: pill.icon == nil ? 0 : 6) {
      if let icon = pill.icon {
        Image(uiImage: icon).font(.system(size: 13, weight: .medium))
      }
      Text(pill.title)
        .font(.system(size: 13, weight: .medium))
        .lineLimit(1)
        .truncationMode(.tail)
    }
    .foregroundStyle(foreground)
    .padding(.leading, 10)
    .padding(.trailing, 12)
    .frame(width: pill.size.width, height: pill.size.height)
    .glassEffect(selected ? .regular.tint(pill.destructive ? .red : Color(uiColor: .systemBlue)) : .regular,
                 in: Capsule())
    .opacity(pill.enabled ? 1 : 0.4)
    .scaleEffect(shown ? 1 : 0.8)
    .offset(shown ? .zero : CGSize(width: (menuCenter.x - pill.center.x) * 0.3,
                                   height: (menuCenter.y - pill.center.y) * 0.3))
    .opacity(shown ? 1 : 0)
    .position(pill.center)
    .accessibilityLabel(pill.title)
    .accessibilityValue(pill.checked ? "On" : "")
    .accessibilityIdentifier(pill.identifier)
    .onAppear {
      if UIAccessibility.isReduceMotionEnabled { shown = true }
      else { withAnimation(.spring(response: 0.24, dampingFraction: 0.82)) { shown = true } }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Menu
 * \{ */

@MainActor
@objc(WMIOSSwiftPencilMenu)
public final class SwiftPencilMenu: UIView, @MainActor GHOSTIOSPencilMenu {
  @objc public weak var delegate: GHOSTIOSPencilMenuDelegate?
  @objc public private(set) var active = false
  @objc public private(set) var menuVisible = false

  private let model = PencilMenuModel()
  private let host: UIHostingController<PencilMenuContent>
  private var items: [[String: Any]] = []
  private var history: [(items: [[String: Any]], page: Int, title: String)] = []
  private var title: String?
  private var capturedTouch: UITouch?
  private var lastPencilPoint = CGPoint.zero
  private var anchor = CGPoint.zero
  private var menuCenter = CGPoint.zero
  private var inner = PencilRing()
  private var outer = PencilRing()
  private var perPage = innerSlotsMax
  /** Per pill: (outer ring ? 1000 : 0) + slot. */
  private var slots: [Int] = []
  /** Per pill: index into `items`, or -1 for More. */
  private var tags: [Int] = []
  private var layoutSize = CGSize.zero
  private var page = 0
  private var requestGeneration = 0
  private var loading = false
  private var hasPencilLocation = false
  private var openedOnTouch = false
  private let feedback = UISelectionFeedbackGenerator()

  override public init(frame: CGRect) {
    host = UIHostingController(rootView: PencilMenuContent(model: model))
    super.init(frame: frame)
    backgroundColor = .clear
    autoresizingMask = [.flexibleWidth, .flexibleHeight]
    host.view.backgroundColor = .clear
    host.view.isUserInteractionEnabled = false
    host.safeAreaRegions = []
    host.view.frame = bounds
    host.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    addSubview(host.view)
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

  override public func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
    /* An open menu shields the underlying controls until selection or dismissal. */
    active && bounds.contains(point) ? self : nil
  }

  override public func layoutSubviews() {
    super.layoutSubviews()
    if menuVisible && layoutSize != bounds.size {
      /* Rotation or Stage Manager resizing invalidates the Pencil anchor. */
      cancel()
    }
  }

  private var pageCount: Int { max(1, (items.count + perPage - 1) / perPage) }

  private func updateLabel() {
    if loading || items.isEmpty {
      model.label = loading ? "Loading..." : "No Context Actions"
      return
    }
    if let selected = model.selected {
      model.label = model.pills[selected].title
      return
    }
    /* The menu's name; a menu with several pages adds which page is showing. */
    let name = (title ?? "").isEmpty ? "Actions" : title!
    model.label = pageCount > 1 ? "\(name)\n\(page + 1) / \(pageCount)" : name
  }

  private func select(_ index: Int?) {
    var index = index
    if let current = index, !model.pills[current].enabled { index = nil }
    if model.selected == index { return }
    model.selected = index
    if index != nil { feedback.selectionChanged() }
    updateLabel()
  }

  private func buildPage() {
    select(nil)
    let available = bounds.inset(by: safeAreaInsets).insetBy(dx: 12, dy: 12)
    /* One pill width per menu, fitted to the longest title, keeps the rings regular. */
    let font = UIFont.systemFont(ofSize: 13, weight: .medium)
    let titleWidth = items.map { ceil((($0["title"] as? String) ?? "" as String).size(withAttributes: [.font: font]).width) }
      .max() ?? 0
    let pill = CGSize(width: min(132, max(88, titleWidth + 54)), height: pillHeight)
    let centerRect = CGRect(x: -53 - pillGap, y: -22 - pillGap, width: 106 + 2 * pillGap, height: 44 + 2 * pillGap)
    func fits(_ extent: CGSize) -> Bool { extent.width <= available.width && extent.height <= available.height }

    /* The widest ring that fits: 12 clock positions, fewer in small windows, and a taller ring
     * when the window is narrow. */
    var ring = PencilRing()
    var aspect: CGFloat = 1.8
    search: for count in [innerSlotsMax, 10, 8, 6] {
      for candidate: CGFloat in [1.8, 1.4, 1.0, 0.7, 0.5, 0.35] {
        ring = solveRing(count, aspect: candidate, phase: 0, pill: pill, obstacles: [centerRect], start: 40)
        if fits(ring.extent(pill)) {
          aspect = candidate
          break search
        }
      }
    }
    let innerCapacity = ring.slots
    var innerRects = [centerRect]
    for i in 0..<ring.slots {
      innerRects.append(pillRect(ring.point(i, .zero), pill).insetBy(dx: -pillGap / 2, dy: -pillGap / 2))
    }
    /* A second ring sits between the inner pills, half a step around. */
    let second = solveRing(outerSlots, aspect: aspect, phase: .pi / CGFloat(outerSlots), pill: pill,
                           obstacles: innerRects, start: ring.radiusY + pillHeight)
    let capacity = innerCapacity + (fits(second.extent(pill)) ? outerSlots : 0)
    let paged = items.count > capacity
    perPage = paged ? capacity - 1 : capacity

    let start = page * perPage
    let count = min(perPage, items.count - min(start, items.count))
    let shown = count + (paged ? 1 : 0)
    if shown <= innerCapacity {
      /* Small menus stay compact: no more inner slots than needed, but at least eight. */
      let needed = min(innerCapacity, max(shown, 8))
      if needed != ring.slots {
        let compact = solveRing(needed, aspect: aspect, phase: 0, pill: pill, obstacles: [centerRect], start: 40)
        if fits(compact.extent(pill)) { ring = compact }
      }
    }
    inner = ring
    outer = shown > ring.slots ? second : PencilRing()
    let extent = outer.slots > 0 ? outer.extent(pill) : inner.extent(pill)
    menuCenter = CGPoint(x: max(available.minX + extent.width / 2, min(anchor.x, available.maxX - extent.width / 2)),
                     y: max(available.minY + extent.height / 2, min(anchor.y, available.maxY - extent.height / 2)))

    var pills: [PencilPill] = []
    slots = []
    tags = []
    for i in 0..<shown {
      let more = paged && i == count
      let item = more ? [:] : items[start + i]
      /* The inner ring fills clockwise from the top, then the outer ring. More keeps the inner
       * ring's last slot, just left of the top, so it doesn't move between pages. */
      let outerRing = !more && i >= inner.slots - (paged ? 1 : 0)
      let slot = more ? inner.slots - 1 : outerRing ? i - (inner.slots - (paged ? 1 : 0)) : i
      slots.append((outerRing ? 1000 : 0) + slot)
      tags.append(more ? -1 : start + i)
      let identifier = more ? "blender.pencil.more" :
        (item["back"] as? Bool ?? false) ? "blender.pencil.back" : (item["operator"] as? String ?? "")
      pills.append(PencilPill(
        id: i, title: more ? "More" : (item["title"] as? String ?? ""),
        icon: more ? UIImage(systemName: "ellipsis") : item["image"] as? UIImage,
        center: (outerRing ? outer : inner).point(slot, menuCenter), size: pill,
        enabled: !((item["disabled"] as? NSNumber)?.boolValue ?? false),
        checked: (item["checked"] as? NSNumber)?.boolValue ?? false,
        destructive: (item["destructive"] as? NSNumber)?.boolValue ?? false, identifier: identifier))
    }
    model.center = menuCenter
    model.page += 1
    model.pills = pills
    model.visible = true
    updateLabel()
  }

  private func open(at point: CGPoint) {
    if menuVisible || !active { return }
    anchor = point
    layoutSize = bounds.size
    page = 0
    menuVisible = true
    history.removeAll()
    loadItems(route: ["root": "context", "path": []], title: nil)
  }

  private func loadItems(route: NSDictionary, title: String?) {
    self.title = title
    requestGeneration += 1
    let generation = requestGeneration
    loading = true
    select(nil)
    items = []
    page = 0
    buildPage()
    delegate?.pencilMenu(self, requestItemsForRoute: route as! [AnyHashable: Any],
                         at: anchor) { [weak self] received in
      guard let self, self.active, self.menuVisible, generation == self.requestGeneration else { return }
      var actions: [[String: Any]] = []
      for item in (received ?? []).compactMap({ $0 as? [String: Any] }) {
        if let heading = item["heading"] as? String {
          if (self.title ?? "").isEmpty { self.title = heading }
        }
        else {
          actions.append(item)
        }
      }
      var next: [[String: Any]] = []
      if !self.history.isEmpty {
        next.append(["title": "Back", "back": true, "image": UIImage(systemName: "chevron.backward") as Any])
      }
      self.items = next + actions
      self.loading = false
      self.buildPage()
    }
  }

  private func updateSelection(at point: CGPoint) {
    guard menuVisible else { return }
    /* Neither opening at the tip nor returning to the center chooses an action. */
    if hypot(point.x - anchor.x, point.y - anchor.y) < 18 || hypot(point.x - menuCenter.x, point.y - menuCenter.y) < 40 {
      select(nil)
      return
    }
    var hit = model.pills.firstIndex { pillRect($0.center, $0.size).insetBy(dx: -8, dy: -8).contains(point) }
    if hit == nil && inner.slots > 0 {
      /* Between pills, the direction picks the slot and the distance picks the ring. */
      let dx = (point.x - menuCenter.x) / inner.radiusX
      let dy = (point.y - menuCenter.y) / inner.radiusY
      let distance = hypot(dx, dy)
      let outerDistance = outer.slots > 0 ? outer.radiusY / inner.radiusY : 0
      let outerRing = outer.slots > 0 && distance > (1 + outerDistance) / 2
      let limit = outer.slots > 0 ? outerDistance + 0.35 : 1.45
      if distance >= 0.6 && distance <= limit {
        let ring = outerRing ? outer : inner
        let step = 2 * Double.pi / Double(ring.slots)
        let angle = atan2(Double(dy), Double(dx)) + .pi / 2 - Double(ring.phase)
        let slot = (Int((angle / step).rounded()) + ring.slots) % ring.slots
        hit = slots.firstIndex(of: (outerRing ? 1000 : 0) + slot)
      }
    }
    select(hit)
  }

  private func dismiss() {
    requestGeneration += 1
    loading = false
    menuVisible = false
    active = false
    select(nil)
    model.visible = false
    model.pills = []
  }

  private func activate(_ index: Int) {
    guard active, menuVisible, !loading, model.pills[index].enabled else { return }
    let tag = tags[index]
    if tag < 0 {
      page = (page + 1) % pageCount
      buildPage()
      return
    }
    let item = items[tag]
    if item["back"] as? Bool ?? false, let parent = history.popLast() {
      select(nil)
      items = parent.items
      page = parent.page
      title = parent.title
      buildPage()
      return
    }
    if (item["submenu"] as? NSNumber)?.boolValue ?? false {
      history.append((items, page, title ?? ""))
      loadItems(route: (item["route"] as? NSDictionary) ?? [:], title: item["title"] as? String)
      return
    }
    dismiss()
    delegate?.pencilMenu(self, didChooseItem: item, at: anchor)
  }

  @objc(toggleWithPencilLocation:)
  public func toggle(withPencilLocation location: NSValue?) {
    if active {
      cancel()
      return
    }
    active = true
    feedback.prepare()
    if let location {
      lastPencilPoint = location.cgPointValue
      hasPencilLocation = true
    }
    if hasPencilLocation { open(at: lastPencilPoint) }
  }

  @objc(handlePencilTouch:)
  public func handlePencilTouch(_ touch: UITouch) -> Bool {
    lastPencilPoint = touch.preciseLocation(in: self)
    hasPencilLocation = true
    if touch.phase == .began && touch !== capturedTouch {
      /* UIKit may retire a cancelled touch without another event when backgrounded. */
      capturedTouch = nil
    }
    if !active && touch !== capturedTouch { return false }
    if capturedTouch == nil && active && touch.phase == .began {
      capturedTouch = touch
      openedOnTouch = !menuVisible
    }
    if touch !== capturedTouch {
      /* A tool stroke cancelled by the secondary interaction must lift before it can select a
       * menu item. Only a fresh contact starts menu selection. */
      return active
    }
    if active {
      let point = touch.preciseLocation(in: self)
      open(at: point)
      updateSelection(at: point)
      if touch.phase == .ended {
        if let selected = model.selected { activate(selected) }
        else if !loading && !openedOnTouch { dismiss() }
      }
      else if touch.phase == .cancelled {
        dismiss()
      }
    }
    if touch.phase == .ended || touch.phase == .cancelled { capturedTouch = nil }
    return true
  }

  @objc(handlePencilHover:)
  public func handlePencilHover(_ hover: UIHoverGestureRecognizer) -> Bool {
    if hover.state == .began || hover.state == .changed {
      lastPencilPoint = hover.location(in: self)
      hasPencilLocation = true
      if active && capturedTouch == nil {
        open(at: lastPencilPoint)
        updateSelection(at: lastPencilPoint)
      }
    }
    else if capturedTouch == nil {
      select(nil)
    }
    return active || capturedTouch != nil
  }

  @objc public func cancel() {
    /* A captured Pencil remains swallowed until lift after dismissal. */
    dismiss()
  }
}

/** \} */

#endif
