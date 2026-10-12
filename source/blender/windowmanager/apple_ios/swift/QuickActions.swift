/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The row along the bottom of the document window: quick actions for what is selected, Blender's
 * full context menu, and Adjust Last Operation.
 *
 * Folded, the actions are an "Actions" pill. Open, they are a row of glass buttons centered on
 * the viewport, ending with the context menu. The last operation is a pill beside them, opening
 * into a strip of its settings. Only one of the two is open at a time. Glass morphs between the
 * pills and what they open into.
 */

import SwiftUI
import UIKit

/** Sizes shared by the window's controls. */
enum ShellMetrics {
  /** The tool rail's distance from the leading edge. */
  static let railLeading: CGFloat = 14
  /** The navigation gizmo's corner, which the row stays clear of until Blender places it. */
  static let gizmoReserve: CGFloat = 124
  static let rowHeight: CGFloat = 56
  static let buttonHeight: CGFloat = 44
  static let actionsPillWidth: CGFloat = 88
  /** From the bottom edge of the safe area. */
  static let rowBottom: CGFloat = 6
  /** Wide enough for the strip to open in the row itself, beside the actions. */
  static let wideRow: CGFloat = 700
}

/* -------------------------------------------------------------------- */
/** \name Quick actions
 * \{ */

/** A quick action from Blender's toolbar snapshot. */
struct QuickAction: Identifiable, Equatable {
  let id: Int
  let title: String
  let operatorName: String
  let property: String
  let value: String
  /** An X, Y or Z key while a transform runs, to constrain it to that axis. */
  let key: String
  /** Chosen and run by `bl_ui.native_actions`, by this identifier. */
  let action: String
  let icon: String
  let selected: Bool

  var axis: Bool { ["X", "Y", "Z"].contains(key) }

  static func list(_ snapshot: [JSON]) -> [QuickAction] {
    snapshot.enumerated().compactMap { index, item in
      guard let title = item.string("title"), !title.isEmpty, item.string("symbol") != nil else { return nil }
      let action = QuickAction(id: index, title: title, operatorName: item.string("operator") ?? "",
                               property: item.string("property") ?? "", value: item.string("value") ?? "",
                               key: item.string("key") ?? "", action: item.string("action") ?? "",
                               icon: item.string("icon") ?? "", selected: item.bool("selected"))
      return action.operatorName.isEmpty && action.key.isEmpty ? nil : action
    }
  }

  var image: UIImage? {
    if !key.isEmpty { return nil }
    if !action.isEmpty && !icon.isEmpty { return BlenderIcon.image(icon) }
    return wm_apple_ios_action_icon(operatorName, property, value)
  }

  @MainActor
  func run(in model: ShellModel) {
    if !action.isEmpty {
      Engine.setting("native_action", action)
    }
    else if axis {
      Engine.setting("constraint_axis", key)
    }
    else if !key.isEmpty {
      wm_apple_ios_window_send_key(model.window, key)
    }
    else {
      WM_apple_ios_ui_operator_enum_enqueue(operatorName, property, value)
    }
  }

  var axisColor: Color {
    switch key {
      case "X": .red
      case "Y": .green
      default: .blue
    }
  }
}

struct QuickActionButton: View {
  let action: QuickAction
  let model: ShellModel
  /** On glass of its own, such as an ornament's: plain unless selected. */
  var onGlass = false

  var body: some View {
    Button {
      action.run(in: model)
    } label: {
      label
        .frame(height: ShellMetrics.buttonHeight)
        .frame(minWidth: action.axis ? 60 : nil)
        .contentShape(Capsule())
    }
    .buttonStyle(.plain)
    .panelGlass(glass, interactive: true, in: Capsule())
    .accessibilityLabel(action.axis ? "\(action.key) axis constraint" : action.title)
    .accessibilityValue(action.axis ? (action.selected ? "On" : "Off") : "")
    .accessibilityAddTraits(action.selected ? .isSelected : [])
    .accessibilityIdentifier(action.action.isEmpty ? action.operatorName : "blender.action.\(action.action)")
  }

