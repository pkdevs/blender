/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The document launcher's Blender styling that needs Swift: Suzanne as a RealityKit model to
 * flick around, and Blender's logo drawn from its vector artwork. Objective-C finds these classes
 * by name, see `GHOST_IOSLauncher.mm`.
 */

import RealityKit
import SwiftUI
import UIKit

/* -------------------------------------------------------------------- */
/** \name Suzanne
 * \{ */

/** Suzanne's place on screen and how she is turned. */
final class LauncherModel {
  let url: URL
  /** Centre and size on screen, in points of the view. */
  var center = CGPoint.zero
  var size: CGFloat = 0
  var pivot: Entity?
  var occluder: Entity?
  var unitScale: Float = 1
  var orientation = simd_quatf(angle: 0, axis: [0, 1, 0])
  /** Radians per second around the screen's vertical and horizontal axes. */
  var velocity = SIMD2<Float>(0, 0)
  var dragging = false
  var settle: Float = 1
  var intro: Float = 0
  var introTime: Float = 0

  init(url: URL) {
    self.url = url
  }

  /** At rest: looking left toward the title, the whole head slanted clockwise, breathing. */
  func rest(at time: Float) -> simd_quatf {
    return simd_quatf(angle: -0.55 + sin(time * 0.31) * 0.04, axis: [0, 0, 1]) *
      simd_quatf(angle: -0.05 + sin(time * 0.43) * 0.03, axis: [1, 0, 0]) *
      simd_quatf(angle: -0.62 + sin(time * 0.27) * 0.08, axis: [0, 1, 0])
  }

  /** Turn by a drag in points, around the screen axes. */
  func rotate(by delta: SIMD2<Float>) {
    let angle = simd_length(delta) * 0.012
    if angle > 0 {
      let axis = simd_normalize(SIMD3<Float>(delta.y, delta.x, 0))
      orientation = simd_normalize(simd_quatf(angle: angle, axis: axis) * orientation)
    }
  }
}

final class LauncherSceneState {
  let model: LauncherModel
  var viewSize = CGSize(width: 1, height: 1)
  var subscription: EventSubscription?
  var time: Float = 0
  var loaded = false
  var smoothSince: CFTimeInterval = 0
  var lastFrame: CFTimeInterval = 0
  var started = false

  init(model: LauncherModel) {
    self.model = model
  }
}

/** Fast from the first frame, slowing smoothly to a stop. */
private func easeOut(_ t: Float) -> Float {
  let u = 1 - t
  return 1 - u * u * u * u
}

private let cameraDistance: Float = 3
private let cameraFieldOfView: Float = 25

struct LauncherScene: View {
  let state: LauncherSceneState

  /** Points per scene unit at the model's depth, from the camera's vertical field of view. */
  func pointsPerUnit() -> Float {
    Float(state.viewSize.height) / (2 * cameraDistance * tan(cameraFieldOfView * .pi / 360))
  }

  /** A point in the view, on the model's plane. */
  func scenePosition(_ point: CGPoint) -> SIMD3<Float> {
    let ppu = pointsPerUnit()
    return [Float(point.x - state.viewSize.width / 2) / ppu,
            -Float(point.y - state.viewSize.height / 2) / ppu, 0]
  }

  /**
   * Whether the entrance may start: the model has loaded and the app has drawn smoothly for a
   * moment, past the hitches of launching and of drawing the model for the first time.
   */
  func ready() -> Bool {
    if state.started {
      return true
    }
    let now = CACurrentMediaTime()
    let smooth = state.lastFrame > 0 && now - state.lastFrame < 1.0 / 30.0
    state.lastFrame = now
    if !state.loaded || !smooth {
      state.smoothSince = 0
      return false
    }
    if state.smoothSince == 0 {
      state.smoothSince = now
    }
    state.started = now - state.smoothSince > 0.5
    return state.started
  }

