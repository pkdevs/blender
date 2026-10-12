/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Blender's navigation gizmo in the document window, and its pie of views. Blender draws the
 * gizmo; a hotspot over it takes the touches. A tap picks the axis under it, as in Blender; a
 * hold, or a drag out of it, opens the pie, and releasing over a view chooses it. The Pencil's
 * touches on the hotspot go to Blender, which tells them apart by the hotspot's identifier.
 */

import SwiftUI
import UIKit

struct ViewCommand {
  let title: String
  let symbol: String
  /** `WMIOSUICommand`. */
  let command: Int32

  static let pie = [
    ViewCommand(title: "Frame All", symbol: "rectangle.expand.vertical", command: 24),
    ViewCommand(title: "Frame Selected", symbol: "viewfinder", command: 14),
    ViewCommand(title: "Camera", symbol: "camera", command: 26),
    ViewCommand(title: "Perspective", symbol: "cube.transparent", command: 27),
    ViewCommand(title: "Local View", symbol: "scope", command: 25),
    ViewCommand(title: "Front", symbol: "arrow.up.square", command: 28),
    ViewCommand(title: "Right", symbol: "arrow.right.square", command: 30),
    ViewCommand(title: "Top", symbol: "arrow.down.to.line.square", command: 32),
  ]

  var image: UIImage? {
    wm_apple_ios_command_icon(command, symbol) ?? UIImage(systemName: "circle")
  }

  func run() {
    WM_apple_ios_ui_command_enqueue(command)
  }
}

/** Where the gizmo and the pie's buttons are, in the viewport layer's points. */
struct ViewGizmoGeometry {
  var center: CGPoint
  /** Blender's gizmo, and the larger hotspot around it. */
  var radius: CGFloat
  var hotspotRadius: CGFloat
  var buttons: [CGRect]

  var hotspot: CGRect {
    CGRect(x: center.x - hotspotRadius, y: center.y - hotspotRadius,
           width: hotspotRadius * 2, height: hotspotRadius * 2)
  }

  init(gizmo: [Double], window: CGSize, safe: CGRect) {
    radius = gizmo.count == 3 ? gizmo[2] * window.width : 40
    center = gizmo.count == 3 ? CGPoint(x: gizmo[0] * window.width, y: gizmo[1] * window.height) :
      CGPoint(x: window.width - 50, y: window.height - 50)
    hotspotRadius = max(44, radius + 10)
    buttons = []
    guard !safe.isEmpty else { return }
    /* Beside the gizmo, rising from the bottom corner; in a small window, a grid. */
    let compact = safe.height < 540 || safe.width < 430
    let gap = safe.maxX - (center.x - hotspotRadius)
    let trailing: [CGFloat] = [gap + 16, gap + 26, gap + 42, gap + 42, gap + 16, 20, 20, 20]
    let bottom: [CGFloat] = [12, 72, 132, 192, 244, 304, 364, 424]
    let gridWidth = min(180, (safe.width - 36) / 2)
    let base = min(136, max(68, safe.height - 300))
    for (index, view) in ViewCommand.pie.enumerated() {
      let x = compact ? 12 + CGFloat(index % 2) * (gridWidth + 12) : trailing[index]
      let y = compact ? base + CGFloat(index / 2) * 52 : bottom[index]
      let width = compact ? gridWidth : min(180, ViewGizmoGeometry.buttonWidth(view))
      buttons.append(CGRect(x: safe.maxX - x - width, y: safe.maxY - y - 44, width: width, height: 44))
    }
  }

  /** A pie button's natural width: its icon and title between the glass's margins. */
  static func buttonWidth(_ view: ViewCommand) -> CGFloat {
    let title = (view.title as NSString).size(withAttributes: [.font: UIFont.systemFont(ofSize: 17)]).width
    return ceil(title) + 24 + 7 + 28
  }

  /** The button under a point, with some slack around each. */
  func button(at point: CGPoint) -> Int? {
    buttons.firstIndex { $0.insetBy(dx: -14, dy: -10).contains(point) }
  }
}

@MainActor
@Observable
final class ViewGizmoState {
  var pieVisible = false
  var hovered: Int?
  var pointerOver = false
  var pressed = false
  @ObservationIgnored var geometry: ViewGizmoGeometry?
#if !os(visionOS)
  @ObservationIgnored let selectionFeedback = UISelectionFeedbackGenerator()
  @ObservationIgnored let openFeedback = UIImpactFeedbackGenerator(style: .soft)
#endif

  func hover(at point: CGPoint) {
    let button = geometry?.button(at: point)
    guard button != hovered else { return }
    hovered = button
#if !os(visionOS)
    if button != nil { selectionFeedback.selectionChanged() }
#endif
  }

