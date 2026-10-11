/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The timeline: a floating playback strip with a scrubber whose glass knob shows and sets the
 * frame. Expanded, the scrubber grows a key track where keys are selected by tapping or boxing
 * them and dragged together to other frames. Answers the `WMIOSTimeline` protocol's messages;
 * the view controller finds it by name, see `GHOST_WindowIOS.mm`.
 */

import SwiftUI
import UIKit

private let collapsedHeight: CGFloat = 56
/* Expanded: a slim row of controls along the bottom, and the full-width scrubber above it with
 * the keys over the knob's band. */
private let controlsRowHeight: CGFloat = 44
private let knobBandHeight: CGFloat = 50
private let keyAreaHeight: CGFloat = 42
/* Small enough that the grabbed knob, scaled up, stays inside its band. */
private let knobSize: CGFloat = 32
private let knobMinWidth: CGFloat = 46

struct TimelineKey: Equatable {
  var frame: Int
  var selected: Bool
}

/** Blender's frame on its own, so playback, which changes little else, redraws only the knob,
 * the playhead and the frame number. */
@MainActor
@Observable
final class TimelineFrame {
  var blender = 1
}

@MainActor
@Observable
final class TimelineStore {
  var start = 1
  var end = 250
  var preview = false
  var playing = false
  var recording = false
  var keys: [TimelineKey] = []
  var expanded = false
  var compact = false
  /** Blender's frame; the knob follows it unless a finger holds the knob. */
  let frame = TimelineFrame()
  @ObservationIgnored var toggleExpanded: () -> Void = {}

  /** Observers redraw on every assignment, even of the same value. */
  func set<Value: Equatable>(_ keyPath: ReferenceWritableKeyPath<TimelineStore, Value>, _ value: Value) {
    if self[keyPath: keyPath] != value { self[keyPath: keyPath] = value }
  }

  func send(_ setting: String, _ value: String) {
    Engine.setting(setting, value)
  }

  /* Each edit shows at once; Blender's snapshot confirms it. */
  func select(_ frames: [Int]) {
    keys = keys.map { TimelineKey(frame: $0.frame, selected: frames.contains($0.frame)) }
    send("timeline_key_select", frames.map(String.init).joined(separator: ",") + "|0")
  }

  func moveSelected(by delta: Int) {
    /* Moved keys replace the unselected keys where they land, as in Blender. */
    var result: [Int: Bool] = [:]
    for key in keys where !key.selected && result[key.frame] == nil { result[key.frame] = false }
    for key in keys where key.selected { result[key.frame + delta] = true }
    keys = result.keys.sorted().map { TimelineKey(frame: $0, selected: result[$0]!) }
    send("timeline_key_move", "\(delta)")
  }

  func deleteSelected() {
    keys = keys.filter { !$0.selected }
    send("timeline_key_delete", "")
  }
}

/* -------------------------------------------------------------------- */
/** \name Scrubber
 * \{ */

/** One frame mapping places the knob, its playhead line and the keys. */
struct FrameMapping: Equatable {
  let start: Int
  let end: Int
  let width: CGFloat

  private var inset: CGFloat { knobSize / 2 + 4 }
  private var cells: CGFloat { CGFloat(max(1, end - start + 1)) }

  /** Frames map to whole-frame cells, inset so the knob stays inside at the first and last frame. */
  func x(_ frame: CGFloat) -> CGFloat {
    inset + (frame - CGFloat(start) + 0.5) / cells * max(1, width - 2 * inset)
  }

  func frame(at x: CGFloat) -> Int {
    let frame = start + Int(floor((x - inset) / max(1, width - 2 * inset) * cells))
    return min(end, max(start, frame))
  }
}

private func diamond(_ center: CGPoint, _ size: CGFloat) -> Path {
  Path { path in
    path.move(to: CGPoint(x: center.x, y: center.y - size / 2))
    path.addLine(to: CGPoint(x: center.x + size / 2, y: center.y))
    path.addLine(to: CGPoint(x: center.x, y: center.y + size / 2))
    path.addLine(to: CGPoint(x: center.x - size / 2, y: center.y))
    path.closeSubpath()
  }
}

private func keyColor(_ selected: Bool) -> Color { selected ? .orange : .primary }

struct Scrubber: View {
  let store: TimelineStore
  @State private var playhead: Int?
  @State private var mode: Mode = .idle
  @State private var grabbed = 0
  @State private var delta = 0
  @State private var boxFrom: CGFloat = 0
  @State private var boxTo: CGFloat = 0
  @State private var sent = Int.min

