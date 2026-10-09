/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * A button drawn by SwiftUI that is still a `UIButton`, for the controls the view controller lays
 * out, animates and hit-tests itself (the view pie, the action strip, the rails). It keeps the
 * UIKit control's targets, tag, selection and highlight; SwiftUI draws its glass, title and image.
 * Created by name from `GHOST_WindowIOS.mm`.
 */

import SwiftUI
import UIKit

@MainActor
final class GlassButtonModel: ObservableObject {
  @Published var title: String?
  @Published var image: UIImage?
  @Published var selected = false
  @Published var highlighted = false
  @Published var enabled = true
  /** "glass": glass, tinted while selected; "plain": no background, tinted while selected. */
  @Published var style = "glass"
  @Published var selectedTint = UIColor.systemBlue
  @Published var foreground = UIColor.label
  @Published var selectedForeground = UIColor.white
  @Published var font = UIFont.systemFont(ofSize: 17)
  @Published var imagePadding: CGFloat = 7
  @Published var insets = NSDirectionalEdgeInsets(top: 8, leading: 14, bottom: 8, trailing: 14)
  @Published var cornerRadius: CGFloat?
  @Published var imageTrailing = false
  @Published var leading = false
}

struct GlassButtonContent: View {
  @ObservedObject var model: GlassButtonModel
  /** Measuring the natural size rather than filling the button. */
  var measuring = false

  var body: some View {
    let foreground = Color(uiColor: model.selected ? model.selectedForeground : model.foreground)
    let shape = model.cornerRadius.map { AnyShape(RoundedRectangle(cornerRadius: $0, style: .continuous)) } ??
      AnyShape(Capsule())
    HStack(spacing: model.image != nil && model.title != nil ? model.imagePadding : 0) {
      if model.imageTrailing, let title = model.title {
        Text(title)
          .font(Font(model.font))
          .lineLimit(1)
      }
      if let image = model.image {
        /* UIKit draws a button's symbol a little larger than its title. */
        Image(uiImage: image)
          .renderingMode(image.renderingMode == .alwaysOriginal ? .original : .template)
          .font(.system(size: model.font.pointSize * 1.2))
      }
      if !model.imageTrailing, let title = model.title {
        Text(title)
          .font(Font(model.font))
          .lineLimit(1)
          .truncationMode(.tail)
      }
      if model.leading && !measuring { Spacer(minLength: 0) }
    }
    .foregroundStyle(foreground)
    .padding(EdgeInsets(top: model.insets.top, leading: model.insets.leading, bottom: model.insets.bottom,
                        trailing: model.insets.trailing))
    .frame(maxWidth: measuring ? nil : .infinity, maxHeight: measuring ? nil : .infinity)
    .background {
      if model.style == "plain" {
        shape.fill(model.selected ? Color(uiColor: model.selectedTint) : .clear)
      }
    }
    .glassEffect(model.style == "glass" ?
                   (model.selected ? .regular.tint(Color(uiColor: model.selectedTint)) : .regular) :
                   model.style == "clear" ? .clear : .identity,
                 in: shape)
    .opacity(model.enabled ? 1 : 0.4)
    .scaleEffect(model.highlighted ? 0.96 : 1)
    .animation(UIAccessibility.isReduceMotionEnabled ? nil : .spring(response: 0.25, dampingFraction: 0.8),
               value: model.highlighted)
  }
}

@MainActor
@objc(GHOSTIOSSwiftButton)
final class GlassButton: UIButton {
  private let model = GlassButtonModel()
  private let host: UIHostingController<GlassButtonContent>
  private lazy var measure = UIHostingController(rootView: GlassButtonContent(model: model, measuring: true))

  override init(frame: CGRect) {
    host = UIHostingController(rootView: GlassButtonContent(model: model))
    super.init(frame: frame)
    host.view.backgroundColor = .clear
    host.view.isUserInteractionEnabled = false
    host.safeAreaRegions = []
    host.sizingOptions = []
    host.view.frame = bounds
    host.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    addSubview(host.view)
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

  @objc var buttonTitle: String? {
    get { model.title }
    set {
      model.title = newValue
      invalidateIntrinsicContentSize()
    }
  }

  @objc var buttonImage: UIImage? {
    get { model.image }
    set {
      model.image = newValue
      invalidateIntrinsicContentSize()
    }
  }

  @objc var buttonStyle: String {
    get { model.style }
    set { model.style = newValue }
  }

  @objc var selectedTint: UIColor {
    get { model.selectedTint }
    set { model.selectedTint = newValue }
  }

  @objc var foreground: UIColor {
    get { model.foreground }
    set { model.foreground = newValue }
  }

  @objc var selectedForeground: UIColor {
    get { model.selectedForeground }
    set { model.selectedForeground = newValue }
  }

  @objc var buttonFont: UIFont {
    get { model.font }
    set {
      model.font = newValue
      invalidateIntrinsicContentSize()
    }
  }

  @objc var imagePadding: CGFloat {
    get { model.imagePadding }
    set { model.imagePadding = newValue }
  }

  @objc var buttonInsets: NSDirectionalEdgeInsets {
    get { model.insets }
    set {
      model.insets = newValue
      invalidateIntrinsicContentSize()
    }
  }

  /** The image after the title, as for a checkmark. */
  @objc var imageTrailing: Bool {
    get { model.imageTrailing }
    set { model.imageTrailing = newValue }
  }

  /** Content along the leading edge rather than centered. */
  @objc var contentLeading: Bool {
    get { model.leading }
    set { model.leading = newValue }
  }

  /** A rounded rectangle instead of a capsule; 0 or less returns to the capsule. */
  @objc var buttonCornerRadius: CGFloat {
    get { model.cornerRadius ?? 0 }
    set { model.cornerRadius = newValue > 0 ? newValue : nil }
  }

  override var isSelected: Bool {
    didSet { model.selected = isSelected }
  }

  override var isHighlighted: Bool {
    didSet { model.highlighted = isHighlighted }
  }

  override var isEnabled: Bool {
    didSet { model.enabled = isEnabled }
  }

  override func sizeThatFits(_ size: CGSize) -> CGSize {
    let natural = intrinsicContentSize
    return CGSize(width: min(natural.width, size.width), height: min(natural.height, size.height))
  }

  override var intrinsicContentSize: CGSize {
    measure.sizeThatFits(in: CGSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude))
  }
}
