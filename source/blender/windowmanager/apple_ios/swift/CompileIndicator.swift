/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The shader compilation pill below the toolbar: shown while Blender compiles shaders and is held
 * up by it, with its progress and count.
 */

import SwiftUI
import UIKit

/** Whether Blender is held up compiling shaders, polled with the window's state. */
@MainActor
@Observable
final class CompileProgress {
  /** Brief hitches are not worth a message: only show once Blender has been held up this long. */
  private static let showAfter = 0.3
  /** Once shown, stay long enough to be read rather than flicker. */
  private static let minimumVisible = 0.5

  private(set) var shown = false
  private(set) var finished = 0
  private(set) var total = 0
  @ObservationIgnored private var shownAt = Date.distantPast

  func refresh() {
    var finished: Int32 = 0, total: Int32 = 0, busy = 0.0
    let compiling = wm_apple_ios_shader_compilation(&finished, &total, &busy)
    if compiling && busy > Self.showAfter {
      if self.finished != Int(finished) { self.finished = Int(finished) }
      if self.total != Int(total) { self.total = Int(total) }
      if !shown {
        shownAt = Date()
        withAnimation(UIAccessibility.isReduceMotionEnabled ? nil : .easeOut(duration: 0.2)) { shown = true }
      }
    }
    else if shown && Date().timeIntervalSince(shownAt) >= Self.minimumVisible {
      withAnimation(UIAccessibility.isReduceMotionEnabled ? nil : .easeIn(duration: 0.2)) { shown = false }
    }
  }
}

struct CompileIndicator: View {
  let progress: CompileProgress

  var body: some View {
    VStack {
      if progress.shown {
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
        .frame(height: 32)
        .panelGlass(in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        .padding(.top, 8)
        .transition(.opacity.combined(with: .offset(y: -6)))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Compiling Shaders")
        .accessibilityValue("\(progress.finished) of \(progress.total)")
        .accessibilityIdentifier("blender.shader-compilation")
      }
      Spacer(minLength: 0)
    }
    .allowsHitTesting(false)
  }
}
