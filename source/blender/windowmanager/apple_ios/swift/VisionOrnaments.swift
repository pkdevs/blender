/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Apple Vision Pro: the window's tools and actions as visionOS ornaments, floating beside the
 * window rather than over the viewport. The tools rest along the window's leading edge and, in
 * paint and sculpt modes, the brushes along its trailing edge; the quick actions and Blender's
 * context menu hang below it, as visionOS toolbars do. They show the same items as the iPad's
 * rails and actions row.
 */

#if os(visionOS)

import SwiftUI
import UIKit

/** A column of tools or brushes: tap to use one, touch and hold a tool for the others in its
 * group. */
struct RailOrnament: View {
  let items: [RailItem]
  let active: String
  let brush: Bool
  let title: String
  let choose: (RailItem) -> Void
  @State private var state = RailState()
  @Namespace private var selection

  var body: some View {
    let shown = state.active(active)
    ScrollView(.vertical) {
      VStack(spacing: 4) {
        ForEach(items) { item in
          if item.gapBefore {
            Divider().frame(width: 28).padding(.vertical, 4)
          }
          /* A grouped tool shows the active one of its group. */
          let current = item.group.first { $0.id == shown } ?? item
          Button {
            pick(current)
          } label: {
            RailItemView(item: current, selected: current.id == shown, emphasized: false, showsTitle: false,
                         brush: brush, namespace: selection)
          }
          .buttonStyle(.plain)
          .frame(width: 52)
          .help(current.title)
          .accessibilityLabel(current.title)
          .accessibilityAddTraits(current.id == shown ? .isSelected : [])
          .contextMenu {
            if item.grouped {
              ForEach(item.group) { tool in
                Button { pick(tool) } label: {
                  Label { Text(tool.title) } icon: { tool.image.map { Image(uiImage: $0) } }
                }
              }
            }
          }
        }
      }
      .padding(8)
    }
    .scrollIndicators(.hidden)
    .frame(width: 68)
    .frame(maxHeight: 640)
    .fixedSize(horizontal: false, vertical: true)
    .glassBackgroundEffect(in: RoundedRectangle(cornerRadius: 34, style: .continuous))
    .accessibilityElement(children: .contain)
    .accessibilityLabel(title)
  }

  private func pick(_ item: RailItem) {
    state.choose(item.id)
    choose(item)
  }
}

/** The quick actions for what is selected, then Blender's full context menu. */
struct ActionsOrnament: View {
  let model: ShellModel
  @AppStorage(SettingsModel.showQuickActionsKey) private var showQuickActions = true
  /** Shown as buttons, so the ornament stays narrower than the window; the rest are in the
   * context menu too. */
  private let inlineActions = 7

  var body: some View {
    let actions = showQuickActions ? QuickAction.list(model.actions) : []
    HStack(spacing: 4) {
      ForEach(actions.prefix(inlineActions)) { action in
        QuickActionButton(action: action, model: model, onGlass: true)
      }
      if !actions.contains(where: { !$0.key.isEmpty }) {
        UIMenuButton(title: nil, image: UIImage(systemName: "ellipsis"), style: .plain,
                     accessibilityLabel: "Actions", tint: .label, deferred: false) {
          wm_apple_ios_context_menu()
        }
        .frame(width: 44, height: 44)
        .accessibilityIdentifier("blender.context-menu")
      }
    }
    .padding(.horizontal, 12)
    .padding(.vertical, 8)
    .fixedSize()
    .glassBackgroundEffect(in: Capsule())
    .animation(.smooth(duration: 0.25), value: actions.count)
    .accessibilityElement(children: .contain)
    .accessibilityLabel("Actions")
  }
}

extension View {
  /** The tools beside the window, the brushes in paint and sculpt modes on its other side, and the
   * actions below it. */
  func visionOrnaments(model: ShellModel, shown: Bool) -> some View {
    let brushes = RailItem.brushes(model.brushes)
    return ornament(visibility: shown ? .visible : .hidden, attachmentAnchor: .scene(.leading),
                    contentAlignment: .trailing)
    {
      RailOrnament(items: RailItem.tools(model.tools, mode: model.mode), active: model.activeTool, brush: false,
                   title: "Tools") { WM_apple_ios_ui_tool_set_by_id($0.id) }
    }
    .ornament(visibility: shown && !brushes.isEmpty ? .visible : .hidden, attachmentAnchor: .scene(.trailing),
              contentAlignment: .leading)
    {
      RailOrnament(items: brushes, active: model.brushes?.string("active") ?? "", brush: true,
                   title: "Brushes") { WM_apple_ios_ui_brush_activate($0.id) }
    }
    .ornament(visibility: shown ? .visible : .hidden, attachmentAnchor: .scene(.bottom), contentAlignment: .top) {
      ActionsOrnament(model: model)
    }
  }
}

#endif
