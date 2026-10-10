/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Renders the image or animation with a native sheet instead of Blender's render window: live
 * progress, then the result, kept with every earlier render in a film strip to look back at and
 * share. Animations become a video. A render goes on in the background as a continued processing
 * task, with the system's progress, so leaving the app doesn't end it. Answers the
 * `GHOSTIOSRender` protocol's messages; the view controller finds it by name, see
 * `GHOST_WindowIOS.mm`. Blender renders through `bl_ui.native_render`; its state arrives in the
 * toolbar snapshot.
 */

import AVFoundation
import AVKit
import BackgroundTasks
import CryptoKit
import ImageIO
import SwiftUI
import UIKit

/** Frames rendered as images, encoded as an H.264 video off the main thread. */
private func encodeVideo(frames: [String], fps: Double, output: URL, progress: @escaping (Float) -> Void,
                         done: @escaping (URL?) -> Void)
{
  DispatchQueue.global(qos: .userInitiated).async {
    var result: URL?
    autoreleasepool {
      guard let first = frames.first.flatMap({ UIImage(contentsOfFile: $0)?.cgImage }) else { return }
      /* H.264 needs even dimensions. */
      let width = first.width & ~1, height = first.height & ~1
      try? FileManager.default.removeItem(at: output)
      guard width > 0, height > 0, let writer = try? AVAssetWriter(outputURL: output, fileType: .mp4) else { return }
      let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
        AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: width, AVVideoHeightKey: height])
      let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
        kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
        kCVPixelBufferWidthKey as String: width, kCVPixelBufferHeightKey as String: height])
      guard writer.canAdd(input) else { return }
      writer.add(input)
      writer.startWriting()
      writer.startSession(atSourceTime: .zero)
      let timescale: Int32 = 600
      var ok = true
      for (index, path) in frames.enumerated() where ok {
        autoreleasepool {
          while !input.isReadyForMoreMediaData { Thread.sleep(forTimeInterval: 0.005) }
          var buffer: CVPixelBuffer?
          if let image = UIImage(contentsOfFile: path)?.cgImage, let pool = adaptor.pixelBufferPool,
             CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer) == kCVReturnSuccess, let buffer
          {
            CVPixelBufferLockBaseAddress(buffer, [])
            let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: width, height: height,
                                    bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(buffer),
                                    space: CGColorSpaceCreateDeviceRGB(),
                                    bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue |
                                      CGBitmapInfo.byteOrder32Little.rawValue)
            context?.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            CVPixelBufferUnlockBaseAddress(buffer, [])
            let time = CMTime(value: Int64((Double(index) * Double(timescale) / fps).rounded()), timescale: timescale)
            ok = adaptor.append(buffer, withPresentationTime: time)
          }
          let fraction = Float(index + 1) / Float(frames.count)
          DispatchQueue.main.async { progress(fraction) }
        }
      }
      input.markAsFinished()
      let finished = DispatchSemaphore(value: 0)
      writer.finishWriting { finished.signal() }
      finished.wait()
      if ok && writer.status == .completed { result = output }
    }
    DispatchQueue.main.async { done(result) }
  }
}

/* -------------------------------------------------------------------- */
/** \name Render library
 *
 * Every finished render of a project, kept on disk in the app's Application Support folder, so
 * none is lost when the sheet closes, the next render starts or the app quits. Each project has
 * its own folder, found by the document's location.
 * \{ */

struct RenderItem: Codable, Identifiable, Hashable {
  let id: String
  /** The image or video, by name in the library folder. */
  let file: String
  /** A still for a video. */
  let poster: String?
  let video: Bool
  let date: Date
  let width: Int
  let height: Int
  let frames: Int
}

@MainActor
final class RenderLibrary: ObservableObject {
  private static var libraries: [String: RenderLibrary] = [:]

  /** The renders of the document at `url`; renders of an unsaved scene share one library. */
  static func forDocument(_ url: URL?) -> RenderLibrary {
    let path = url?.standardizedFileURL.resolvingSymlinksInPath().path ?? ""
    let key = path.isEmpty ? "Unsaved" :
      SHA256.hash(data: Data(path.utf8)).prefix(12).map { String(format: "%02x", $0) }.joined()
    if let library = libraries[key] { return library }
    let library = RenderLibrary(key: key)
    libraries[key] = library
    return library
  }

