/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The Properties editor, viewport shading and tool settings as SwiftUI state. Blender sends each
 * panel's buttons as JSON (see `interface_native_properties.cc`); this turns them into sections
 * and rows, and sends edits back through the engine inbox.
 *
 * Edits carry a sequence number. Snapshots acknowledging an older sequence are ignored, so a
 * control never jumps back to a stale value while Blender is still applying an edit.
 */

import SwiftUI
import UIKit

func propertyKey(_ property: JSON) -> String {
  if let key = property.string("key") { return key }
  let index = property["index"].map { "\($0)" } ?? ""
  return "\(property["owner"] ?? "").\(property["property"] ?? "").\(index)"
}

func propertyLabel(_ property: JSON) -> String {
  let label = property.string("label") ?? ""
  if let group = property.string("group"), !group.isEmpty { return "\(group) \(label)" }
  return label
}

/** Snapshots arrive as new dictionaries even when nothing changed; compare their contents. */
func jsonListEqual(_ a: [JSON], _ b: [JSON]) -> Bool {
  a.count == b.count && zip(a, b).allSatisfy { ($0 as NSDictionary).isEqual($1 as NSDictionary) }
}

func jsonDictEqual(_ a: JSON, _ b: JSON) -> Bool {
  (a as NSDictionary).isEqual(b as NSDictionary)
}

struct InspectorTab: Identifiable, Equatable {
  let id: String
  let title: String
  let icon: String
}

/** A row of a section. Rows are identified by meaning, so updates animate only what changed. */
struct InspectorRow: Identifiable, Equatable {
  enum Kind: Equatable {
    /** A sub-panel's heading: a disclosure when it can collapse, with its header controls. */
    case heading(title: String, subpanel: String, disclosure: Bool, open: Bool, depth: Int,
                 controls: [JSON])
    case property(JSON)
    /** A vector such as Location: one caption over its components. */
    case vector(title: String, items: [JSON])
    /** Toggles shown together, such as Mirror X Y Z. */
    case toggles(caption: String, items: [JSON])
    /** Data-block pickers, lists, previews and operator menus. */
    case composite(JSON)

    static func == (a: Kind, b: Kind) -> Bool {
      switch (a, b) {
        case let (.heading(title, subpanel, disclosure, open, depth, controls),
                  .heading(title2, subpanel2, disclosure2, open2, depth2, controls2)):
          return title == title2 && subpanel == subpanel2 && disclosure == disclosure2 && open == open2 &&
            depth == depth2 && jsonListEqual(controls, controls2)
        case let (.property(property), .property(property2)):
          return jsonDictEqual(property, property2)
        case let (.vector(title, items), .vector(title2, items2)):
          return title == title2 && jsonListEqual(items, items2)
        case let (.toggles(caption, items), .toggles(caption2, items2)):
          return caption == caption2 && jsonListEqual(items, items2)
        case let (.composite(property), .composite(property2)):
          return jsonDictEqual(property, property2)
        default:
          return false
      }
    }
  }

  let id: String
  var kind: Kind
}

struct InspectorSection: Identifiable, Equatable {
  let id: String
  let title: String
  let active: Bool
  let headerHidden: Bool
  let icon: String?
  let switchOnly: Bool
  var headerControls: [JSON]
  var rows: [InspectorRow]

  static func == (a: InspectorSection, b: InspectorSection) -> Bool {
    a.id == b.id && a.title == b.title && a.active == b.active && a.headerHidden == b.headerHidden &&
      a.icon == b.icon && a.switchOnly == b.switchOnly && a.rows == b.rows &&
      jsonListEqual(a.headerControls, b.headerControls)
  }
}

/** A value the user just set, observed per property so an edit redraws only its own control. */
@MainActor
@Observable
final class PendingValue {
  var value: Any?
}

/** Observed per property: a snapshot redraws only the views that read what it changed. */
@MainActor
@Observable
final class InspectorStore {
  enum Mode {
    /** The Properties editor, with its tabs. */
    case properties
    /** The viewport shading popover. */
    case shading
    /** Tool settings in the top bar, as one compact strip. */
    case toolStrip
    /** All tool settings in a popover. */
    case toolPopover
  }