  var body: some View {
    RealityView { content in
      content.camera = .virtual
      let camera = PerspectiveCamera()
      camera.camera.fieldOfViewInDegrees = cameraFieldOfView
      camera.look(at: .zero, from: [0, 0, cameraDistance], relativeTo: nil)
      content.add(camera)
      for (position, intensity) in [(SIMD3<Float>(1.5, 2.5, 3), Float(2600)),
                                    (SIMD3<Float>(-0.6, 0.3, 3), Float(1100)),
                                    (SIMD3<Float>(-2, 1, -1.5), Float(1100))]
      {
        let light = DirectionalLight()
        light.light.intensity = intensity
        light.look(at: .zero, from: position, relativeTo: nil)
        content.add(light)
      }
      let item = state.model
      guard let model = try? await Entity(contentsOf: item.url) else {
        return
      }
      let bounds = model.visualBounds(relativeTo: nil)
      model.position = -bounds.center
      let pivot = Entity()
      pivot.addChild(model)
      item.unitScale = 1.0 / max(bounds.extents.x, bounds.extents.y, bounds.extents.z)
      item.orientation = item.rest(at: 0)
      content.add(pivot)
      /* Until its entrance the model is drawn, opaque, behind an invisible occluder, so
       * RealityKit does its first, slow drawing out of sight. */
      let occluder = ModelEntity(mesh: .generatePlane(width: 1, height: 1),
                                 materials: [OcclusionMaterial()])
      content.add(occluder)
      item.pivot = pivot
      item.occluder = occluder
      state.loaded = true

      state.subscription = content.subscribe(to: SceneEvents.Update.self) { event in
        /* A long frame must never skip motion. */
        let dt = min(Float(event.deltaTime), 1.0 / 30.0)
        state.time += dt
        let ready = ready()
        /* The model spans 72% of its place on screen. */
        let extent = Float(item.size) * 0.72 / pointsPerUnit()
        let scale = SIMD3<Float>(repeating: extent * item.unitScale)
        let home = scenePosition(item.center)
        if ready {
          item.introTime += dt
        }
        if item.intro < 1 {
          if !ready {
            pivot.scale = scale
            pivot.position = home
            pivot.orientation = item.rest(at: state.time)
            occluder.position = home + [0, 0, extent]
            occluder.scale = SIMD3(repeating: extent * 2.5)
            return
          }
          occluder.isEnabled = false
          /* The entrance: dropping in, turning about a tilted axis, slowing smoothly into the
           * pose where the idle motion continues. A touch hands the model to the finger. */
          item.intro = item.dragging ? 1 : min(1, item.introTime / 1.6)
          let ease = easeOut(item.intro)
          let spin = simd_quatf(angle: (1 - ease) * Float.pi * 2.2,
                                axis: simd_normalize([0.35, 1, 0.25]))
          item.orientation = spin * item.rest(at: state.time)
          pivot.position = home + [0, (1 - ease) * extent * 0.9, 0]
          pivot.scale = scale * (0.6 + 0.4 * ease)
          pivot.orientation = item.orientation
          return
        }
        if !item.dragging {
          /* A flick keeps spinning, slowing down, then the model settles into its idle motion. */
          item.rotate(by: item.velocity * dt * 100)
          item.velocity *= pow(0.45, dt)
          let moving = simd_length(item.velocity) > 0.4
          item.settle = moving ? 0 : min(1, item.settle + dt * 0.6)
          if !moving {
            item.orientation = simd_slerp(item.orientation, item.rest(at: state.time),
                                          1 - pow(0.25, dt * item.settle))
          }
        }
        pivot.scale = scale
        pivot.position = home
        pivot.orientation = item.orientation
      }
    }
  }
}