  /** Oldest first, as a film strip reads. */
  @Published private(set) var items: [RenderItem] = []
  let folder: URL
  private var index: URL { folder.appendingPathComponent("Renders.json") }

  private init(key: String) {
    let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    folder = support.appendingPathComponent("Renders", isDirectory: true)
      .appendingPathComponent(key, isDirectory: true)
    try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    if let data = try? Data(contentsOf: index),
       let stored = try? JSONDecoder().decode([RenderItem].self, from: data)
    {
      items = stored.filter { FileManager.default.fileExists(atPath: url($0).path) }
    }
  }

  func url(_ item: RenderItem) -> URL { folder.appendingPathComponent(item.file) }

  /** The image shown for an item: the render itself, or a video's still. */
  func still(_ item: RenderItem) -> URL? {
    if item.video { return item.poster.map { folder.appendingPathComponent($0) } }
    return url(item)
  }

  private func pixelSize(_ url: URL) -> (Int, Int) {
    guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
          let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any] else { return (0, 0) }
    return (properties[kCGImagePropertyPixelWidth] as? Int ?? 0, properties[kCGImagePropertyPixelHeight] as? Int ?? 0)
  }

  /** Keep a rendered image; the original stays where Blender wrote it. */
  func addImage(_ path: String) -> RenderItem? {
    let id = UUID().uuidString
    let file = id + ".png"
    guard (try? FileManager.default.copyItem(at: URL(fileURLWithPath: path), to: folder.appendingPathComponent(file))) != nil
    else { return nil }
    let (width, height) = pixelSize(folder.appendingPathComponent(file))
    return append(RenderItem(id: id, file: file, poster: nil, video: false, date: .now, width: width,
                             height: height, frames: 1))
  }

  /** Keep a rendered animation, moving its video into the library. */
  func addVideo(_ video: URL, poster: String?, frames: Int) -> RenderItem? {
    let id = UUID().uuidString
    let file = id + ".mp4"
    guard (try? FileManager.default.moveItem(at: video, to: folder.appendingPathComponent(file))) != nil else { return nil }
    var still: String?
    if let poster, (try? FileManager.default.copyItem(at: URL(fileURLWithPath: poster),
                                                     to: folder.appendingPathComponent(id + ".png"))) != nil
    {
      still = id + ".png"
    }
    let (width, height) = still.map { pixelSize(folder.appendingPathComponent($0)) } ?? (0, 0)
    return append(RenderItem(id: id, file: file, poster: still, video: true, date: .now, width: width,
                             height: height, frames: frames))
  }

  private func append(_ item: RenderItem) -> RenderItem {
    withMotion { items.append(item) }
    save()
    return item
  }

  func remove(_ item: RenderItem) {
    try? FileManager.default.removeItem(at: url(item))
    if let poster = item.poster { try? FileManager.default.removeItem(at: folder.appendingPathComponent(poster)) }
    withMotion { items.removeAll { $0.id == item.id } }
    save()
  }

  private func save() {
    if let data = try? JSONEncoder().encode(items) { try? data.write(to: index, options: .atomic) }
  }
}

/** Small images for the film strip, decoded off the main thread. */
enum RenderThumbnails {
  private static let cache = NSCache<NSURL, UIImage>()