  let mode: Mode
  private(set) var sections: [InspectorSection] = []
  private(set) var tabs: [InspectorTab] = []
  private(set) var selectedTab: String?
  private(set) var title = "Properties"
  private(set) var status = ""
  private(set) var objectID: String?
  private(set) var hasInspector = false
  private(set) var loaded = false
  /** Open sections per tab; nil until Blender first reports which are open. */
  private(set) var expandedPanels: [String: Set<String>] = [:]
  /** Per tab: sub-panel id to open state, once the user toggles it. */
  private(set) var subpanelStates: [String: [String: Bool]] = [:]
  /** Values the user just set, shown until Blender acknowledges them. Kept once made, so the
   * controls reading them keep observing the same value. */
  @ObservationIgnored private var pendingValues: [String: PendingValue] = [:]
  /** Bumped when controls must drop local editing state, e.g. on selection change. */
  private(set) var generation = 0

  @ObservationIgnored var menuProvider: ((NSDictionary) -> UIMenu?)?
  @ObservationIgnored var visible = true
  @ObservationIgnored private var snapshot: JSON?
  @ObservationIgnored private var shownTab: String?
  @ObservationIgnored private var inspector: JSON?
  @ObservationIgnored private(set) var sequence: UInt64 = 0
  private static var nextSequence: UInt64 = 0

  init(mode: Mode, tab: String? = nil) {
    self.mode = mode
    self.selectedTab = tab
  }

  /** Observers redraw on every assignment, even of the same value. */
  private func set<Value: Equatable>(_ keyPath: ReferenceWritableKeyPath<InspectorStore, Value>, _ value: Value) {
    if self[keyPath: keyPath] != value { self[keyPath: keyPath] = value }
  }

  private func pendingValue(_ key: String) -> PendingValue {
    if let existing = pendingValues[key] { return existing }
    let created = PendingValue()
    pendingValues[key] = created
    return created
  }

  private func setPending(_ property: JSON, _ value: Any) {
    let pending = pendingValue(propertyKey(property))
    if !jsonEqual(pending.value, value) { pending.value = value }
  }

  var currentTab: String { selectedTab ?? "OBJECT" }

  /* -------------------------------------------------------------------- */
  /** \name Sending edits
   * \{ */

  func send(_ request: JSON) {
    InspectorStore.nextSequence += 1
    sequence = InspectorStore.nextSequence
    var payload = request
    payload["sequence"] = NSNumber(value: sequence)
    Engine.setting(request.bool("native") ? "native_properties_edit" : "native_panel_edit",
                   json: payload)
  }

  func edit(_ property: JSON, value: Any) {
    var request = property
    request["action"] = "property"
    request["id"] = objectID ?? ""
    request["value"] = value
    setPending(property, value)
    send(request)
  }

  /** Continuous edits (sliders, scrubbing, color) share one interaction and one undo step. */
  func preview(_ property: JSON, value: Any, interaction: String, phase: String) {
    var request = property
    request["action"] = "property"
    request["id"] = objectID ?? ""
    request["value"] = value
    request["interaction"] = interaction
    request["phase"] = phase
    setPending(property, value)
    send(request)
  }

  /** Requests for pickers, lists, menus and operators, keyed by the button in its panel. */
  func request(_ property: JSON, extra: JSON) {
    var request: JSON = [
      "action": "property", "id": objectID ?? "", "native": 1,
      "key": property.string("key") ?? "", "panel": property.string("panel") ?? "",
      "tab": property.string("tab") ?? "",
    ]
    request.merge(extra) { $1 }
    send(request)
  }

  func runOperator(_ property: JSON) {
    var request = property
    request["id"] = objectID ?? ""
    send(request)
  }

  func animate(_ property: JSON, action: String) {
    var request = property
    request["action"] = "animation"
    request["animation_action"] = action
    request["id"] = objectID ?? ""
    send(request)
  }

  func showStatus(_ message: String) {
    guard message != status else { return }
    withMotion { status = message }
  }

  func value(_ property: JSON) -> Any? {
    pendingValue(propertyKey(property)).value ?? property["value"]
  }

  /** \} */

  /* -------------------------------------------------------------------- */
  /** \name Tabs and disclosure
   * \{ */