/** Suzanne, to look at and flick around, the way Preview turns 3D files. */
@objc(GHOSTIOSLauncherModelView) public final class GHOSTIOSLauncherModelView: UIView,
  UIGestureRecognizerDelegate
{
  private let state: LauncherSceneState
  private var host: UIHostingController<LauncherScene>?
  private var lastTranslation = CGPoint.zero

  @objc public init(modelURL: URL) {
    state = LauncherSceneState(model: LauncherModel(url: modelURL))
    super.init(frame: .zero)
    let host = UIHostingController(rootView: LauncherScene(state: state))
    host.view.backgroundColor = .clear
    host.view.translatesAutoresizingMaskIntoConstraints = false
    addSubview(host.view)
    NSLayoutConstraint.activate([
      host.view.leadingAnchor.constraint(equalTo: leadingAnchor),
      host.view.trailingAnchor.constraint(equalTo: trailingAnchor),
      host.view.topAnchor.constraint(equalTo: topAnchor),
      host.view.bottomAnchor.constraint(equalTo: bottomAnchor),
    ])
    self.host = host
    let pan = UIPanGestureRecognizer(target: self, action: #selector(pan(_:)))
    pan.delegate = self
    /* Fingers, and the pointer of a trackpad or mouse. */
    pan.allowedTouchTypes = [UITouch.TouchType.direct, .indirect, .indirectPointer, .pencil]
      .map { NSNumber(value: $0.rawValue) }
    addGestureRecognizer(pan)
  }

  required init?(coder: NSCoder) {
    fatalError()
  }

  deinit {
    NotificationCenter.default.removeObserver(self)
  }

  public override func didMoveToWindow() {
    super.didMoveToWindow()
    NotificationCenter.default.removeObserver(self)
    guard window != nil else {
      return
    }
    for name in [UIWindow.didBecomeVisibleNotification, UIWindow.didBecomeHiddenNotification] {
      NotificationCenter.default.addObserver(self, selector: #selector(windowsChanged),
                                             name: name, object: nil)
    }
    windowsChanged()
  }

  /**
   * RealityKit draws and simulates every frame while its view is in a window. A document covers
   * the launcher in a window above it, so take the model out of the window meanwhile.
   */
  @objc private func windowsChanged() {
    guard let window, let host else {
      return
    }
    let covered = window.windowScene?.windows.contains {
      $0 !== window && !$0.isHidden && $0.windowLevel > window.windowLevel
    } ?? false
    if covered, host.view.superview != nil {
      host.view.removeFromSuperview()
    }
    else if !covered, host.view.superview == nil {
      addSubview(host.view)
      NSLayoutConstraint.activate([
        host.view.leadingAnchor.constraint(equalTo: leadingAnchor),
        host.view.trailingAnchor.constraint(equalTo: trailingAnchor),
        host.view.topAnchor.constraint(equalTo: topAnchor),
        host.view.bottomAnchor.constraint(equalTo: bottomAnchor),
      ])
    }
  }

  /** Place the model by its centre and size, in this view's points. */
  @objc public func placeModel(center: CGPoint, size: CGFloat) {
    state.model.center = center
    state.model.size = size
  }

  public override func layoutSubviews() {
    super.layoutSubviews()
    state.viewSize = bounds.size
  }

  /** Only the model takes touches, roughly the disc it fills; around it they reach the launcher. */
  public override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
    let model = state.model
    let radius = model.size * 0.38
    let dx = point.x - model.center.x, dy = point.y - model.center.y
    return dx * dx + dy * dy <= radius * radius
  }

  @objc private func pan(_ gesture: UIPanGestureRecognizer) {
    let model = state.model
    let translation = gesture.translation(in: self)
    switch gesture.state {
    case .began:
      model.dragging = true
      model.velocity = .zero
      lastTranslation = .zero
      fallthrough
    case .changed:
      model.rotate(by: SIMD2(Float(translation.x - lastTranslation.x),
                             Float(translation.y - lastTranslation.y)))
      lastTranslation = translation
    default:
      let velocity = gesture.velocity(in: self)
      model.dragging = false
      model.settle = 0
      model.velocity = gesture.state == .ended ?
        SIMD2(Float(velocity.x), Float(velocity.y)) * 0.012 : .zero
    }
  }

  /** Turning the model wins over the launcher's own drags, which wait for it. */
  public func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer,
                                shouldBeRequiredToFailBy other: UIGestureRecognizer) -> Bool
  {
    other.view !== self
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Logo
 * \{ */

/** Blender's logo, drawn from its one-colour vector artwork in the system's text colour. */
@objc(GHOSTIOSLauncherLogoView) public final class GHOSTIOSLauncherLogoView: UIView {
  private let shape = CAShapeLayer()
  private var artwork: CGPath?

  @objc public init(artworkURL: URL) {
    super.init(frame: .zero)
    if let svg = try? String(contentsOf: artworkURL, encoding: .utf8) {
      artwork = svgArtwork(svg)
    }
    layer.addSublayer(shape)
    isAccessibilityElement = true
    accessibilityLabel = "Blender"
    accessibilityTraits = .header
    registerForTraitChanges([UITraitUserInterfaceStyle.self]) {
      (view: GHOSTIOSLauncherLogoView, _: UITraitCollection) in
      view.updateColor()
    }
    updateColor()
  }

  required init?(coder: NSCoder) {
    fatalError()
  }

  /** Width over height of the artwork. */
  @objc public var aspectRatio: CGFloat {
    guard let box = artwork?.boundingBoxOfPath, box.height > 0 else {
      return 1
    }
    return box.width / box.height
  }

  private func updateColor() {
    shape.fillColor = UIColor.label.resolvedColor(with: traitCollection).cgColor
  }

  public override func layoutSubviews() {
    super.layoutSubviews()
    guard let artwork else {
      return
    }
    let box = artwork.boundingBoxOfPath
    var fit = CGAffineTransform(scaleX: bounds.width / box.width, y: bounds.height / box.height)
      .translatedBy(x: -box.minX, y: -box.minY)
    shape.frame = bounds
    shape.path = artwork.copy(using: &fit)
  }
}

/**
 * The filled shapes of an SVG, paths and rectangles, as one path with the transforms of the
 * groups around them applied. Text is left out.
 */
private func svgArtwork(_ svg: String) -> CGPath {
  func attribute(_ tag: Substring, _ name: String) -> String? {
    guard let range = tag.range(of: " \(name)=\"") else {
      return nil
    }
    let rest = tag[range.upperBound...]
    return rest.firstIndex(of: "\"").map { String(rest[..<$0]) }
  }
  func number(_ tag: Substring, _ name: String) -> CGFloat {
    CGFloat(Double((attribute(tag, name) ?? "0").replacingOccurrences(of: "px", with: "")) ?? 0)
  }
  func matrix(_ value: String?) -> CGAffineTransform {
    guard let value, value.hasPrefix("matrix(") else {
      return .identity
    }
    let v = value.dropFirst(7).dropLast().split(whereSeparator: { $0 == " " || $0 == "," })
      .compactMap { Double($0) }.map { CGFloat($0) }
    return v.count == 6 ?
      CGAffineTransform(a: v[0], b: v[1], c: v[2], d: v[3], tx: v[4], ty: v[5]) : .identity
  }
  let result = CGMutablePath()
  var groups: [CGAffineTransform] = [.identity]
  for part in svg.split(separator: "<").dropFirst() {
    let tag = part.prefix(while: { $0 != ">" })
    let filled = !(tag.contains("fill:none") || tag.contains("fill=\"none\""))
    if tag.hasPrefix("/g") {
      groups.removeLast()
    }
    else if tag.hasPrefix("g") {
      groups.append(matrix(attribute(tag, "transform")).concatenating(groups.last!))
      if tag.hasSuffix("/") {
        groups.removeLast()
      }
    }
    else if tag.hasPrefix("path"), filled, let data = attribute(tag, "d") {
      result.addPath(svgPath(data),
                     transform: matrix(attribute(tag, "transform")).concatenating(groups.last!))
    }
    else if tag.hasPrefix("rect"), filled {
      let rect = CGRect(x: number(tag, "x"), y: number(tag, "y"),
                        width: number(tag, "width"), height: number(tag, "height"))
      result.addRect(rect, transform: matrix(attribute(tag, "transform")).concatenating(groups.last!))
    }
  }
  return result
}

/** A path from SVG path data: moves, lines, cubic and quadratic curves, absolute or relative. */
private func svgPath(_ data: String) -> CGMutablePath {
  var tokens: [String] = []
  var number = ""
  func flush() {
    if !number.isEmpty {
      tokens.append(number)
      number = ""
    }
  }
  for ch in data {
    if ch.isLetter && ch != "e" && ch != "E" {
      flush()
      tokens.append(String(ch))
    }
    else if ch == "," || ch.isWhitespace {
      flush()
    }
    else if ch == "-" && !(number.last == "e" || number.last == "E") {
      flush()
      number.append(ch)
    }
    else if ch == "." && number.contains(".") && !number.contains("e") {
      flush()
      number = "."
    }
    else {
      number.append(ch)
    }
  }
  flush()

  let path = CGMutablePath()
  var i = 0
  var command = "M", previous = ""
  var current = CGPoint.zero, start = CGPoint.zero, control = CGPoint.zero
  func next() -> CGFloat {
    defer { i += 1 }
    return i < tokens.count ? CGFloat(Double(tokens[i]) ?? 0) : 0
  }
  while i < tokens.count {
    if let c = tokens[i].first, c.isLetter {
      command = tokens[i]
      i += 1
    }
    let relative = command == command.lowercased()
    let base = relative ? current : .zero
    switch command.uppercased() {
    case "M":
      current = CGPoint(x: base.x + next(), y: base.y + next())
      start = current
      path.move(to: current)
      /* Further pairs are lines. */
      command = relative ? "l" : "L"
    case "L":
      current = CGPoint(x: base.x + next(), y: base.y + next())
      path.addLine(to: current)
    case "H":
      current.x = (relative ? current.x : 0) + next()
      path.addLine(to: current)
    case "V":
      current.y = (relative ? current.y : 0) + next()
      path.addLine(to: current)
    case "C":
      let c1 = CGPoint(x: base.x + next(), y: base.y + next())
      let c2 = CGPoint(x: base.x + next(), y: base.y + next())
      current = CGPoint(x: base.x + next(), y: base.y + next())
      path.addCurve(to: current, control1: c1, control2: c2)
      control = c2
    case "S":
      let c1 = previous == "C" || previous == "S" ?
        CGPoint(x: 2 * current.x - control.x, y: 2 * current.y - control.y) : current
      let c2 = CGPoint(x: base.x + next(), y: base.y + next())
      current = CGPoint(x: base.x + next(), y: base.y + next())
      path.addCurve(to: current, control1: c1, control2: c2)
      control = c2
    case "Q":
      let c = CGPoint(x: base.x + next(), y: base.y + next())
      current = CGPoint(x: base.x + next(), y: base.y + next())
      path.addQuadCurve(to: current, control: c)
      control = c
    case "Z":
      path.closeSubpath()
      current = start
    default:
      i += 1
    }
    previous = command.uppercased()
  }
  return path
}

/** \} */
