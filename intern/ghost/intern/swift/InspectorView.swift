/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The Properties panel: Blender's Properties tabs along the top, then each panel as a section
 * that collapses from its header, with the panel's header switches beside its title.
 */

import SwiftUI
import UIKit

struct PanelHeader<Trailing: View>: View {
  let title: String
  let close: () -> Void
  @ViewBuilder var trailing: Trailing

  var body: some View {
    HStack(spacing: 4) {
      Text(title)
        .font(.headline)
        .lineLimit(1)
        .frame(maxWidth: .infinity, alignment: .leading)
      trailing
      Button(action: close) {
        Image(systemName: "xmark")
          .font(.body.weight(.medium))
          .foregroundStyle(.secondary)
          .frame(width: 44, height: 44)
          .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .accessibilityLabel("Close \(title)")
      .keyboardShortcut(.cancelAction)
    }
    .padding(.leading, 16)
    .padding(.trailing, 5)
    .padding(.top, 5)
  }
}

struct InspectorPanel: View {
  @ObservedObject var store: InspectorStore
  let close: () -> Void

  var body: some View {
    VStack(spacing: 0) {
      PanelHeader(title: store.hasInspector ? store.title : "Properties", close: close) { EmptyView() }
      PropertiesTabs(store: store)
        .padding(.horizontal, 8)
        .frame(height: 44)
      InspectorForm(store: store)
        .padding(.top, 4)
    }
    .tint(Color.accentColor)
  }
}

struct PropertiesTabs: View {
  @ObservedObject var store: InspectorStore

  var body: some View {
    ScrollViewReader { reader in
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 2) {
          ForEach(store.tabs) { tab in
            let selected = tab.id == store.selectedTab
            Button {
              store.selectTab(tab.id)
              Haptics.selection()
            } label: {
              Group {
                /* Selected tabs draw a white glyph on the blue highlight, as iOS does. */
                if let image = BlenderIcon.image(tab.icon, themed: !selected) {
                  Image(uiImage: image).resizable().scaledToFit().frame(width: 22, height: 22)
                }
              }
              .foregroundStyle(selected ? Color.white : .secondary)
              .frame(width: 42, height: 42)
              .background(selected ? Color.accentColor : .clear,
                          in: RoundedRectangle(cornerRadius: 8, style: .continuous))
              .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .id(tab.id)
            .accessibilityLabel(tab.title)
            .accessibilityAddTraits(selected ? .isSelected : [])
            .accessibilityIdentifier(tab.id)
            .help(tab.title)
          }
        }
      }
      .scrollEdgeEffectStyle(.soft, for: .horizontal)
      .onChange(of: store.selectedTab) { _, tab in
        if let tab { withMotion { reader.scrollTo(tab, anchor: .center) } }
      }
    }
  }
}

struct InspectorForm: View {
  @ObservedObject var store: InspectorStore

  var body: some View {
    ScrollView {
      /* Not lazy: lazy stacks stay empty when they first appear in a hidden, unsized panel. */
      VStack(alignment: .leading, spacing: 10) {
        ForEach(store.sections) { section in
          SectionView(store: store, section: section)
        }
        if !store.status.isEmpty {
          Text(store.status)
            .font(.footnote)
            .foregroundStyle(.red)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 8)
            .transition(.opacity)
        }
      }
      .padding(.horizontal, 16)
      .padding(.bottom, 16)
    }
    .scrollDismissesKeyboard(.interactively)
    .overlay {
      if store.loaded && !store.hasInspector && store.mode == .properties {
        ContentUnavailableView("No Active Object", systemImage: "cube.transparent")
      }
      else if !store.loaded && store.mode == .shading {
        ProgressView()
      }
    }
  }
}

struct SectionView: View {
  @ObservedObject var store: InspectorStore
  let section: InspectorSection

