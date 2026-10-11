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
  /** The tool rail's width and its distance from the leading edge. */
  static let railWidth: CGFloat = 64
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
    return .regular
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
  @State private var rowWidth: CGFloat = 0
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

  private var redoPillTitle: String? {
    guard let snapshot = redo.snapshot, !redo.properties.isEmpty, !redoOpen else { return nil }
    return SwiftRedoStrip.shortNames[snapshot.string("operator") ?? ""] ?? snapshot.string("title")
  }

  private var animation: Animation? {
    UIAccessibility.isReduceMotionEnabled ? nil : .spring(response: 0.36, dampingFraction: 0.86)
  }

  var body: some View {
    GeometryReader { geometry in
      let safe = geometry.safeAreaInsets
      let width = geometry.size.width
      /* The lane between the tool rail and the navigation gizmo. */
      let laneLeading = ShellMetrics.railLeading + ShellMetrics.railWidth + 12
      /* Up to the navigation gizmo's hotspot, which Blender places in window fractions. */
      let frame = geometry.frame(in: .global)
      let gizmo = model.navigationGizmo
      let hotspot = gizmo.count == 3 ?
        gizmo[0] * (frame.maxX + frame.minX) - max(44, gizmo[2] * (frame.maxX + frame.minX) + 10) - frame.minX :
        width - ShellMetrics.gizmoReserve
      let laneWidth = max(0, hotspot - 12 - laneLeading)
      let wide = width >= ShellMetrics.wideRow
      VStack(spacing: 8) {
        Spacer(minLength: 0)
        if redoOpen && !wide {
          redoStrip(width: stripWidth(maxWidth: laneWidth))
            .frame(maxWidth: .infinity)
        }
        /* Merging only glass that touches, so neighbouring buttons stay separate capsules. */
        PanelGlassGroup(spacing: 2) {
          ZStack(alignment: .leading) {
            HStack(spacing: 8) {
              actionsGroup(laneWidth: laneWidth)
              if let title = redoPillTitle {
                redoPill(title)
              }
            }
            .fixedSize()
            .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { rowWidth = $0 }
            /* Folded, the pills rest beside the tool rail; open, the row centers on the viewport,
             * staying in the lane. */
            .offset(x: actionsOpen ?
                      min(max(width / 2 - rowWidth / 2, laneLeading), laneLeading + laneWidth - rowWidth) :
                      laneLeading)
            if redoOpen && wide {
              /* Opened in the row, after the Actions pill, centered on the viewport. */
              let start = laneLeading + rowWidth + 8
              let stripWidth = self.stripWidth(maxWidth: laneLeading + laneWidth - start)
              redoStrip(width: stripWidth)
                .offset(x: min(max(width / 2 - stripWidth / 2, start), laneLeading + laneWidth - stripWidth))
            }
          }
          .frame(width: width, height: ShellMetrics.rowHeight, alignment: .leading)
        }
      }
      .padding(.bottom, max(safe.bottom, 0) + ShellMetrics.rowBottom)
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
          HStack(spacing: 4) {
            ForEach(actions) { action in
              QuickActionButton(action: action, model: model)
                .panelGlassID("action.\(action.id)", in: glass)
            }
          }
          .padding(6)
          .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { actionsWidth = $0 }
        }
        .softHorizontalScrollEdge()
        .scrollBounceBehavior(.basedOnSize, axes: .horizontal)
        /* As wide as the actions, scrolling when the lane is narrower. */
        .frame(width: min(actionsWidth, max(0, laneWidth - 44 - 16 - (contextMenuAvailable ? 52 : 0) -
                                              (redoPillTitle != nil ? 144 : 0))))
        if contextMenuAvailable {
          UIMenuButton(title: nil, image: UIImage(systemName: "ellipsis"), style: .plain,
                       accessibilityLabel: "Actions", tint: .label, deferred: false) {
            wm_apple_ios_context_menu()
          }
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

  private func redoPill(_ title: String) -> some View {
    Button {
      withAnimation(animation) {
        redoOpenID = redo.operation
        actionsOpen = false
      }
    } label: {
      /* As wide as the title, from 72 to 136 points with its margins. */
      let measured = ceil((title as NSString).size(withAttributes: [.font: UIFont.systemFont(ofSize: 17)]).width)
      Text(title)
        .lineLimit(1)
        .truncationMode(.tail)
        .foregroundStyle(.primary)
        .frame(width: min(136 - 24, max(72 - 24, measured)))
        .padding(.horizontal, 12)
        .frame(height: ShellMetrics.buttonHeight)
        .contentShape(Capsule())
    }
    .buttonStyle(.plain)
    .panelGlass(interactive: true, in: Capsule())
    .panelGlassID("redo", in: glass)
    .accessibilityLabel("Adjust \(redo.snapshot?.string("title") ?? "")")
    .accessibilityHint("Show last operation controls")
    .accessibilityIdentifier("blender.redo.collapsed")
  }

  /** The strip hugs its settings, after the close button and its margin. */
  private func stripWidth(maxWidth: CGFloat) -> CGFloat {
    min(max(0, maxWidth), min(1000, ceil(redoContentWidth) + 48))
  }

  private func redoStrip(width: CGFloat) -> some View {
    RedoContent(store: redo)
      .frame(width: width, height: 60)
      /* Not interactive glass, which would take the touches of the controls on it. */
      .panelGlass(in: RoundedRectangle(cornerRadius: 20, style: .continuous))
      .panelGlassID("redo", in: glass)
      .accessibilityIdentifier("blender.redo.strip")
  }
}

/** \} */