  private var glass: PanelGlass {
    if action.selected { return .tinted(action.axis ? action.axisColor : .blue) }
    return onGlass ? .none : .regular
  }

  @ViewBuilder
  private var label: some View {
    if action.axis {
      /* Black lettering stays readable on the bright axis-colored glass. */
      HStack(spacing: action.selected ? 4 : 0) {
        Text(action.key)
          .font(.system(size: 17, weight: .semibold))
        if action.selected {
          Image(systemName: "checkmark")
            .font(.system(size: 11, weight: .bold))
        }
      }
      .foregroundStyle(action.selected ? Color.black : action.axisColor)
      .padding(.horizontal, 8)
    }
    else {
      HStack(spacing: 5) {
        if let image = action.image {
          Image(uiImage: image)
            .renderingMode(image.renderingMode == .alwaysOriginal ? .original : .template)
            .font(.system(size: 20))
        }
        Text(action.title)
          .font(.system(size: 17))
          .lineLimit(1)
      }
      .foregroundStyle(action.selected ? Color.white : Color.primary)
      .padding(.horizontal, 10)
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The row
 * \{ */

struct QuickActionsRow: View {
  let model: ShellModel
  @StateObject private var redo = RedoStore()
  @State private var actionsOpen = false
  /** The operation the user opened the strip for; another operation arrives folded. */
  @State private var redoOpenID: String?
  @State private var redoError: String?
  @State private var shownError: String?
  /** The strip hugs its settings, measured as they lay out. */
  @State private var redoContentWidth: CGFloat = 0
  @State private var actionsWidth: CGFloat = 0
  @State private var actionsScrollWidth: CGFloat = 0
  @AppStorage(SettingsModel.showQuickActionsKey) private var showQuickActions = true
  @Namespace private var glass

  private var actions: [QuickAction] {
    showQuickActions ? QuickAction.list(model.actions) : []
  }

  /** Blender's full context menu follows the actions, but not while a transform takes keys. */
  private var contextMenuAvailable: Bool {
    !actions.isEmpty && !actions.contains { !$0.key.isEmpty }
  }

  private var redoOpen: Bool {
    redoOpenID != nil && redoOpenID == redo.operation && !redo.properties.isEmpty
  }

  /** The last operation's short name, while it has settings to adjust. */
  private var redoTitle: String? {
    guard let snapshot = redo.snapshot, !redo.properties.isEmpty else { return nil }
    return RedoStore.shortNames[snapshot.string("operator") ?? ""] ?? snapshot.string("title")
  }

  private var redoPillTitle: String? {
    redoOpen ? nil : redoTitle
  }

  private var animation: Animation? {
    UIAccessibility.isReduceMotionEnabled ? nil : .spring(response: 0.36, dampingFraction: 0.86)
  }

  var body: some View {
    GeometryReader { geometry in
      let width = geometry.size.width
      /* The lane between the tool rail and the navigation gizmo. */
      let laneLeading = ShellMetrics.railLeading + model.toolRailWidth + 12
      /* Up to the navigation gizmo's hotspot, which Blender places in fractions of the viewport. */
      let gizmo = model.navigationGizmo
      let hotspot = gizmo.count == 3 ? gizmo[0] * width - max(44, gizmo[2] * width + 10) :
        width - ShellMetrics.gizmoReserve
      let laneWidth = max(0, hotspot - 12 - laneLeading)
      let wide = width >= ShellMetrics.wideRow
      let pillWidth = redoTitle.map(redoPillWidth) ?? 0
      /* The pill follows the actions in the row, so the row is measured without it. */
      let rowWidth = actionsWidth + (redoPillTitle != nil ? (actionsWidth > 0 ? 8 : 0) + pillWidth : 0)
      /* Folded, the pills rest beside the tool rail; open, the row centers on the viewport,
       * staying in the lane. */
      let rowX = actionsOpen ?
        min(max(width / 2 - rowWidth / 2, laneLeading), laneLeading + laneWidth - rowWidth) : laneLeading
      VStack(spacing: 0) {
        Spacer(minLength: 0)
        /* Merging only glass that touches, so neighbouring buttons stay separate capsules. */
        PanelGlassGroup(spacing: 2) {
          ZStack(alignment: .leading) {
            actionsGroup(laneWidth: laneWidth)
              .fixedSize()
              .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { actionsWidth = $0 }
              .offset(x: rowX)
            if let title = redoTitle {
              redoMorph(title: title, frame: redoOpen ?
                          stripFrame(width: width, laneLeading: laneLeading, laneWidth: laneWidth, wide: wide) :
                          CGRect(x: rowX + rowWidth - pillWidth, y: 0, width: pillWidth,
                                 height: ShellMetrics.buttonHeight))
            }
          }
          .frame(width: width, height: ShellMetrics.rowHeight, alignment: .leading)
        }
      }
      .padding(.bottom, model.safeAreaInsets.bottom + ShellMetrics.rowBottom + model.bottomTray)
      .ignoresSafeArea(edges: .bottom)
    }
    .onChange(of: model.redo.map { $0 as NSDictionary }, initial: true) { _, snapshot in
      update(redo: snapshot as? JSON)
    }
    .alert("Unable to Adjust Operation", isPresented: Binding(
      get: { redoError != nil }, set: { if !$0 { redoError = nil } })) {
      Button("OK", role: .cancel) {}
    } message: {
      Text(redoError ?? "")
    }
  }

  private func update(redo snapshot: JSON?) {
    if snapshot?.string("id") != redo.operation, redoOpenID != snapshot?.string("id") {
      redoOpenID = nil
    }
    redo.update(snapshot)
    redo.close = {
      withAnimation(animation) { redoOpenID = nil }
    }
    redo.contentWidthChanged = {
      if abs(redo.contentWidth - redoContentWidth) > 0.5 { redoContentWidth = redo.contentWidth }
    }
    if let error = snapshot?.string("error"), !error.isEmpty {
      let identity = "\(snapshot?["id"] ?? ""):\(snapshot?["ack"] ?? ""):\(error)"
      if shownError != identity {
        shownError = identity
        redoError = error
      }
    }
  }

  /* -------------------------------------------------------------------- */
  /* Actions */

  @ViewBuilder
  private func actionsGroup(laneWidth: CGFloat) -> some View {
    if actionsOpen && !actions.isEmpty {
      HStack(spacing: 8) {
        toggle
        ScrollView(.horizontal, showsIndicators: false) {
          /* Glass of their own inside the scroll view, which clips it as they scroll. */
          PanelGlassGroup(spacing: 2) {
            HStack(spacing: 4) {
              ForEach(actions) { action in
                QuickActionButton(action: action, model: model)
              }
            }
          }
          .padding(6)
          .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { actionsScrollWidth = $0 }
        }
        .softHorizontalScrollEdge()
        .scrollBounceBehavior(.basedOnSize, axes: .horizontal)
        /* As wide as the actions, scrolling when the lane is narrower. */
        .frame(width: min(actionsScrollWidth, max(0, laneWidth - 44 - 16 - (contextMenuAvailable ? 52 : 0) -
                                              (redoPillTitle != nil ? 144 : 0))))
        if contextMenuAvailable {
          ContextMenuButton()
          .frame(width: ShellMetrics.buttonHeight, height: ShellMetrics.buttonHeight)
          .panelGlass(interactive: true, in: Circle())
          .panelGlassID("context", in: glass)
          .accessibilityIdentifier("blender.context-menu")
        }
      }
      .transition(.opacity.combined(with: .offset(x: -20)))
    }
    else if !actions.isEmpty {
      toggle
    }
  }

  private var toggle: some View {
    Button {
      withAnimation(animation) {
        actionsOpen.toggle()
        /* Only one may be open: the actions fold the strip. */
        if actionsOpen { redoOpenID = nil }
      }
    } label: {
      Group {
        if actionsOpen {
          Image(systemName: "xmark")
            .font(.system(size: 17, weight: .semibold))
            .frame(width: ShellMetrics.buttonHeight)
        }
        else {
          Text("Actions")
            .lineLimit(1)
            .minimumScaleFactor(0.7)
            .frame(width: ShellMetrics.actionsPillWidth - 20)
            .padding(.horizontal, 10)
        }
      }
      .foregroundStyle(.primary)
      .frame(height: ShellMetrics.buttonHeight)
      .contentShape(Capsule())
    }
    .buttonStyle(.plain)
    .panelGlass(interactive: true, in: Capsule())
    .panelGlassID("actions", in: glass)
    .accessibilityLabel(actionsOpen ? "Hide Quick Actions" : "Show Quick Actions")
    .accessibilityValue(actionsOpen ? "Expanded" : "Collapsed")
    .accessibilityIdentifier("blender.quick-actions.toggle")
    .help(actionsOpen ? "Hide Quick Actions" : "Show Quick Actions")
  }

  /* -------------------------------------------------------------------- */
  /* Adjust Last Operation */

  /** As wide as the title, from 72 to 136 points with its margins. */
  private func redoPillWidth(_ title: String) -> CGFloat {
    let measured = ceil((title as NSString).size(withAttributes: [.font: UIFont.systemFont(ofSize: 17)]).width)
    return min(136 - 24, max(72 - 24, measured)) + 24
  }

  /** The strip hugs its settings, after the close button and its margin. */
  private func stripWidth(maxWidth: CGFloat) -> CGFloat {
    min(max(0, maxWidth), min(1000, ceil(redoContentWidth) + 48))
  }

  /** Where the strip opens, in the row's space, centered on its y. */
  private func stripFrame(width: CGFloat, laneLeading: CGFloat, laneWidth: CGFloat, wide: Bool) -> CGRect {
    if wide {
      /* Opened in the row, after the Actions pill, centered on the viewport. */
      let start = laneLeading + actionsWidth + (actionsWidth > 0 ? 8 : 0)
      let strip = stripWidth(maxWidth: laneLeading + laneWidth - start)
      return CGRect(x: min(max(width / 2 - strip / 2, start), laneLeading + laneWidth - strip), y: 0,
                    width: strip, height: 60)
    }
    /* Above the row, centered on the window. */
    let strip = stripWidth(maxWidth: laneWidth)
    return CGRect(x: (width - strip) / 2, y: -(ShellMetrics.rowHeight / 2 + 8 + 30), width: strip, height: 60)
  }

  /**
   * The pill and the strip are one glass shape, so opening grows the pill's frame into the strip's
   * and closing folds it back, while their contents cross over inside it.
   */
  private func redoMorph(title: String, frame: CGRect) -> some View {
    let shape = RoundedRectangle(cornerRadius: redoOpen ? 20 : ShellMetrics.buttonHeight / 2,
                                 style: .continuous)
    return ZStack(alignment: .leading) {
      if redoOpen {
        RedoContent(store: redo)
          .frame(width: frame.width, height: frame.height)
          .accessibilityIdentifier("blender.redo.strip")
          .transition(.opacity)
      }
      else {
        Button {
          withAnimation(animation) {
            redoOpenID = redo.operation
            actionsOpen = false
          }
        } label: {
          Text(title)
            .lineLimit(1)
            .truncationMode(.tail)
            .foregroundStyle(.primary)
            .frame(width: frame.width - 24)
            .padding(.horizontal, 12)
            .frame(height: ShellMetrics.buttonHeight)
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Adjust \(redo.snapshot?.string("title") ?? "")")
        .accessibilityHint("Show last operation controls")
        .accessibilityIdentifier("blender.redo.collapsed")
        .transition(.opacity)
        .background {
          /* Measured ahead of opening, so the strip grows straight to its width. */
          RedoContent(store: redo)
            .frame(width: 1, height: 1)
            .hidden()
        }
      }
    }
    .frame(width: frame.width, height: frame.height, alignment: .leading)
    .clipShape(shape)
    /* Not interactive glass once open, which would take the touches of the controls on it. */
    .panelGlass(interactive: !redoOpen, in: shape)
    .offset(x: frame.minX, y: frame.minY)
    .transition(.scale(scale: 0.8, anchor: .leading).combined(with: .opacity))
  }
}

/** \} */
