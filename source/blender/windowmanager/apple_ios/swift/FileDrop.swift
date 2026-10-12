/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Files dragged in from other apps drop on the viewport where they land, as on a Mac: images,
 * models, .blend files and anything else Blender opens. Each is copied into a folder of its own,
 * so its path stays valid for as long as the document refers to it.
 */

import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct FileDrop: DropDelegate {
  let model: ShellModel
  /** Assets dragged out of the pane drop through their own path. */
  let assetDragging: () -> Bool

  /** Image formats Blender reads directly. Others, such as HEIC from Photos, become PNG. */
  private static let readableImages: [UTType] = [.png, .jpeg, .tiff, .bmp, .webP] +
    ["com.ilm.openexr-image", "public.radiance", "com.truevision.tga-image", "public.jpeg-2000"]
      .compactMap { UTType($0) }

  func validateDrop(info: DropInfo) -> Bool {
    !assetDragging() && info.hasItemsConforming(to: [.data, .image])
  }

  func dropUpdated(info: DropInfo) -> DropProposal? {
    DropProposal(operation: .copy)
  }

  func performDrop(info: DropInfo) -> Bool {
    let providers = info.itemProviders(for: [.data, .image])
    guard let folder = Self.dropFolder(), !providers.isEmpty else { return false }
    let point = info.location
    let window = model.window
    Task {
      var paths: [String] = []
      for (index, provider) in providers.enumerated() {
        if let path = await Self.copy(provider, index: index, into: folder) { paths.append(path) }
      }
      await MainActor.run {
        if paths.isEmpty { Haptics.error() }
        else { wm_apple_ios_window_drop_files(window, paths, point) }
      }
    }
    return true
  }

  /** A new folder per drop, so files of the same name never replace each other. */
  private static func dropFolder() -> URL? {
    let files = FileManager.default
    guard let support = try? files.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                       appropriateFor: nil, create: true) else { return nil }
    let format = DateFormatter()
    format.dateFormat = "yyyy-MM-dd HH.mm.ss"
    let name = "\(format.string(from: Date())) \(UUID().uuidString.prefix(8))"
    let folder = support.appending(path: "Dropped Files", directoryHint: .isDirectory)
      .appending(path: name, directoryHint: .isDirectory)
    return (try? files.createDirectory(at: folder, withIntermediateDirectories: true)) != nil ? folder : nil
  }

  /** The original's name, with the type's extension when it has none, as Blender's drop boxes
   * and importers recognize files by extension. */
  private static func fileName(_ name: String?, _ type: UTType, _ index: Int) -> String {
    var name = (name as NSString?)?.lastPathComponent ?? ""
    if name.isEmpty || name.hasPrefix(".") { name = "Dropped \(index + 1)" }
    if (name as NSString).pathExtension.isEmpty, let ext = type.preferredFilenameExtension {
      name = (name as NSString).appendingPathExtension(ext) ?? name
    }
    return name
  }

  /** Copy one item into `folder`, returning its path, or nil when it offers nothing Blender
   * could open. */
  private static func copy(_ provider: NSItemProvider, index: Int, into folder: URL) async -> String? {
    let types = provider.registeredTypeIdentifiers.compactMap { UTType($0) }
    if provider.hasItemConformingToTypeIdentifier(UTType.image.identifier) {
      for readable in readableImages {
        if let type = types.first(where: { $0.conforms(to: readable) }) {
          return await copyFile(provider, type: type, index: index, into: folder)
        }
      }
      if provider.canLoadObject(ofClass: UIImage.self) {
        let image: UIImage? = await withCheckedContinuation { done in
          _ = provider.loadObject(ofClass: UIImage.self) { object, _ in done.resume(returning: object as? UIImage) }
        }
        let name = fileName((provider.suggestedName as NSString?)?.deletingPathExtension, .png, index)
        let target = folder.appending(path: name)
        guard let png = image?.pngData(), (try? png.write(to: target)) != nil else { return nil }
        return target.path
      }
    }
    /* Any other file (models, .blend, fonts, ...): copied as it is. */
    guard let type = types.first(where: { $0.conforms(to: .data) && !$0.conforms(to: .url) }) else { return nil }
    return await copyFile(provider, type: type, index: index, into: folder)
  }

  private static func copyFile(_ provider: NSItemProvider, type: UTType, index: Int, into folder: URL) async -> String? {
    let suggested = provider.suggestedName
    return await withCheckedContinuation { done in
      _ = provider.loadFileRepresentation(forTypeIdentifier: type.identifier) { url, _ in
        /* The file is deleted when this returns, so copy it now. */
        guard let url else { return done.resume(returning: nil) }
        let target = folder.appending(path: fileName(url.lastPathComponent.isEmpty ? suggested : url.lastPathComponent,
                                                     type, index))
        done.resume(returning: (try? FileManager.default.copyItem(at: url, to: target)) != nil ? target.path : nil)
      }
    }
  }
}
