/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Renders the image or animation with a native sheet instead of Blender's render window: live
 * progress, then the result with the system share sheet. Animations become a video. Answers the
 * same messages as `GHOSTIOSRender`; the view controller finds it by name, see
 * `GHOST_WindowIOS.mm`. Blender renders through `bl_ui.native_render`; its state arrives in the
 * toolbar snapshot.
 */

import AVFoundation
import AVKit
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

@MainActor
final class RenderModel: ObservableObject {
  @Published var title = "Rendering Image"
  @Published var image: UIImage?
  @Published var player: AVPlayer?
  @Published var progress: Float = 0
  @Published var showsProgress = true
  @Published var detail: String? = "Preparing…"
  @Published var result: URL?
  @Published var cancellable = true
  var cancel: () -> Void = {}
  var done: () -> Void = {}
}

struct RenderSheetView: View {
  @ObservedObject var model: RenderModel

  var body: some View {
    NavigationStack {
      VStack(spacing: 12) {
        ZStack {
          Color(uiColor: .secondarySystemGroupedBackground)
          if let player = model.player {
            VideoPlayer(player: player)
          }
          else if let image = model.image {
            Image(uiImage: image).resizable().scaledToFit()
              .transition(.opacity)
              .accessibilityIgnoresInvertColors()
          }
          else {
            ProgressView().tint(.secondary)
          }
        }
        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
        .animation(.easeInOut(duration: 0.15), value: model.image)
        if model.showsProgress {
          ProgressView(value: model.progress)
            .animation(.default, value: model.progress)
            .accessibilityValue(model.detail ?? "")
        }
        if let detail = model.detail {
          Text(detail)
            .font(.footnote)
            .foregroundStyle(.secondary)
            .frame(maxWidth: .infinity)
        }
      }
      .padding(.horizontal, 20)
      .padding(.top, 12)
      .padding(.bottom, 20)
      .background(Color(uiColor: .systemGroupedBackground))
      .navigationTitle(model.title)
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        if let result = model.result {
          ToolbarItem(placement: .topBarLeading) {
            ShareLink(item: result) { Image(systemName: "square.and.arrow.up") }
              .accessibilityLabel("Share")
          }
          ToolbarItem(placement: .confirmationAction) { Button("Done", action: model.done) }
        }
        else {
          ToolbarItem(placement: .cancellationAction) {
            Button("Cancel", role: .cancel, action: model.cancel).disabled(!model.cancellable)
          }
        }
      }
    }
    .tint(Color(uiColor: .systemBlue))
    /* While rendering, only Cancel ends it. */
    .interactiveDismissDisabled(model.result == nil)
  }
}

@MainActor
@objc(GHOSTIOSSwiftRender)
public final class SwiftRender: NSObject, UIAdaptivePresentationControllerDelegate {
  private weak var presenter: UIViewController?
  private var sheet: UIViewController?
  private var model: RenderModel?
  private var loop: NSObjectProtocol?
  private var lastID: NSNumber?
  private var lastFrame: String?
  private var animation = false
  private var finishing = false

  @objc(initWithPresenter:)
  public init(presenter: UIViewController) {
    self.presenter = presenter
  }

  private func tearDown() {
    if let loop { NotificationCenter.default.removeObserver(loop) }
    loop = nil
    model?.player?.pause()
    sheet = nil
    model = nil
    lastFrame = nil
    finishing = false
  }

  @objc(renderAnimation:)
  public func renderAnimation(_ animation: Bool) {
    guard sheet == nil, presenter?.presentedViewController == nil else { return }
    self.animation = animation
    lastID = nil
    Engine.setting("render", animation ? "animation" : "image")
    let model = RenderModel()
    model.title = animation ? "Rendering Animation" : "Rendering Image"
    model.cancel = { [weak self] in
      Engine.setting("render", "cancel")
      self?.dismiss()
    }
    model.done = { [weak self] in self?.dismiss() }
    self.model = model
    let controller = UIHostingController(rootView: RenderSheetView(model: model))
    controller.modalPresentationStyle = .formSheet
    controller.preferredContentSize = CGSize(width: 540, height: 420)
    controller.presentationController?.delegate = self
    sheet = controller
    presenter?.present(controller, animated: true)
  }