  enum Mode { case idle, scrubbing, movingKeys, boxing }

  /** For gestures and actions only: the body never reads the frame, so playback redraws just the
   * knob and the playhead, see `TimelineKnob` and `PlayheadLine`. */
  private var shownFrame: Int { playhead ?? store.frame.blender }

  var body: some View {
    /* Read here rather than in the canvases, so changing them redraws the track. */
    let start = store.start
    let end = store.end
    let expanded = store.expanded
    let preview = store.preview
    let keys = store.keys
    GeometryReader { geometry in
      let size = geometry.size
      let mapping = FrameMapping(start: start, end: end, width: size.width)
      let band = expanded ? knobBandHeight : size.height
      let trackY = size.height - band / 2
      ZStack(alignment: .topLeading) {
        Canvas { context, _ in
          drawTrack(context, size: size, mapping: mapping, band: band, trackY: trackY, expanded: expanded,
                    preview: preview, keys: keys)
        }
        if expanded {
          /* The playhead line rises from the knob through the keys. */
          PlayheadLine(frame: store.frame, playhead: playhead, mapping: mapping, trackY: trackY)
          Canvas { context, _ in drawKeys(context, size: size, mapping: mapping, band: band, keys: keys) }
        }
        TimelineKnob(frame: store.frame, playhead: playhead, lifted: mode == .scrubbing, mapping: mapping,
                     trackY: trackY)
      }
      .contentShape(Rectangle())
      .gesture(DragGesture(minimumDistance: 0).onChanged { value in
        changed(value, mapping: mapping, size: size, band: band)
      }.onEnded { value in
        ended(value, mapping: mapping, size: size, band: band)
      })
    }
    .accessibilityElement()
    .accessibilityLabel("Current Frame")
    .modifier(FrameFollower(frame: store.frame, playhead: $playhead, scrubbing: mode == .scrubbing))
    .accessibilityIdentifier("blender.timeline.scrubber")
    .accessibilityAdjustableAction { direction in
      sent = Int.min
      scrub(to: direction == .increment ? min(store.end, shownFrame + 1) : max(store.start, shownFrame - 1))
    }
  }

  private func inKeyArea(_ point: CGPoint, size: CGSize, band: CGFloat) -> Bool {
    store.expanded && point.y >= 16 && point.y <= size.height - band + 4
  }

  private func key(near point: CGPoint, mapping: FrameMapping, size: CGSize, band: CGFloat) -> Int? {
    guard inKeyArea(point, size: size, band: band) else { return nil }
    var best: Int?
    var distance: CGFloat = 22
    for key in store.keys where key.frame >= store.start && key.frame <= store.end {
      let d = abs(mapping.x(CGFloat(key.frame)) - point.x)
      if d < distance {
        distance = d
        best = key.frame
      }
    }
    return best
  }

  private func scrub(to frame: Int) {
    if frame != shownFrame {
      if mode == .scrubbing { Haptics.selection() }
      playhead = frame
    }
    if frame != sent {
      sent = frame
      store.send("timeline_frame", "\(frame)")
    }
  }

  private func changed(_ value: DragGesture.Value, mapping: FrameMapping, size: CGSize, band: CGFloat) {
    let moved = hypot(value.translation.width, value.translation.height) > 2
    if mode == .idle {
      guard moved else { return }
      let origin = value.startLocation
      /* Grabbing a key drags the selection, selecting the key first unless it is already part of
       * it; empty key track draws a selection box; anywhere else scrubs. */
      if let key = key(near: origin, mapping: mapping, size: size, band: band) {
        mode = .movingKeys
        grabbed = key
        delta = 0
        if !(store.keys.first { $0.frame == key }?.selected ?? false) { store.select([key]) }
        Haptics.impact()
      }
      else if inKeyArea(origin, size: size, band: band) {
        mode = .boxing
        boxFrom = origin.x
        boxTo = origin.x
      }
      else {
        mode = .scrubbing
        sent = Int.min
        Haptics.impact()
      }
    }
    switch mode {
      case .movingKeys:
        let next = mapping.frame(at: value.location.x) - grabbed
        if next != delta {
          delta = next
          Haptics.selection()
        }
      case .boxing:
        boxTo = value.location.x
      case .scrubbing:
        scrub(to: mapping.frame(at: value.location.x))
      case .idle:
        break
    }
  }