  func requestProperties() {
    guard mode == .properties || mode == .shading else { return }
    if selectedTab == "TOOL" { return }
    var state: JSON = ["tab": currentTab]
    if let expanded = expandedPanels[currentTab] { state["expanded"] = Array(expanded) }
    if let subpanels = subpanelStates[currentTab], !subpanels.isEmpty { state["subpanels"] = subpanels }
    Engine.setting(mode == .shading ? "native_shading_state" : "native_properties_state", json: state)
  }

  func selectTab(_ tab: String) {
    guard tab != selectedTab else { return }
    selectedTab = tab
    generation += 1
    requestProperties()
  }

  func isExpanded(_ section: String) -> Bool {
    guard let expanded = expandedPanels[currentTab] else { return true }
    return expanded.contains(section)
  }

  func toggleSection(_ section: String) {
    var expanded = expandedPanels[currentTab] ?? Set(sections.map(\.id))
    if expanded.contains(section) { expanded.remove(section) }
    else { expanded.insert(section) }
    withMotion { expandedPanels[currentTab] = expanded }
    requestProperties()
  }

  /** Physics turned on opens its settings below the switch. */
  func expandSection(_ section: String) {
    var expanded = expandedPanels[currentTab] ?? Set(sections.map(\.id))
    expanded.insert(section)
    expandedPanels[currentTab] = expanded
    requestProperties()
  }

  func toggleSubpanel(_ subpanel: String, currentlyOpen: Bool) {
    var states = subpanelStates[currentTab] ?? [:]
    states[subpanel] = !currentlyOpen
    withMotion { subpanelStates[currentTab] = states }
    requestProperties()
  }

  func subpanelOpen(_ subpanel: String, reported: Bool) -> Bool {
    subpanelStates[currentTab]?[subpanel] ?? reported
  }

  /** \} */

  /* -------------------------------------------------------------------- */
  /** \name Snapshots
   * \{ */

  /** Returns whether the snapshot was accepted. */
  @discardableResult
  func update(snapshot incoming: JSON) -> Bool {
    let ack = (incoming["ack"] as? NSNumber)?.uint64Value ?? 0
    if ack < sequence { return false }
    /* A hidden inspector applies the latest snapshot when it opens. */
    if mode == .properties && !visible { return false }
    let merged = keepingCollapsedContents(incoming)
    if let snapshot, (snapshot as NSDictionary).isEqual(merged as NSDictionary) { return false }
    snapshot = merged
    set(\.status, merged.string("error") ?? "")
    for pending in pendingValues.values where pending.value != nil { pending.value = nil }
    guard let raw = merged.dict("inspector") else {
      if merged["inspector"] is NSNull {
        set(\.hasInspector, false)
        set(\.sections, [])
        set(\.loaded, true)
      }
      return true
    }
    let inspector = mode == .properties ? physicsSections(raw) : raw
    if mode == .properties { updateTabs(inspector) }
    if let tab = inspector.string("tab"), mode == .properties, tab != selectedTab { return true }
    self.inspector = inspector
    set(\.hasInspector, true)
    set(\.loaded, true)
    set(\.title, inspector.string("name") ?? "Properties")
    let newObject = inspector.string("id")
    let replacing = newObject != objectID || sections.isEmpty || inspector.string("tab") != shownTab
    if newObject != objectID {
      objectID = newObject
      generation += 1
    }
    shownTab = inspector.string("tab")
    let built = buildSections(inspector)
    /* Unchanged sections keep their views; changed ones redraw only the rows that changed. */
    guard built != sections else { return true }
    /* Only rows coming and going within the same panel animate: a first fill, another object or
     * another tab replaces the rows at once, so no row waits on an insertion that may never be
     * shown. New values never animate, they just show. */
    if replacing || layout(built) == layout(sections) { sections = built }
    else { withMotion(.smooth(duration: 0.25)) { sections = built } }
    return true
  }

  /** Which sections and rows show, to tell rows coming and going from values changing. */
  private func layout(_ sections: [InspectorSection]) -> [[String]] {
    sections.map { [$0.id] + visibleRows($0).map(\.id) }
  }

  func clear() {
    set(\.hasInspector, false)
    set(\.sections, [])
  }