  var body: some View {
    let expanded = section.headerHidden || store.isExpanded(section.id)
    VStack(alignment: .leading, spacing: 4) {
      if !section.headerHidden {
        header(expanded: expanded)
      }
      let rows = store.visibleRows(section)
      if expanded && !rows.isEmpty {
        if bare(rows) {
          rowView(rows[0])
        }
        else {
          VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(rows.enumerated()), id: \.element.id) { position, row in
              if position > 0 { Divider() }
              rowView(row)
                .padding(.vertical, 2)
            }
          }
          .padding(.horizontal, 12)
          .padding(.vertical, 2)
          .background(PanelStyle.card, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
          .transition(.opacity.combined(with: .move(edge: .top)))
        }
      }
    }
    .padding(.top, section.headerHidden ? 8 : 0)
  }

  /** The shading mode picker is already a pill; a card around it nests the shape. */
  private func bare(_ rows: [InspectorRow]) -> Bool {
    guard store.mode == .shading, rows.count == 1, case .property(let property) = rows[0].kind else {
      return false
    }
    return property.string("property") == "type" && property.string("type") == "ENUM"
  }

  private func header(expanded: Bool) -> some View {
    HStack(spacing: 8) {
      Button {
        guard !section.switchOnly else { return }
        store.toggleSection(section.id)
      } label: {
        HStack(spacing: 8) {
          Image(systemName: "chevron.right")
            .font(.system(size: 11, weight: .semibold))
            .rotationEffect(.degrees(expanded ? 90 : 0))
            .frame(width: 14, height: 14)
            .opacity(section.switchOnly ? 0 : 1)
          if let icon = BlenderIcon.image(section.icon) {
            Image(uiImage: icon).resizable().scaledToFit().frame(width: 18, height: 18)
          }
          Text(section.title)
            .lineLimit(1)
            .foregroundStyle(section.active ? Color.accentColor : .primary)
          Spacer(minLength: 0)
        }
        .frame(minHeight: 44)
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .disabled(section.switchOnly)
      .accessibilityValue((expanded ? "Expanded" : "Collapsed") + (section.active ? ", Active" : ""))
      .accessibilityIdentifier("blender.section." + section.id)
      HeaderControls(store: store, controls: section.headerControls)
    }
    .padding(.leading, 12)
    .padding(.trailing, 4)
  }

  @ViewBuilder
  private func rowView(_ row: InspectorRow) -> some View {
    switch row.kind {
      case .heading(let title, let subpanel, let disclosure, let reported, let depth, let controls):
        let open = store.subpanelOpen(subpanel, reported: reported)
        HStack(spacing: 8) {
          if disclosure {
            Button {
              store.toggleSubpanel(subpanel, currentlyOpen: open)
            } label: {
              HStack(spacing: 8) {
                Image(systemName: "chevron.right")
                  .font(.system(size: 10, weight: .semibold))
                  .rotationEffect(.degrees(open ? 90 : 0))
                  .frame(width: 12)
                Text(title)
                  .font(.subheadline)
                  .frame(maxWidth: .infinity, alignment: .leading)
              }
              .foregroundStyle(.secondary)
              .frame(minHeight: 36)
              .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityValue(open ? "Expanded" : "Collapsed")
            .accessibilityIdentifier("blender.subpanel." + subpanel)
          }
          else {
            Text(title)
              .font(.subheadline)
              .foregroundStyle(.secondary)
              .frame(maxWidth: .infinity, minHeight: 36, alignment: .leading)
          }
          HeaderControls(store: store, controls: controls)
        }
        .padding(.leading, CGFloat(12 * depth))
      case .property(let property):
        PropertyRow(store: store, property: property)
      case .vector(let title, let items):
        VectorRow(store: store, title: title, items: items)
      case .toggles(let caption, let items):
        ToggleGroupRow(store: store, caption: caption, items: items)
      case .composite(let property):
        CompositeRow(store: store, property: property)
          .padding(.vertical, 4)
    }
  }
}

/** A panel header's own switches and operators, such as a modifier's visibility and Remove. */
struct HeaderControls: View {
  @ObservedObject var store: InspectorStore
  let controls: [JSON]

  var body: some View {
    ForEach(Array(controls.enumerated()), id: \.offset) { _, control in
      Group {
        switch control.string("type") {
          case "BOOLEAN":
            PropertyToggle(store: store, property: control)
          case "OPERATOR" where control.bool("checkbox"):
            OperatorToggle(store: store, property: control)
          case "OPERATOR":
            HeaderOperatorButton(store: store, property: control)
          default:
            EmptyView()
        }
      }
      .propertyActions(store, control)
      .fixedSize()
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Popovers
 * \{ */

/** Viewport shading and the full tool settings: the same form in a popover. */
struct PopoverForm: View {
  @ObservedObject var store: InspectorStore
  let title: String
  let done: () -> Void

  var body: some View {
    NavigationStack {
      InspectorForm(store: store)
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
          ToolbarItem(placement: .confirmationAction) {
            Button("Done", action: done)
          }
        }
    }
    .tint(Color.accentColor)
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tool settings strip
 *
 * The active tool's settings in the top bar: compact columns, a caption over each control. The
 * strip scrolls sideways when the bar is narrow, and a trailing button opens them all.
 * \{ */

struct ToolStrip: View {
  @ObservedObject var store: InspectorStore

  var body: some View {
    HStack(spacing: 6) {
      ForEach(store.sections) { section in
        ForEach(section.rows) { row in
          switch row.kind {
            case .property(let property):
              StripColumn(store: store, property: property)
            case .toggles(let caption, let items):
              ToggleGroupRow(store: store, caption: caption, items: items, compact: true)
            default:
              EmptyView()
          }
        }
      }
    }
    .frame(height: 40)
    .tint(Color.accentColor)
    .accessibilityIdentifier("blender.tool-settings.strip")
  }
}

struct StripColumn: View {
  @ObservedObject var store: InspectorStore
  let property: JSON

  private var type: String { property.string("type") ?? "" }

  private var width: CGFloat {
    if type == "COLOR" { return 52 }
    if type == "BOOLEAN" || (type == "OPERATOR" && property.bool("checkbox")) { return 72 }
    if type == "ENUM" {
      let options = property.array("options").count
      return property.bool("expand") && (2...6).contains(options) ? 72 : 108
    }
    if type == "OPERATOR" { return 96 }
    return 104
  }

  var body: some View {
    VStack(spacing: 0) {
      Text(property.string("toolbar_label") ?? property.string("label") ?? "")
        .font(.caption2)
        .foregroundStyle(.secondary)
        .lineLimit(1)
        .minimumScaleFactor(0.8)
      control
        .frame(height: 26)
    }
    .frame(width: width, height: 40)
    .propertyActions(store, property)
  }

  @ViewBuilder
  private var control: some View {
    switch type {
      case "BOOLEAN":
        PropertyToggle(store: store, property: property)
          .scaleEffect(0.7)
          .frame(width: 40, height: 24)
      case "OPERATOR" where property.bool("checkbox"):
        OperatorToggle(store: store, property: property)
          .scaleEffect(0.7)
          .frame(width: 40, height: 24)
      case "OPERATOR":
        OperatorButton(store: store, property: property, compact: true)
      case "MENU":
        UIMenuButton(title: property.string("label"), image: BlenderIcon.image(property.string("icon")),
                     style: .borderless, enabled: property.bool("editable")) {
          store.menuProvider?(property as NSDictionary)
        }
      case "ENUM":
        EnumControl(store: store, property: property, compact: true)
      case "COLOR":
        ColorSwatch(store: store, property: property)
          .scaleEffect(0.8)
      case "INT", "FLOAT":
        if property.has("slider") {
          PropertySlider(store: store, property: property)
            .controlSize(.mini)
            .frame(width: width - 8)
        }
        else {
          NumberField(store: store, property: property, compact: true)
            .frame(width: width - 8)
        }
      default:
        EmptyView()
    }
  }
}

/** \} */