  private func ended(_ value: DragGesture.Value, mapping: FrameMapping, size: CGSize, band: CGFloat) {
    switch mode {
      case .idle:
        /* A tap: in the key track it selects the key under it, or clears the selection. */
        if inKeyArea(value.location, size: size, band: band) {
          store.select(key(near: value.location, mapping: mapping, size: size, band: band).map { [$0] } ?? [])
        }
        else {
          sent = Int.min
          scrub(to: mapping.frame(at: value.location.x))
        }
      case .movingKeys:
        if delta != 0 { store.moveSelected(by: delta) }
      case .boxing:
        let left = min(boxFrom, boxTo), right = max(boxFrom, boxTo)
        store.select(store.keys.filter {
          $0.frame >= store.start && $0.frame <= store.end &&
            (left...right).contains(mapping.x(CGFloat($0.frame)))
        }.map(\.frame))
      case .scrubbing:
        scrub(to: mapping.frame(at: value.location.x))
    }
    mode = .idle
    delta = 0
  }

  /** The track and, expanded, the ruler: everything under the playhead line. */
  private func drawTrack(_ context: GraphicsContext, size: CGSize, mapping: FrameMapping, band: CGFloat,
                         trackY: CGFloat, expanded: Bool, preview: Bool, keys: [TimelineKey])
  {
    /* Track: the playback range, orange when it is the preview range, as in Blender. */
    let range = preview ? Color.orange : Color(uiColor: .tertiaryLabel)
    let left = mapping.x(CGFloat(mapping.start)) - 2
    context.fill(Path(CGRect(x: left, y: trackY - 1.5,
                             width: mapping.x(CGFloat(mapping.end)) - mapping.x(CGFloat(mapping.start)) + 4,
                             height: 3)),
                 with: .color(range))
    if !expanded {
      /* Keyed frames along the track, behind the knob like the track itself. */
      for key in keys where key.frame >= mapping.start && key.frame <= mapping.end {
        let shape = diamond(CGPoint(x: mapping.x(CGFloat(key.frame)), y: trackY), 9)
        context.fill(shape, with: .color(keyColor(key.selected)))
        context.stroke(shape, with: .color(.black.opacity(0.55)), lineWidth: 1)
      }
      return
    }
    /* Ruler: label every frame step that leaves room for its number. */
    let areaBottom = size.height - band
    let perFrame = max(0.001, mapping.x(CGFloat(mapping.start + 1)) - mapping.x(CGFloat(mapping.start)))
    var step = 1
    for candidate in [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2500, 5000] {
      step = candidate
      if perFrame * CGFloat(candidate) >= 44 { break }
    }
    var frame = Int(ceil(Double(mapping.start) / Double(step))) * step
    while frame <= mapping.end {
      let x = mapping.x(CGFloat(frame))
      context.fill(Path(CGRect(x: x - 0.5, y: 14, width: 1, height: areaBottom - 14)),
                   with: .color(Color.secondary.opacity(0.3)))
      let label = context.resolve(Text("\(frame)").font(.system(size: 10).monospacedDigit())
        .foregroundStyle(.secondary))
      let measured = label.measure(in: size)
      context.draw(label, at: CGPoint(x: min(size.width - measured.width - 2, max(2, x - measured.width / 2)), y: 1),
                   anchor: .topLeading)
      frame += step
    }
  }