  /* The result was swiped away. */
  public func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
    tearDown()
  }

  private func dismiss() {
    let sheet = self.sheet
    tearDown()
    sheet?.dismiss(animated: true)
  }

  @objc(updateStatus:)
  public func updateStatus(_ status: NSDictionary?) {
    guard let status = status as? JSON, sheet != nil, !finishing,
          !jsonEqual(lastID, status["id"]) else { return }
    lastID = status["id"] as? NSNumber
    switch status.string("state") {
      case "rendering":
        showProgress(status)
      case "done":
        finishing = true
        Engine.setting("render", "clear")
        if animation { makeVideo(status) }
        else { showImage(status.string("image")) }
      case "error":
        Engine.setting("render", "clear")
        fail(status.string("message") ?? "The render could not be completed.")
      case "cancelled":
        Engine.setting("render", "clear")
        dismiss()
      default:
        break
    }
  }

  private func fail(_ message: String) {
    let sheet = self.sheet
    let presenter = self.presenter
    tearDown()
    sheet?.dismiss(animated: true) {
      let alert = UIAlertController(title: "Unable to Render", message: message, preferredStyle: .alert)
      alert.addAction(UIAlertAction(title: "OK", style: .default))
      presenter?.present(alert, animated: true)
    }
  }

  private func showProgress(_ status: JSON) {
    guard let model else { return }
    let total = max(1, status.double("total") ?? 1)
    let done = status.double("done") ?? 0
    let samples = status.double("samples") ?? 0
    model.progress = Float(min(1, (done + samples) / total))
    if animation {
      model.detail = "Frame \(Int(min(done + 1, total))) of \(Int(total))"
      /* Show each frame as it is finished. */
      if let last = status.string("last"), !last.isEmpty, last != lastFrame {
        lastFrame = last
        if let image = UIImage(contentsOfFile: last) { model.image = image }
      }
    }
    else {
      model.detail = samples > 0 ? "\(Int((samples * 100).rounded()))%" : "Rendering…"
    }
  }

  private func showImage(_ path: String?) {
    guard let model, let path, let image = UIImage(contentsOfFile: path) else {
      finishing = false
      fail("The render could not be completed.")
      return
    }
    model.image = image
    model.showsProgress = false
    model.detail = "\(Int(image.size.width * image.scale)) × \(Int(image.size.height * image.scale))"
    showResult("Render", URL(fileURLWithPath: path))
  }

  private func makeVideo(_ status: JSON) {
    guard let model else { return }
    let frames = status.strings("frames")
    guard !frames.isEmpty, let folder = status.string("folder") else {
      finishing = false
      fail("No frames were rendered.")
      return
    }
    model.title = "Creating Video"
    model.cancellable = false
    model.detail = "Creating Video…"
    model.progress = 0
    encodeVideo(frames: frames, fps: max(1, status.double("fps") ?? 24),
                output: URL(fileURLWithPath: folder).appendingPathComponent("Animation.mp4"),
                progress: { [weak model] in model?.progress = $0 })
    { [weak self] video in
      guard let self, self.sheet != nil else { return }
      guard let video else {
        self.finishing = false
        self.fail("The video could not be created.")
        return
      }
      self.showVideo(video)
    }
  }

  private func showVideo(_ video: URL) {
    guard let model else { return }
    let player = AVPlayer(url: video)
    player.actionAtItemEnd = .none
    loop = NotificationCenter.default.addObserver(forName: .AVPlayerItemDidPlayToEndTime, object: player.currentItem,
                                                  queue: .main) { _ in
      player.seek(to: .zero)
      player.play()
    }
    model.player = player
    model.showsProgress = false
    model.detail = nil
    player.play()
    showResult("Animation", video)
  }

  private func showResult(_ title: String, _ url: URL) {
    guard let model else { return }
    model.title = title
    model.result = url
    UINotificationFeedbackGenerator().notificationOccurred(.success)
  }
}
