/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The contents of the shader compilation pill: its title, progress and count. The pill and its
 * polling are in `GHOST_IOSCompileIndicator.mm`, which creates this view by name.
 */

import SwiftUI
import UIKit

@MainActor
final class CompileProgress: ObservableObject {
  @Published var finished = 0
  @Published var total = 0
}

struct CompileProgressContent: View {
  @ObservedObject var progress: CompileProgress

  var body: some View {
    HStack(spacing: 10) {
      Text("Compiling Shaders")
        .font(.footnote)
        .foregroundStyle(.secondary)
      ProgressView(value: progress.total > 0 ? Double(progress.finished) / Double(progress.total) : 0)
        .frame(width: 88)
      Text("\(progress.finished)/\(progress.total)")
        .font(.system(size: 12).monospacedDigit())
        .foregroundStyle(.tertiary)
    }
    .tint(Color(uiColor: .systemBlue))
    .padding(.horizontal, 14)
    .fixedSize()
  }
}

@MainActor
@objc(GHOSTIOSSwiftCompileProgressView)
final class CompileProgressView: UIView {
  private let progress = CompileProgress()

  override init(frame: CGRect) {
    super.init(frame: frame)
    let host = UIHostingController(rootView: CompileProgressContent(progress: progress))
    host.view.backgroundColor = .clear
    host.sizingOptions = .intrinsicContentSize
    host.view.translatesAutoresizingMaskIntoConstraints = false
    addSubview(host.view)
    NSLayoutConstraint.activate([
      host.view.leadingAnchor.constraint(equalTo: leadingAnchor),
      host.view.trailingAnchor.constraint(equalTo: trailingAnchor),
      host.view.centerYAnchor.constraint(equalTo: centerYAnchor),
    ])
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

  @objc(setFinished:total:)
  func set(finished: Int, total: Int) {
    progress.finished = finished
    progress.total = total
  }
}