  private func updateTabs(_ inspector: JSON) {
    let next = inspector.array("tabs").map {
      InspectorTab(id: $0.string("id") ?? "", title: $0.string("title") ?? "", icon: $0.string("icon") ?? "")
    }
    if next != tabs { tabs = next }
    if selectedTab == nil || !tabs.contains(where: { $0.id == selectedTab }) {
      set(\.selectedTab, inspector.string("tab"))
    }
    if let tab = selectedTab, expandedPanels[tab] == nil, inspector.string("tab") == tab {
      expandedPanels[tab] = Set(inspector.array("sections").filter { $0.bool("open") }
        .compactMap { $0.string("id") })
    }
  }

  /** Collapsed sections arrive with only their header controls; keep the contents Blender sent
   * before, so collapsing animates the rows away rather than rebuilding them. */
  private func keepingCollapsedContents(_ snapshot: JSON) -> JSON {
    guard let inspector = snapshot.dict("inspector"),
          let previous = self.snapshot?.dict("inspector"),
          jsonEqual(inspector["id"], previous["id"])
    else { return snapshot }
    var sections: [JSON] = []
    for section in inspector.array("sections") {
      if section.has("open") && !section.bool("open"),
         let old = previous.array("sections").first(where: { jsonEqual($0["id"], section["id"]) })
      {
        var properties = section.array("properties")
        properties += old.array("properties").filter { !$0.bool("header") || $0.has("section") }
        var merged = section
        merged["properties"] = properties
        sections.append(merged)
      }
      else {
        sections.append(section)
      }
    }
    var merged = inspector
    merged["sections"] = sections
    var result = snapshot
    result["inspector"] = merged
    return result
  }

  /** Blender lists the physics as buttons above their settings. Natively each physics is one
   * section: its switch in the header and, while on, its settings below. */
  private func physicsSections(_ inspector: JSON) -> JSON {
    guard inspector.string("tab") == "PHYSICS",
          let add = inspector.array("sections").first(where: { $0.string("id") == "PHYSICS_PT_add" })
    else { return inspector }
    var byID: [String: JSON] = [:]
    for section in inspector.array("sections") {
      if let id = section.string("id"), id != "PHYSICS_PT_add" { byID[id] = section }
    }
    var sections: [JSON] = []
    var used = Set<String>()
    for toggle in add.array("properties") {
      let panel = toggle.string("physics_panel") ?? toggle.string("key") ?? ""
      var header = toggle
      header["header"] = 1
      if var settings = byID[panel], toggle.bool("value") {
        settings["properties"] = [header] + settings.array("properties")
        settings["icon"] = toggle.string("leading_icon") ?? ""
        sections.append(settings)
        used.insert(panel)
      }
      else {
        sections.append(["id": panel, "title": toggle.string("label") ?? "", "open": 0,
                         "icon": toggle.string("leading_icon") ?? "", "switch_only": 1,
                         "properties": [header]])
      }
    }
    for section in inspector.array("sections") {
      let id = section.string("id") ?? ""
      if id != "PHYSICS_PT_add" && !used.contains(id) { sections.append(section) }
    }
    var result = inspector
    result["sections"] = sections
    return result
  }

  /** \} */

  /* -------------------------------------------------------------------- */
  /** \name Rows
   * \{ */