  static func load(_ url: URL, size: CGFloat) async -> UIImage? {
    if let cached = cache.object(forKey: url as NSURL) { return cached }
    let image = await Task.detached(priority: .userInitiated) { () -> UIImage? in
      guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
            let cg = CGImageSourceCreateThumbnailAtIndex(source, 0, [
              kCGImageSourceCreateThumbnailFromImageAlways: true,
              kCGImageSourceThumbnailMaxPixelSize: size,
              kCGImageSourceCreateThumbnailWithTransform: true] as CFDictionary)
      else { return nil }
      return UIImage(cgImage: cg)
    }.value
    if let image { cache.setObject(image, forKey: url as NSURL) }
    return image
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Background rendering
 *
 * A render is a continued processing task: iOS keeps the app running when it leaves the
 * foreground and shows the render's progress, as for an export in Final Cut Pro. Blender's loop
 * keeps running meanwhile, without drawing, see `GHOST_ios_engine_continue_work`.
 * \{ */

@MainActor
final class BackgroundRender {
  static let shared = BackgroundRender()

  private let prefix = (Bundle.main.bundleIdentifier ?? "org.blender") + ".render"
  private var registered = false
  private var task: BGContinuedProcessingTask?
  private var pending: String?
  private var title = ""
  private var subtitle = ""
  private var fraction = 0.0
  /** Called when the system ends the task, or the user stops it from the system's progress. */
  var expired: () -> Void = {}

  private var permitted: Bool {
    (Bundle.main.object(forInfoDictionaryKey: "BGTaskSchedulerPermittedIdentifiers") as? [String] ?? [])
      .contains(prefix + ".*")
  }

  private func register() {
    guard !registered, permitted else { return }
    registered = BGTaskScheduler.shared.register(forTaskWithIdentifier: prefix + ".*", using: .main) { task in
      MainActor.assumeIsolated { self.started(task) }
    }
  }

  func begin(title: String) {
    GHOST_ios_engine_continue_work(true)
    self.title = title
    subtitle = "Starting…"
    fraction = 0
    register()
    guard registered else { return }
    let identifier = prefix + "." + UUID().uuidString
    pending = identifier
    let request = BGContinuedProcessingTaskRequest(identifier: identifier, title: title, subtitle: subtitle)
    request.strategy = .fail
    /* EEVEE and Cycles render on the GPU, which the background only allows when asked for. */
    if BGTaskScheduler.supportedResources.contains(.gpu) {
      request.requiredResources = .gpu
      if (try? BGTaskScheduler.shared.submit(request)) != nil { return }
      request.requiredResources = []
    }
    try? BGTaskScheduler.shared.submit(request)
  }

  private func started(_ background: BGTask) {
    guard let background = background as? BGContinuedProcessingTask else { return }
    guard background.identifier == pending else {
      background.setTaskCompleted(success: false)
      return
    }
    task = background
    background.progress.totalUnitCount = 1000
    background.expirationHandler = { [weak self] in
      DispatchQueue.main.async {
        guard let self, self.task === background else { return }
        self.task = nil
        self.expired()
        background.setTaskCompleted(success: false)
      }
    }
    update(title: title, subtitle: subtitle, fraction: fraction)
  }

  func update(title: String, subtitle: String, fraction: Double) {
    let changed = title != self.title || subtitle != self.subtitle
    self.title = title
    self.subtitle = subtitle
    self.fraction = fraction
    guard let task else { return }
    task.progress.completedUnitCount = Int64((min(1, max(0, fraction)) * 1000).rounded())
    if changed { task.updateTitle(title, subtitle: subtitle) }
  }

  func finish(success: Bool) {
    GHOST_ios_engine_continue_work(false)
    pending = nil
    guard let task else { return }
    self.task = nil
    task.progress.completedUnitCount = task.progress.totalUnitCount
    task.setTaskCompleted(success: success)
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sheet
 * \{ */

@MainActor
final class RenderModel: ObservableObject {
  @Published var rendering = false
  @Published var animation = false
  @Published var progress = 0.0
  @Published var phase = ""
  @Published var remaining: String?
  /** The frame rendered last, while an animation renders. */
  @Published var live: UIImage?
  /** The render shown, or nil for the one in progress. */
  @Published var selection: String?
  var started = Date.now
  var stop: () -> Void = {}
  var close: () -> Void = {}

  /** The page for the render in progress. */
  static let live = "rendering"

  /** The render shown: the one picked, else the one in progress, else the latest. */
  func current(_ library: RenderLibrary) -> String? {
    if let selection, library.items.contains(where: { $0.id == selection }) { return selection }
    return rendering ? RenderModel.live : library.items.last?.id
  }

  func select(_ id: String) {
    selection = id == RenderModel.live ? nil : id
  }
}

struct RenderSheetView: View {
  @ObservedObject var model: RenderModel
  @ObservedObject var library: RenderLibrary

  private var selected: RenderItem? {
    guard let id = model.current(library) else { return nil }
    return library.items.first { $0.id == id }
  }

  var body: some View {
    NavigationStack {
      VStack(spacing: 0) {
        RenderGallery(model: model, library: library)
          .padding(.top, 8)
          .padding(.bottom, 12)
        if model.rendering {
          RenderProgressCard(model: model)
            .padding(.horizontal, 20)
            .padding(.bottom, 12)
            .transition(.move(edge: .bottom).combined(with: .opacity))
        }
        if !library.items.isEmpty || model.rendering {
          FilmStrip(model: model, library: library)
        }
      }
      .animation(.smooth, value: model.rendering)
      .background(Color(uiColor: .systemGroupedBackground))
      .navigationTitle(model.rendering ? (model.animation ? "Rendering Animation" : "Rendering Image") : "Renders")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        ToolbarItem(placement: .cancellationAction) {
          /* Closing keeps a render going; it lands in the film strip. */
          Button(role: .close, action: model.close)
        }
        if let selected {
          ToolbarItem(placement: .primaryAction) {
            ShareLink(item: library.url(selected)) { Image(systemName: "square.and.arrow.up") }
              .accessibilityLabel("Share")
          }
        }
      }
    }
    .tint(Color(uiColor: .systemBlue))
  }
}

/** The renders side by side, a page each, as in Photos: swipe between them, pinch or double tap
 * to zoom. Picking a frame in the film strip slides the page over. */
struct RenderGallery: View {
  @ObservedObject var model: RenderModel
  @ObservedObject var library: RenderLibrary
  @State private var page: String?

  private var pages: [String] {
    library.items.map(\.id) + (model.rendering ? [RenderModel.live] : [])
  }

  var body: some View {
    GeometryReader { geometry in
      ScrollView(.horizontal) {
        LazyHStack(spacing: 0) {
          ForEach(pages, id: \.self) { id in
            pageView(id, active: id == (page ?? model.current(library)))
              .padding(.horizontal, 20)
              .frame(width: geometry.size.width, height: geometry.size.height)
              .id(id)
          }
        }
        .scrollTargetLayout()
      }
      .scrollTargetBehavior(.paging)
      .scrollIndicators(.hidden)
      .scrollPosition(id: $page)
      .overlay {
        if pages.isEmpty {
          ContentUnavailableView("No Renders", systemImage: "camera.aperture",
                                 description: Text("Render an image or animation from the Share menu."))
        }
      }
    }
    .onAppear { page = model.current(library) }
    /* Swiping the page picks it in the film strip too. */
    .onChange(of: page) { _, page in
      guard let page, page != model.current(library) else { return }
      model.select(page)
    }
    /* A frame picked in the film strip, or a new render, slides its page over. */
    .onChange(of: model.current(library)) { _, current in
      guard current != page else { return }
      withMotion(.smooth(duration: 0.35)) { page = current }
    }
  }

  @ViewBuilder
  private func pageView(_ id: String, active: Bool) -> some View {
    ZStack {
      Color(uiColor: .secondarySystemGroupedBackground)
      if id == RenderModel.live {
        if let live = model.live {
          ZoomableImage(image: live)
        }
        else {
          ProgressView().controlSize(.large)
        }
      }
      else if let item = library.items.first(where: { $0.id == id }) {
        if item.video {
          RenderVideo(url: library.url(item), playing: active)
        }
        else {
          RenderStill(url: library.url(item))
        }
      }
    }
    .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
  }
}

/** The render in progress: what is rendering, how far along, the time left, and Stop. */
struct RenderProgressCard: View {
  @ObservedObject var model: RenderModel

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack(alignment: .firstTextBaseline) {
        Text(model.phase)
          .font(.headline)
          .contentTransition(.numericText())
        Spacer()
        Text(model.progress, format: .percent.precision(.fractionLength(0)))
          .font(.headline.monospacedDigit())
          .foregroundStyle(.secondary)
          .contentTransition(.numericText(value: model.progress))
      }
      ProgressView(value: model.progress)
        .progressViewStyle(.linear)
        .controlSize(.large)
        .accessibilityLabel(model.phase)
      HStack {
        Text(model.remaining ?? "Estimating time…")
          .font(.footnote)
          .foregroundStyle(.secondary)
          .contentTransition(.opacity)
        Spacer()
        Button("Stop", role: .destructive, action: model.stop)
          .buttonStyle(.bordered)
          .controlSize(.small)
      }
    }
    .animation(.smooth, value: model.progress)
    .padding(16)
    .background(Color(uiColor: .secondarySystemGroupedBackground),
                in: RoundedRectangle(cornerRadius: 16, style: .continuous))
  }
}

struct RenderStill: View {
  let url: URL
  @State private var image: UIImage?

  var body: some View {
    ZStack {
      if let image {
        ZoomableImage(image: image)
          .transition(.opacity)
      }
    }
    .animation(.easeOut(duration: 0.15), value: image != nil)
    .task(id: url) {
      image = await Task.detached(priority: .userInitiated) {
        await UIImage(contentsOfFile: url.path)?.byPreparingForDisplay()
      }.value
    }
  }
}

/** An image to pinch, pan and double tap into, fitting the page when zoomed out, as in Photos.
 * Zoomed out it leaves sideways swipes to the gallery's paging. */
struct ZoomableImage: UIViewRepresentable {
  let image: UIImage

  func makeUIView(context: Context) -> ZoomView { ZoomView() }

  func updateUIView(_ view: ZoomView, context: Context) {
    if view.image !== image { view.image = image }
  }
}

final class ZoomView: UIScrollView, UIScrollViewDelegate {
  private let imageView = UIImageView()
  private var fitted = CGSize.zero

  var image: UIImage? {
    get { imageView.image }
    set {
      /* Lay the new image out unzoomed, then fit it. */
      minimumZoomScale = 1
      maximumZoomScale = 1
      zoomScale = 1
      imageView.image = newValue
      imageView.frame = CGRect(origin: .zero, size: newValue?.size ?? .zero)
      contentSize = imageView.frame.size
      fitted = .zero
      setNeedsLayout()
    }
  }

  override init(frame: CGRect) {
    super.init(frame: frame)
    delegate = self
    showsHorizontalScrollIndicator = false
    showsVerticalScrollIndicator = false
    contentInsetAdjustmentBehavior = .never
    decelerationRate = .fast
    bouncesZoom = true
    imageView.accessibilityIgnoresInvertColors = true
    addSubview(imageView)
    let doubleTap = UITapGestureRecognizer(target: self, action: #selector(doubleTapped(_:)))
    doubleTap.numberOfTapsRequired = 2
    addGestureRecognizer(doubleTap)
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

  private var fitScale: CGFloat {
    guard let size = image?.size, size.width > 0, size.height > 0, bounds.width > 0 else { return 1 }
    return min(bounds.width / size.width, bounds.height / size.height)
  }

  override func layoutSubviews() {
    super.layoutSubviews()
    guard image != nil, bounds.width > 1, bounds.height > 1, bounds.size != fitted else { return }
    /* A new size, or a new image: fit it, keeping a zoom the user chose if there is one. */
    let zoomedIn = fitted != .zero && zoomScale > minimumZoomScale + 0.001
    fitted = bounds.size
    minimumZoomScale = fitScale
    maximumZoomScale = max(fitScale * 8, 2)
    if !zoomedIn { zoomScale = fitScale }
    center()
  }

  private func center() {
    let x = max(0, (bounds.width - contentSize.width) / 2)
    let y = max(0, (bounds.height - contentSize.height) / 2)
    contentInset = UIEdgeInsets(top: y, left: x, bottom: y, right: x)
  }

  func viewForZooming(in scrollView: UIScrollView) -> UIView? { imageView }

  func scrollViewDidZoom(_ scrollView: UIScrollView) { center() }

  @objc private func doubleTapped(_ tap: UITapGestureRecognizer) {
    if zoomScale > minimumZoomScale + 0.001 {
      setZoomScale(minimumZoomScale, animated: true)
      return
    }
    let point = tap.location(in: imageView)
    let scale = min(maximumZoomScale, minimumZoomScale * 3)
    let size = CGSize(width: bounds.width / scale, height: bounds.height / scale)
    zoom(to: CGRect(x: point.x - size.width / 2, y: point.y - size.height / 2,
                    width: size.width, height: size.height), animated: true)
  }
}

/** A rendered animation, looping while its page is shown. */
struct RenderVideo: View {
  let url: URL
  let playing: Bool
  @State private var player: AVPlayer?
  @State private var loop: NSObjectProtocol?

  var body: some View {
    VideoPlayer(player: player)
      .onAppear {
        let player = AVPlayer(url: url)
        player.actionAtItemEnd = .none
        loop = NotificationCenter.default.addObserver(forName: .AVPlayerItemDidPlayToEndTime,
                                                      object: player.currentItem, queue: .main) { _ in
          player.seek(to: .zero)
          player.play()
        }
        self.player = player
        if playing { player.play() }
      }
      .onChange(of: playing) { _, playing in
        if playing { player?.play() }
        else { player?.pause() }
      }
      .onDisappear {
        player?.pause()
        if let loop { NotificationCenter.default.removeObserver(loop) }
      }
  }
}

/** Every render, oldest first, as the scrubber in Photos: small frames packed together with the
 * picked one opened out in the middle. Sliding the strip picks the frame that lands in the middle;
 * tapping a frame slides it there. */
struct FilmStrip: View {
  @ObservedObject var model: RenderModel
  @ObservedObject var library: RenderLibrary
  @State private var centered: String?
  @State private var deleting: RenderItem?

  private var ids: [String] {
    library.items.map(\.id) + (model.rendering ? [RenderModel.live] : [])
  }

  var body: some View {
    GeometryReader { geometry in
      ScrollView(.horizontal) {
        LazyHStack(spacing: 2) {
          ForEach(ids, id: \.self) { id in
            frame(id)
              .id(id)
          }
        }
        .scrollTargetLayout()
      }
      /* Any frame can sit in the middle, the first and last too. */
      .contentMargins(.horizontal, geometry.size.width / 2 - 24, for: .scrollContent)
      .scrollTargetBehavior(.viewAligned)
      .scrollPosition(id: $centered, anchor: .center)
      .scrollIndicators(.hidden)
    }
    .frame(height: 72)
    .background(Color(uiColor: .secondarySystemGroupedBackground))
    .onAppear { centered = model.current(library) }
    .onChange(of: centered) { _, id in
      guard let id, id != model.current(library) else { return }
      model.select(id)
      Haptics.selection()
    }
    .onChange(of: model.current(library)) { _, current in
      guard current != centered else { return }
      withMotion(.smooth(duration: 0.35)) { centered = current }
    }
    .alert("Delete Render?", isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } })) {
      Button("Cancel", role: .cancel) { deleting = nil }
      Button("Delete", role: .destructive) {
        if let deleting {
          if model.selection == deleting.id { model.selection = nil }
          library.remove(deleting)
        }
        deleting = nil
      }
    } message: {
      Text("This render will be removed from the film strip.")
    }
  }

  @ViewBuilder
  private func frame(_ id: String) -> some View {
    let selected = id == model.current(library)
    if id == RenderModel.live {
      Button {
        withMotion(.snappy) { model.select(id) }
      } label: {
        ZStack {
          Color(uiColor: .tertiarySystemFill)
          if let live = model.live { Image(uiImage: live).resizable().scaledToFill() }
          ProgressView(value: model.progress)
            .progressViewStyle(.circular)
            .controlSize(.small)
            .tint(.white)
            .padding(5)
            .background(.black.opacity(0.35), in: Circle())
        }
        .filmFrame(selected: selected, aspect: model.live.map { $0.size.width / max(1, $0.size.height) } ?? 1.5)
      }
      .buttonStyle(.plain)
      .accessibilityLabel("Render in progress")
    }
    else if let item = library.items.first(where: { $0.id == id }) {
      Button {
        withMotion(.snappy) { model.select(id) }
      } label: {
        FilmFrame(url: library.still(item), video: item.video, frames: item.frames)
          .filmFrame(selected: selected,
                     aspect: item.height > 0 ? CGFloat(item.width) / CGFloat(item.height) : 1.5)
      }
      .buttonStyle(.plain)
      .accessibilityLabel(item.video ? "Animation, \(item.frames) frames" : "Image")
      .accessibilityValue(item.date.formatted(date: .abbreviated, time: .shortened))
      .accessibilityAddTraits(selected ? .isSelected : [])
      .contextMenu {
        ShareLink(item: library.url(item)) { Label("Share", systemImage: "square.and.arrow.up") }
        Button(role: .destructive) { deleting = item } label: { Label("Delete", systemImage: "trash") }
      } preview: {
        FilmFrame(url: library.still(item), video: item.video, frames: item.frames)
          .frame(width: 360, height: 360 / max(0.3, item.height > 0 ? CGFloat(item.width) / CGFloat(item.height) : 1.5))
      }
    }
  }
}

private extension View {
  /** A narrow frame packed against its neighbours, or the picked one opened out to its shape. */
  func filmFrame(selected: Bool, aspect: CGFloat) -> some View {
    let height: CGFloat = selected ? 56 : 48
    let width = selected ? min(112, max(40, height * aspect)) : 32
    return self
      .frame(width: width, height: height)
      .clipShape(RoundedRectangle(cornerRadius: selected ? 6 : 3, style: .continuous))
      .padding(.horizontal, selected ? 6 : 0)
      .animation(.smooth(duration: 0.25), value: selected)
  }
}

struct FilmFrame: View {
  let url: URL?
  let video: Bool
  let frames: Int
  @State private var image: UIImage?

  var body: some View {
    ZStack(alignment: .bottomLeading) {
      Color(uiColor: .tertiarySystemFill)
      if let image {
        Image(uiImage: image).resizable().scaledToFill()
      }
      if video {
        Image(systemName: "play.fill")
          .font(.system(size: 8, weight: .bold))
          .foregroundStyle(.white)
          .padding(3)
          .background(.black.opacity(0.45), in: Circle())
          .padding(3)
      }
    }
    .task(id: url) {
      if let url { image = await RenderThumbnails.load(url, size: 360) }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Controller
 * \{ */

@MainActor
@objc(GHOSTIOSSwiftRender)
public final class SwiftRender: NSObject, UIAdaptivePresentationControllerDelegate {
  private weak var presenter: UIViewController?
  private var sheet: UIViewController?
  private let model = RenderModel()
  /** Where the render in progress is kept: the project it was started in. */
  private var renderLibrary = RenderLibrary.forDocument(nil)
  private var lastID: NSNumber?
  private var lastFrame: String?
  private var finishing = false

  @objc(initWithPresenter:)
  public init(presenter: UIViewController) {
    self.presenter = presenter
    super.init()
    model.stop = { [weak self] in self?.stop() }
    model.close = { [weak self] in self?.close() }
    BackgroundRender.shared.expired = { [weak self] in self?.stop() }
  }

  private func present() {
    guard sheet == nil, let presenter, presenter.presentedViewController == nil else { return }
    /* The open project's renders; while one renders, the project it belongs to. */
    let library = model.rendering ? renderLibrary : RenderLibrary.forDocument(GHOST_ios_document_url())
    let controller = UIHostingController(rootView: RenderSheetView(model: model, library: library))
    controller.modalPresentationStyle = .formSheet
    controller.preferredContentSize = CGSize(width: 760, height: 700)
    controller.presentationController?.delegate = self
    sheet = controller
    presenter.present(controller, animated: true)
  }

  /** The film strip, without rendering. */
  @objc public func showRenders() {
    model.selection = nil
    present()
  }

  @objc(renderAnimation:)
  public func renderAnimation(_ animation: Bool) {
    /* One render at a time: a second request shows the one in progress. */
    if model.rendering {
      present()
      return
    }
    lastID = nil
    lastFrame = nil
    finishing = false
    model.animation = animation
    model.rendering = true
    model.progress = 0
    model.remaining = nil
    model.live = nil
    model.selection = nil
    renderLibrary = RenderLibrary.forDocument(GHOST_ios_document_url())
    model.phase = animation ? "Starting" : "Rendering Image"
    model.started = .now
    Engine.setting("render", animation ? "animation" : "image")
    BackgroundRender.shared.begin(title: animation ? "Rendering Animation" : "Rendering Image")
    present()
  }

  /* The sheet was swiped away; a render goes on. */
  public func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
    sheet = nil
  }

  private func close() {
    let sheet = self.sheet
    self.sheet = nil
    sheet?.dismiss(animated: true)
  }

  private func stop() {
    guard model.rendering else { return }
    Engine.setting("render", "cancel")
    end(success: false)
  }

  private func end(success: Bool) {
    model.rendering = false
    model.live = nil
    finishing = false
    BackgroundRender.shared.finish(success: success)
  }

  @objc(updateStatus:)
  public func updateStatus(_ status: Any?) {
    guard let status = status as? JSON, model.rendering, !finishing,
          !jsonEqual(lastID, status["id"]) else { return }
    lastID = status["id"] as? NSNumber
    switch status.string("state") {
      case "rendering":
        showProgress(status)
      case "done":
        finishing = true
        Engine.setting("render", "clear")
        if model.animation { makeVideo(status) }
        else { keepImage(status.string("image")) }
      case "error":
        Engine.setting("render", "clear")
        end(success: false)
        fail(status.string("message") ?? "The render could not be completed.")
      case "cancelled":
        Engine.setting("render", "clear")
        end(success: false)
      default:
        break
    }
  }

  private func fail(_ message: String) {
    let alert = UIAlertController(title: "Unable to Render", message: message, preferredStyle: .alert)
    alert.addAction(UIAlertAction(title: "OK", style: .default))
    var top = presenter
    while let next = top?.presentedViewController, !next.isBeingDismissed { top = next }
    top?.present(alert, animated: true)
  }

  private func estimate() -> String? {
    let elapsed = Date.now.timeIntervalSince(model.started)
    guard model.progress > 0.02, elapsed > 2 else { return nil }
    let left = elapsed * (1 - model.progress) / model.progress
    if left < 5 { return "Almost done" }
    let formatter = DateComponentsFormatter()
    formatter.unitsStyle = .full
    formatter.maximumUnitCount = 1
    formatter.allowedUnits = left >= 3600 ? [.hour, .minute] : left >= 60 ? [.minute] : [.second]
    return formatter.string(from: left).map { "About \($0) remaining" }
  }

  private func showProgress(_ status: JSON) {
    let total = max(1, status.double("total") ?? 1)
    let done = status.double("done") ?? 0
    let samples = status.double("samples") ?? 0
    model.progress = min(1, (done + samples) / total)
    if model.animation {
      model.phase = "Frame \(Int(min(done + 1, total))) of \(Int(total))"
      /* Show each frame as it is finished. */
      if let last = status.string("last"), !last.isEmpty, last != lastFrame {
        lastFrame = last
        Task {
          let image = await Task.detached(priority: .utility) {
            await UIImage(contentsOfFile: last)?.byPreparingForDisplay()
          }.value
          if let image, self.model.rendering { self.model.live = image }
        }
      }
    }
    else {
      model.phase = "Rendering Image"
    }
    model.remaining = estimate()
    BackgroundRender.shared.update(title: model.animation ? "Rendering Animation" : "Rendering Image",
                                   subtitle: model.animation ? model.phase : "\(Int((model.progress * 100).rounded()))%",
                                   fraction: model.progress)
  }

  private func keepImage(_ path: String?) {
    guard let path, let item = renderLibrary.addImage(path) else {
      end(success: false)
      fail("The render could not be completed.")
      return
    }
    try? FileManager.default.removeItem(at: URL(fileURLWithPath: path).deletingLastPathComponent())
    finished(item)
  }

  private func makeVideo(_ status: JSON) {
    let frames = status.strings("frames")
    guard !frames.isEmpty, let folder = status.string("folder") else {
      end(success: false)
      fail("No frames were rendered.")
      return
    }
    model.phase = "Creating Video"
    model.progress = 0
    model.remaining = nil
    BackgroundRender.shared.update(title: "Rendering Animation", subtitle: "Creating Video", fraction: 0.99)
    encodeVideo(frames: frames, fps: max(1, status.double("fps") ?? 24),
                output: URL(fileURLWithPath: folder).appendingPathComponent("Animation.mp4"),
                progress: { [weak self] in self?.model.progress = Double($0) })
    { [weak self] video in
      guard let self else { return }
      guard let video, let item = self.renderLibrary.addVideo(video, poster: frames.first, frames: frames.count)
      else {
        self.end(success: false)
        self.fail("The video could not be created.")
        return
      }
      try? FileManager.default.removeItem(at: URL(fileURLWithPath: folder))
      self.finished(item)
    }
  }

  private func finished(_ item: RenderItem) {
    model.selection = item.id
    end(success: true)
    UINotificationFeedbackGenerator().notificationOccurred(.success)
  }
}

/** \} */