  /** The expanded key track: everything over the playhead line. */
  private func drawKeys(_ context: GraphicsContext, size: CGSize, mapping: FrameMapping, band: CGFloat,
                        keys: [TimelineKey])
  {
    let areaBottom = size.height - band
    /* The selection box spans the key track between the fingers' frames. */
    let boxLeft = min(boxFrom, boxTo), boxRight = max(boxFrom, boxTo)
    if mode == .boxing {
      let box = Path(roundedRect: CGRect(x: boxLeft, y: 16, width: boxRight - boxLeft, height: areaBottom - 18),
                     cornerRadius: 6)
      context.fill(box, with: .color(Color(uiColor: .systemBlue).opacity(0.15)))
      context.stroke(box, with: .color(Color(uiColor: .systemBlue).opacity(0.7)), lineWidth: 1)
    }
    /* Keys. Moving, the selected keys show at their new frames and faded at their old ones; boxing,
     * the keys inside the box show as they will be selected. */
    let keysY = 14 + (size.height - band - 14) / 2
    let rim = Color(uiColor: .systemBackground).opacity(0.7)
    for key in keys where key.frame >= mapping.start && key.frame <= mapping.end {
      let x = mapping.x(CGFloat(key.frame))
      let selected = mode == .boxing ? (x >= boxLeft && x <= boxRight) : key.selected
      var color = keyColor(selected)
      if mode == .movingKeys && selected { color = color.opacity(0.3) }
      let shape = diamond(CGPoint(x: x, y: keysY), 16)
      context.fill(shape, with: .color(color))
      context.stroke(shape, with: .color(rim), lineWidth: 1)
    }
    if mode == .movingKeys {
      for key in keys where key.selected && key.frame != grabbed {
        let shape = diamond(CGPoint(x: mapping.x(CGFloat(key.frame + delta)), y: keysY), 16)
        context.fill(shape, with: .color(.orange))
        context.stroke(shape, with: .color(rim), lineWidth: 1)
      }
      let center = CGPoint(x: mapping.x(CGFloat(grabbed + delta)), y: keysY)
      let shape = diamond(center, 20)
      context.fill(shape, with: .color(.orange))
      context.stroke(shape, with: .color(Color(uiColor: .systemBackground)), lineWidth: 1)
      let label = context.resolve(Text("\(grabbed + delta)").font(.system(size: 11, weight: .semibold).monospacedDigit()))
      let measured = label.measure(in: size)
      context.draw(label, at: CGPoint(x: min(size.width - measured.width - 2, max(2, center.x + 12)), y: center.y),
                   anchor: .leading)
    }
  }
}

/** Held, the knob turns to clear glass and swells, as system slider thumbs do; released, it
 * settles back into the playhead's tinted glass. The only part of the scrubber, with the playhead
 * line, that reads Blender's frame. */
private struct TimelineKnob: View {
  let frame: TimelineFrame
  let playhead: Int?
  let lifted: Bool
  let mapping: FrameMapping
  let trackY: CGFloat

  var body: some View {
    let shown = playhead ?? frame.blender
    let label = "\(shown)"
    let width = max(knobMinWidth, ceil((label as NSString).size(withAttributes: [
      .font: UIFont.monospacedDigitSystemFont(ofSize: 14, weight: .bold)]).width) + 20)
    Text(label)
      .font(.system(size: 14, weight: .bold).monospacedDigit())
      .minimumScaleFactor(0.7)
      .foregroundStyle(lifted ? Color.primary : .white)
      .frame(width: width, height: knobSize)
      .panelGlass(lifted ? .clear : .tinted(Color(uiColor: .systemBlue)), interactive: true, in: Capsule())
      .scaleEffect(lifted ? 1.2 : 1)
      .animation(UIAccessibility.isReduceMotionEnabled ? nil : .spring(response: 0.35, dampingFraction: 0.6),
                 value: lifted)
      .accessibilityIdentifier("blender.timeline.frame")
      .position(x: mapping.x(CGFloat(shown)), y: trackY)
  }
}

/** The expanded playhead, from the knob up through the keys. */
private struct PlayheadLine: View {
  let frame: TimelineFrame
  let playhead: Int?
  let mapping: FrameMapping
  let trackY: CGFloat

  var body: some View {
    let x = mapping.x(CGFloat(playhead ?? frame.blender))
    Rectangle()
      .fill(Color(uiColor: .systemBlue))
      .frame(width: 2, height: max(0, trackY - knobSize / 2 - 12))
      .offset(x: x - 1, y: 12)
      .allowsHitTesting(false)
  }
}

/** The scrubber's accessibility value, and the knob returning to Blender's frame once Blender
 * moves on, kept out of the scrubber's body so playback doesn't redraw the track. */
private struct FrameFollower: ViewModifier {
  let frame: TimelineFrame
  @Binding var playhead: Int?
  let scrubbing: Bool