  /** Choose the view under the point, if any, and close the pie. */
  func release(at point: CGPoint?) {
    if let point, let button = geometry?.button(at: point) {
      ViewCommand.pie[button].run()
    }
    pieVisible = false
    hovered = nil
  }
}

/* -------------------------------------------------------------------- */
/** \name Hotspot
 * \{ */

/** A UIKit view, so the Pencil's touches on it reach Blender, which knows it by its identifier. */
final class ViewGizmoHotspot: UIControl {
  override init(frame: CGRect) {
    super.init(frame: frame)
    accessibilityIdentifier = "blender.viewport.navigation"
    accessibilityLabel = "View navigation"
    backgroundColor = .clear
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }
}

struct ViewGizmoHotspotView: UIViewRepresentable {
  let model: ShellModel
  let state: ViewGizmoState

  @MainActor
  final class Coordinator: NSObject, UIGestureRecognizerDelegate {
    var model: ShellModel
    var state: ViewGizmoState

    init(model: ShellModel, state: ViewGizmoState) {
      self.model = model
      self.state = state
    }

    /** In the layer's points, which the pie's buttons are in: the layer sits beside the
     * window's panels, not at its corner. */
    private func location(_ gesture: UIGestureRecognizer) -> CGPoint {
      let point = gesture.location(in: gesture.view)
      let origin = state.geometry?.hotspot.origin ?? .zero
      return CGPoint(x: point.x + origin.x, y: point.y + origin.y)
    }

    @objc func pressBegan() {
      state.pressed = true
#if !os(visionOS)
      state.openFeedback.prepare()
#endif
    }

    @objc func pressEnded() {
      state.pressed = false
    }

    @objc func hover(_ gesture: UIHoverGestureRecognizer) {
      state.pointerOver = gesture.state == .began || gesture.state == .changed
    }

    @objc func hold(_ gesture: UILongPressGestureRecognizer) {
      switch gesture.state {
        case .began:
          /* Consume the pending Pencil tap before it can become a Blender click. */
          wm_apple_ios_window_cancel_pencil_stroke(model.window)
          state.pieVisible = true
#if !os(visionOS)
          state.openFeedback.impactOccurred()
          state.selectionFeedback.prepare()
#endif
        case .changed: state.hover(at: location(gesture))
        case .ended: state.release(at: location(gesture))
        case .cancelled, .failed: state.release(at: nil)
        default: break
      }
    }

    @objc func drag(_ gesture: UIPanGestureRecognizer) {
      switch gesture.state {
        case .began:
          state.pieVisible = true
          state.hover(at: location(gesture))
        case .changed: state.hover(at: location(gesture))
        case .ended: state.release(at: location(gesture))
        case .cancelled, .failed: state.release(at: nil)
        default: break
      }
    }

    @objc func tap(_ gesture: UITapGestureRecognizer) {
      wm_apple_ios_window_navigation_tap(model.window, gesture)
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer,
                           shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool
    {
      false
    }
  }

  func makeCoordinator() -> Coordinator { Coordinator(model: model, state: state) }