  private func buildSections(_ inspector: JSON) -> [InspectorSection] {
    let strip = mode == .toolStrip
    var result: [InspectorSection] = []
    for section in inspector.array("sections") {
      let sectionID = section.string("id") ?? section.string("title") ?? ""
      if section.bool("header_hidden"), mode == .properties,
         expandedPanels[currentTab]?.contains(sectionID) != true
      {
        /* Blender draws these without a header, so they stay open. */
        expandedPanels[currentTab, default: []].insert(sectionID)
      }
      var model = InspectorSection(
        id: sectionID, title: section.string("title") ?? "", active: section.bool("active"),
        headerHidden: section.bool("header_hidden"), icon: section.string("icon"),
        switchOnly: section.bool("switch_only"), headerControls: [], rows: [])
      var innerSection: String?
      var headingIndex: Int?
      var vectorIndex: [String: Int] = [:]
      var toggleIndex: [String: Int] = [:]
      var seen: [String: Int] = [:]
      func uniqueID(_ base: String) -> String {
        let count = (seen[base] ?? 0) + 1
        seen[base] = count
        return count == 1 ? base : "\(base)#\(count)"
      }
      for property in section.array("properties") {
        let type = property.string("type") ?? ""
        let subpanel = property.string("subpanel") ?? property.string("section")
        if let name = property.string("section"), innerSection != subpanel, !strip {
          innerSection = subpanel
          let disclosure = type == "SUBPANEL"
          let open = disclosure ? subpanelOpen(subpanel ?? "", reported: property.bool("open")) : true
          model.rows.append(InspectorRow(
            id: uniqueID("heading:" + (subpanel ?? "")),
            kind: .heading(title: name, subpanel: subpanel ?? "", disclosure: disclosure, open: open,
                           depth: max(0, (property.int("depth") ?? 1) - 1), controls: [])))
          headingIndex = model.rows.count - 1
        }
        if type == "SUBPANEL" { continue }
        let key = propertyKey(property)
        if let group = property.string("toggle_group") {
          if let index = toggleIndex[group], case .toggles(let caption, var items) = model.rows[index].kind {
            items.append(property)
            model.rows[index].kind = .toggles(caption: caption, items: items)
          }
          else {
            let caption = (property.string("toggle_caption") ?? "").isEmpty ?
              (property.string("label") ?? "") : property.string("toggle_caption")!
            model.rows.append(InspectorRow(id: uniqueID("toggles:" + group),
                                           kind: .toggles(caption: caption, items: [property])))
            toggleIndex[group] = model.rows.count - 1
          }
          continue
        }
        if ["ID", "LIST", "PREVIEW", "OPERATOR_MENU"].contains(type) {
          if strip { continue }
          model.rows.append(InspectorRow(id: uniqueID("property:" + key), kind: .composite(property)))
          continue
        }
        if strip {
          model.rows.append(InspectorRow(id: uniqueID("property:" + key), kind: .property(property)))
          continue
        }
        let headerControl = property.bool("header") &&
          ((type == "OPERATOR" && !property.bool("checkbox")) || type == "BOOLEAN" ||
           (type == "OPERATOR" && property.bool("checkbox")))
        if headerControl {
          if property.has("section"), let index = headingIndex,
             case .heading(let title, let sub, let disclosure, let open, let depth, var controls) = model.rows[index].kind
          {
            controls.append(property)
            model.rows[index].kind = .heading(title: title, subpanel: sub, disclosure: disclosure,
                                              open: open, depth: depth, controls: controls)
          }
          else {
            model.headerControls.append(property)
          }
          continue
        }
        if let group = property.string("group"), !group.isEmpty {
          let identity = property.string("group_key") ??
            "\(property["owner"] ?? "").\(property["property"] ?? "")"
          let groupKey = "\(property.string("panel") ?? "").\(innerSection ?? "").\(identity)"
          if let index = vectorIndex[groupKey], case .vector(let title, var items) = model.rows[index].kind {
            items.append(property)
            model.rows[index].kind = .vector(title: title, items: items)
          }
          else {
            model.rows.append(InspectorRow(id: uniqueID("vector:" + groupKey),
                                           kind: .vector(title: group, items: [property])))
            vectorIndex[groupKey] = model.rows.count - 1
          }
          continue
        }
        model.rows.append(InspectorRow(id: uniqueID("property:" + key), kind: .property(property)))
      }
      result.append(model)
    }
    return result
  }

  /** Rows shown under a heading that is collapsed are hidden. */
  func visibleRows(_ section: InspectorSection) -> [InspectorRow] {
    var rows: [InspectorRow] = []
    var hiddenDepth: Int?
    for row in section.rows {
      if case .heading(_, let subpanel, let disclosure, let reported, let depth, _) = row.kind {
        if let hidden = hiddenDepth, depth > hidden { continue }
        hiddenDepth = nil
        rows.append(row)
        if disclosure && !subpanelOpen(subpanel, reported: reported) { hiddenDepth = depth }
        continue
      }
      if hiddenDepth != nil { continue }
      rows.append(row)
    }
    return rows
  }

  /** The latest version of a property, by key, for menus built before the latest snapshot. */
  func current(_ key: String) -> JSON? {
    for section in inspector?.array("sections") ?? [] {
      for property in section.array("properties") where propertyKey(property) == key {
        return property
      }
    }
    return nil
  }

  /** \} */
}