  func body(content: Content) -> some View {
    content
      .accessibilityValue("\(playhead ?? frame.blender)")
      .onChange(of: frame.blender) { _, _ in
        if !scrubbing && playhead != nil { playhead = nil }
      }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Strip
 * \{ */

/** Reads everything but the frame, so playback leaves the buttons alone. */
struct TimelineContent: View {
  let store: TimelineStore

  var body: some View {
    Group {
      if store.expanded {
        VStack(spacing: 0) {
          Scrubber(store: store)
            .padding(.horizontal, 8)
          ZStack {
            transport
            HStack {
              if store.keys.contains(where: \.selected) { deleteButton }
              Spacer()
              keysButton
            }
            .padding(.horizontal, 8)
          }
          .frame(height: controlsRowHeight)
        }
      }
      else {
        HStack(spacing: 0) {
          transport
            .padding(.leading, 8)
          Scrubber(store: store)
            .padding(.leading, 4)
            .padding(.trailing, 2)
          keysButton
            .padding(.trailing, 8)
        }
      }
    }
    .foregroundStyle(.primary)
  }

  private var transport: some View {
    HStack(spacing: 0) {
      Button {
        /* Show the new state at once; Blender's snapshot confirms it. */
        store.recording.toggle()
        store.send("timeline", "autokey")
      } label: {
        Image(systemName: store.recording ? "record.circle.fill" : "record.circle")
          .foregroundStyle(store.recording ? Color.red : .primary)
          .frame(width: 44, height: 44)
      }
      .accessibilityLabel("Auto Keying")
      .accessibilityValue(store.recording ? "On" : "Off")
      .accessibilityAddTraits(store.recording ? .isSelected : [])
      .accessibilityIdentifier("blender.timeline.autokey")
      .padding(.trailing, 4)
      if !store.compact {
        transportButton("backward.end.fill", "Jump to Start", "start")
      }
      transportButton("backward.frame.fill", "Previous Keyframe", "prev_key")
      transportButton(store.playing ? "pause.fill" : "play.fill", store.playing ? "Pause" : "Play", "play")
      transportButton("forward.frame.fill", "Next Keyframe", "next_key")
      if !store.compact {
        transportButton("forward.end.fill", "Jump to End", "end")
      }
    }
    .buttonStyle(.plain)
    .font(.system(size: 18))
  }

  private func transportButton(_ symbol: String, _ label: String, _ command: String) -> some View {
    Button {
      store.send("timeline", command)
    } label: {
      Image(systemName: symbol).frame(width: 44, height: 44).contentShape(Rectangle())
    }
    .accessibilityLabel(label)
    .accessibilityIdentifier("blender.timeline." + command)
  }

  private var deleteButton: some View {
    Button {
      store.deleteSelected()
    } label: {
      Image(systemName: "trash").foregroundStyle(.red).frame(width: 44, height: 44)
    }
    .buttonStyle(.plain)
    .accessibilityLabel("Delete Keyframes")
    .accessibilityIdentifier("blender.timeline.delete-keys")
  }

  /** Blender's keyframe icon, filled while the key track is open. */
  private var keysButton: some View {
    Button(action: store.toggleExpanded) {
      Group {
        if let image = UIImage(named: store.expanded ? "blender.keyframe_hlt" : "blender.keyframe") {
          Image(uiImage: image.withRenderingMode(.alwaysTemplate))
        }
        else {
          Image(systemName: store.expanded ? "diamond.fill" : "diamond")
        }
      }
      .foregroundStyle(store.expanded ? PanelStyle.selection : .primary)
      .frame(width: 44, height: 44)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .accessibilityLabel("Edit Keyframes")
    .accessibilityAddTraits(store.expanded ? .isSelected : [])
    .accessibilityIdentifier("blender.timeline.keys-toggle")
  }
}

@MainActor
@objc(WMIOSSwiftTimeline)
public final class SwiftTimeline: NSObject {
  private weak var overlay: UIView?
  private let store = TimelineStore()
  private let panel: UIVisualEffectView
  private let host: UIHostingController<TimelineContent>
  private var snapshot: NSDictionary?
  @objc public let button: UIBarButtonItem
  @objc public private(set) var visible = false
  @objc public var visibilityDidChange: (() -> Void)?

  @objc(initWithOverlay:)
  public init(overlay: UIView) {
    self.overlay = overlay
    let icon = UIImage(named: "blender.time")?.withRenderingMode(.alwaysTemplate)
    button = UIBarButtonItem(image: icon ?? UIImage(systemName: "timeline.selection"), style: .plain,
                             target: nil, action: nil)
    panel = UIVisualEffectView(effect: makePanelGlass())
    host = UIHostingController(rootView: TimelineContent(store: store))
    super.init()
    button.target = self
    button.action = #selector(toggle(_:))
    button.title = "Timeline"
    button.accessibilityLabel = "Timeline"
    button.accessibilityIdentifier = "blender.timeline"
    panel.accessibilityIdentifier = "blender.timeline.panel"
    panel.layer.cornerRadius = collapsedHeight / 2
    panel.layer.cornerCurve = .continuous
    panel.clipsToBounds = true
    panel.isHidden = true
    panel.alpha = 0
    host.view.backgroundColor = .clear
    host.sizingOptions = []
    host.safeAreaRegions = []
    host.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    panel.contentView.addSubview(host.view)
    overlay.addSubview(panel)
    store.toggleExpanded = { [weak self] in self?.toggleExpanded(nil) }
#if os(visionOS)
    Self.current = self
#endif
  }

#if os(visionOS)
  /* On Apple Vision Pro the timeline is a window of its own, placed under the editor's. */
  static weak var current: SwiftTimeline?

  func windowContent() -> AnyView {
    store.set(\.compact, false)
    return AnyView(TimelineContent(store: store))
  }

  func setWindowOpen(_ open: Bool) {
    visible = open
    button.isSelected = open
    Engine.setting("timeline_visible", open ? "1" : "0")
    visibilityDidChange?()
  }
#endif

  @objc public var view: UIView { panel }
  @objc public var expanded: Bool { store.expanded }
  @objc public var height: CGFloat {
    store.expanded ? controlsRowHeight + knobBandHeight + keyAreaHeight : collapsedHeight
  }

  @objc(updateSnapshot:)
  public func updateSnapshot(_ snapshot: Any?) {
    guard let snapshot = snapshot as? NSDictionary, !(self.snapshot?.isEqual(snapshot) ?? false),
          let json = snapshot as? JSON else { return }
    self.snapshot = snapshot
    /* Playing, usually only the frame changes: leave the rest untouched so nothing else redraws. */
    let start = json.int("start") ?? 1
    store.set(\.start, start)
    store.set(\.end, max(start, json.int("end") ?? start))
    store.set(\.preview, json.bool("preview"))
    store.set(\.keys, json.array("keys").map { TimelineKey(frame: $0.int("f") ?? 0, selected: $0.bool("s")) })
    let frame = json.int("frame") ?? start
    if store.frame.blender != frame { store.frame.blender = frame }
    store.set(\.playing, json.bool("playing"))
    store.set(\.recording, json.bool("autokey"))
  }

  @objc(toggleExpanded:)
  public func toggleExpanded(_ sender: Any?) {
    store.expanded.toggle()
    panel.layer.cornerRadius = store.expanded ? 24 : collapsedHeight / 2
    visibilityDidChange?()
  }

  @objc(toggle:)
  public func toggle(_ sender: Any?) {
#if os(visionOS)
    PanelWindows.toggle(.timeline)
    return
#endif
    visible.toggle()
    button.isSelected = visible
    /* Blender lists keyframes only while the timeline shows them. */
    Engine.setting("timeline_visible", visible ? "1" : "0")
    let reduceMotion = UIAccessibility.isReduceMotionEnabled
    let hidden = reduceMotion ? .identity : CGAffineTransform(translationX: 0, y: 14).scaledBy(x: 0.98, y: 0.98)
    panel.isUserInteractionEnabled = visible
    if visible {
      overlay?.bringSubviewToFront(panel)
      if panel.isHidden {
        panel.alpha = 0
        panel.transform = hidden
      }
      panel.isHidden = false
    }
    let shown = visible
    UIView.animate(withDuration: reduceMotion ? 0 : 0.28, delay: 0, usingSpringWithDamping: 0.9,
                   initialSpringVelocity: 0, options: [.beginFromCurrentState, .allowUserInteraction])
    {
      self.panel.alpha = shown ? 1 : 0
      self.panel.transform = shown ? .identity : hidden
    } completion: { finished in
      if finished && self.panel.alpha == 0 { self.panel.isHidden = true }
    }
    visibilityDidChange?()
  }

  @objc(layoutInRect:)
  public func layout(in rect: CGRect) {
    let width = max(0, rect.width)
    /* Frame is undefined while transformed; layout must not disturb the presentation animation. */
    panel.bounds = CGRect(x: 0, y: 0, width: width, height: height)
    /* Phones keep play, keyframe steps and the scrubber. */
    store.set(\.compact, width < 520)
    panel.center = CGPoint(x: rect.midX, y: rect.maxY - height / 2)
    host.view.frame = panel.contentView.bounds
  }
}

/** \} */