  func makeUIView(context: Context) -> ViewGizmoHotspot {
    let hotspot = ViewGizmoHotspot(frame: .zero)
    let coordinator = context.coordinator
    hotspot.addTarget(coordinator, action: #selector(Coordinator.pressBegan), for: .touchDown)
    hotspot.addTarget(coordinator, action: #selector(Coordinator.pressEnded),
                      for: [.touchUpInside, .touchUpOutside, .touchCancel])
    hotspot.addGestureRecognizer(UIHoverGestureRecognizer(target: coordinator,
                                                          action: #selector(Coordinator.hover(_:))))

    let hold = UILongPressGestureRecognizer(target: coordinator, action: #selector(Coordinator.hold(_:)))
    hold.minimumPressDuration = 0.32
    hold.allowableMovement = 10
    hold.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue),
                              NSNumber(value: UITouch.TouchType.pencil.rawValue)]
    hotspot.addGestureRecognizer(hold)

    let tap = UITapGestureRecognizer(target: coordinator, action: #selector(Coordinator.tap(_:)))
    tap.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
    tap.require(toFail: hold)
    hotspot.addGestureRecognizer(tap)

    let drag = UIPanGestureRecognizer(target: coordinator, action: #selector(Coordinator.drag(_:)))
    drag.maximumNumberOfTouches = 1
    drag.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
    drag.cancelsTouchesInView = true
    drag.delegate = coordinator
    drag.require(toFail: hold)
    hotspot.addGestureRecognizer(drag)
    return hotspot
  }

  func updateUIView(_ hotspot: ViewGizmoHotspot, context: Context) {
    context.coordinator.model = model
    context.coordinator.state = state
    hotspot.accessibilityValue = state.pieVisible ? "View menu open" : nil
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gizmo and pie
 * \{ */

struct ViewGizmoLayer: View {
  let model: ShellModel
  @State private var state = ViewGizmoState()

  private var animation: Animation? {
    UIAccessibility.isReduceMotionEnabled ? nil : .spring(response: 0.28, dampingFraction: 0.86)
  }

  var body: some View {
    GeometryReader { proxy in
      /* Over the whole viewport, as Blender places the gizmo, keeping clear of the window's sides. */
      let insets = model.safeAreaInsets
      let safe = CGRect(origin: .zero, size: proxy.size).inset(by: UIEdgeInsets(
        top: insets.top, left: model.sideInsets.left, bottom: insets.bottom, right: model.sideInsets.right))
      let geometry = ViewGizmoGeometry(gizmo: model.navigationGizmo, window: proxy.size, safe: safe)
      ZStack(alignment: .topLeading) {
        feedback(geometry)
        ViewGizmoHotspotView(model: model, state: state)
          .frame(width: geometry.hotspot.width, height: geometry.hotspot.height)
          .position(geometry.center)
        ForEach(Array(ViewCommand.pie.enumerated()), id: \.offset) { index, view in
          if index < geometry.buttons.count {
            pieButton(view, index: index, frame: geometry.buttons[index], hub: geometry.center)
          }
        }
      }
      .frame(width: proxy.size.width, height: proxy.size.height, alignment: .topLeading)
      .onChange(of: proxy.size, initial: true) { state.geometry = geometry }
      .onChange(of: model.navigationGizmo) { state.geometry = geometry }
      .onChange(of: model.safeAreaInsets) { state.geometry = geometry }
      .onChange(of: model.sideInsets) { state.geometry = geometry }
    }
    .ignoresSafeArea()
    .animation(animation, value: state.pieVisible)
    .onReceive(NotificationCenter.default.publisher(for: UIApplication.willResignActiveNotification)) { _ in
      state.release(at: nil)
      state.pressed = false
      state.pointerOver = false
    }
  }

  /** A ring around Blender's gizmo while it is hovered, pressed or showing its pie. */
  private func feedback(_ geometry: ViewGizmoGeometry) -> some View {
    let active = state.pieVisible
    let highlighted = active || state.pointerOver || state.pressed
    return Circle()
      .fill(Color.primary.opacity(active ? 0.12 : 0.05))
      .overlay(Circle().strokeBorder(Color.primary.opacity(active ? 0.5 : 0.2), lineWidth: 1))
      .frame(width: (geometry.radius + 2) * 2, height: (geometry.radius + 2) * 2)
      .scaleEffect(active ? 1.08 : (state.pressed ? 0.94 : 1))
      .opacity(highlighted ? 1 : 0)
      .position(geometry.center)
      .allowsHitTesting(false)
      .animation(UIAccessibility.isReduceMotionEnabled ? nil : .spring(response: 0.22, dampingFraction: 0.78),
                 value: highlighted)
  }

  private func pieButton(_ view: ViewCommand, index: Int, frame: CGRect, hub: CGPoint) -> some View {
    let shown = state.pieVisible
    let hovered = state.hovered == index
    /* Hidden, each button rests a little toward the gizmo, smaller, as if folded into it. */
    let travel: CGFloat = shown ? 0 : 0.18
    return Button {
      state.release(at: CGPoint(x: frame.midX, y: frame.midY))
    } label: {
      HStack(spacing: 7) {
        if let image = view.image {
          /* White on the hovered blue glass, which an icon's own colors would blend into. */
          Image(uiImage: image)
            .renderingMode(hovered || image.renderingMode != .alwaysOriginal ? .template : .original)
            .foregroundStyle(hovered ? Color.white : Color.primary)
            .font(.system(size: 20))
        }
        Text(view.title)
          .font(.system(size: 17))
          .lineLimit(1)
      }
      .foregroundStyle(hovered ? Color.white : Color.primary)
      .padding(.horizontal, 14)
      .frame(width: frame.width, height: frame.height)
      .contentShape(Capsule())
    }
    .buttonStyle(.plain)
    .panelGlass(hovered ? .tinted(.blue) : .regular, in: Capsule())
    /* The pills float over the always-dark viewport, whatever the app's appearance. */
    .environment(\.colorScheme, .dark)
    .accessibilityLabel(view.title)
    .scaleEffect(shown ? 1 : 0.82)
    .offset(x: (hub.x - frame.midX) * travel, y: (hub.y - frame.midY) * travel)
    .opacity(shown ? 1 : 0)
    .allowsHitTesting(shown)
    .position(x: frame.midX, y: frame.midY)
  }
}

/** \} */
