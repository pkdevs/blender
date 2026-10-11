/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Apple Vision Pro: the window's controls as visionOS ornaments, floating beside the window
 * rather than over the viewport. The tool palette rests along the window's leading edge, as a
 * vertical glass ornament; the quick actions and the context menu hang below the window, as
 * visionOS toolbars do. The title bar keeps the iPad's top bar. Created by name from
 * `GHOST_WindowIOS.mm`, which feeds it the same tools and actions as the iPad's rails.
 */

#if os(visionOS)

import SwiftUI
import UIKit

@MainActor
final class ToolPaletteModel: ObservableObject {
  struct Tool: Identifiable {
    let id: String
    let title: String
    let image: UIImage?
    let selectedImage: UIImage?
    /** Related tools, a touch and hold away. */
    let menu: UIMenu?
    /** The first tool after a gap starts a new group. */
    let startsGroup: Bool
  }

  @Published var tools: [Tool] = []
  @Published var active = ""
  var choose: (String) -> Void = { _ in }
}

/** A tool: tap to use it, touch and hold for the tools grouped with it. */
struct PaletteButton: UIViewRepresentable {
  let tool: ToolPaletteModel.Tool
  let selected: Bool
  let choose: (String) -> Void

  final class Coordinator {
    var choose: (String) -> Void = { _ in }
    var id = ""
  }

  func makeCoordinator() -> Coordinator { Coordinator() }

  func makeUIView(context: Context) -> UIButton {
    let button = UIButton(type: .system)
    let coordinator = context.coordinator
    button.addAction(UIAction { _ in coordinator.choose(coordinator.id) }, for: .primaryActionTriggered)
    button.showsMenuAsPrimaryAction = false
    return button
  }

  func updateUIView(_ button: UIButton, context: Context) {
    context.coordinator.choose = choose
    context.coordinator.id = tool.id
    var configuration = UIButton.Configuration.borderless()
    configuration.image = (selected ? tool.selectedImage : nil) ?? tool.image
    configuration.preferredSymbolConfigurationForImage = UIImage.SymbolConfiguration(pointSize: 20)
    configuration.baseForegroundColor = selected ? .white : .label
    configuration.background.backgroundColor = selected ? .systemBlue : .clear
    configuration.cornerStyle = .capsule
    configuration.contentInsets = NSDirectionalEdgeInsets(top: 10, leading: 10, bottom: 10, trailing: 10)
    if button.configuration != configuration { button.configuration = configuration }
    if button.menu !== tool.menu { button.menu = tool.menu }
    button.accessibilityLabel = tool.title
    button.accessibilityIdentifier = tool.id
    button.accessibilityTraits = selected ? [.button, .selected] : .button
    button.toolTip = tool.title
  }

  func sizeThatFits(_ proposal: ProposedViewSize, uiView: UIButton, context: Context) -> CGSize? {
    CGSize(width: 44, height: 44)
  }
}

struct ToolPalette: View {
  @ObservedObject var model: ToolPaletteModel

  var body: some View {
    ScrollView(.vertical) {
      VStack(spacing: 4) {
        ForEach(model.tools) { tool in
          if tool.startsGroup {
            Divider().frame(width: 28).padding(.vertical, 4)
          }
          PaletteButton(tool: tool, selected: tool.id == model.active, choose: model.choose)
            .frame(width: 44, height: 44)
            .help(tool.title)
        }
      }
      .padding(.vertical, 10)
      .padding(.horizontal, 8)
    }
    .scrollIndicators(.hidden)
    .frame(width: 60)
    .frame(maxHeight: 640)
    .fixedSize(horizontal: false, vertical: true)
    .glassBackgroundEffect(in: Capsule())
    .accessibilityElement(children: .contain)
    .accessibilityLabel("Tools")
  }
}

/** The toolbar ornament's content: the quick actions for what is selected, then Blender's full
 * context menu. */
@MainActor
final class ToolbarOrnamentModel: ObservableObject {
  @Published var actions: [UIView] = []
  @Published var contextMenu: UIMenu?
}

/** A quick action, a UIKit button shared with the iPad's action rail, at its own size. */
struct HostedControl: UIViewRepresentable {
  let view: UIView

  func makeUIView(context: Context) -> UIView { view }
  func updateUIView(_ view: UIView, context: Context) {}

  func sizeThatFits(_ proposal: ProposedViewSize, uiView: UIView, context: Context) -> CGSize? {
    let size = uiView.systemLayoutSizeFitting(UIView.layoutFittingCompressedSize)
    return CGSize(width: size.width, height: max(size.height, 44))
  }
}

/** Every action for what is selected, as Blender's right-click menu lists them. */
struct ContextMenuButton: UIViewRepresentable {
  let menu: UIMenu?

  func makeUIView(context: Context) -> UIButton {
    let button = UIButton(type: .system)
    var configuration = UIButton.Configuration.borderless()
    configuration.image = UIImage(systemName: "ellipsis")
    configuration.preferredSymbolConfigurationForImage = UIImage.SymbolConfiguration(pointSize: 19)
    configuration.baseForegroundColor = .label
    configuration.cornerStyle = .capsule
    configuration.contentInsets = NSDirectionalEdgeInsets(top: 12, leading: 12, bottom: 12, trailing: 12)
    button.configuration = configuration
    button.showsMenuAsPrimaryAction = true
    button.accessibilityLabel = "Actions"
    button.accessibilityIdentifier = "blender.context-menu"
    button.toolTip = "Actions"
    return button
  }

  func updateUIView(_ button: UIButton, context: Context) {
    if button.menu !== menu { button.menu = menu }
  }

  func sizeThatFits(_ proposal: ProposedViewSize, uiView: UIButton, context: Context) -> CGSize? {
    CGSize(width: 44, height: 44)
  }
}

struct ToolbarOrnament: View {
  @ObservedObject var model: ToolbarOrnamentModel
  /** Quick actions shown as buttons, so the ornament stays narrower than the window; the rest
   * are in the context menu too. */
  private let inlineActions = 7

  var body: some View {
    HStack(spacing: 4) {
      ForEach(Array(model.actions.prefix(inlineActions).enumerated()), id: \.offset) { _, view in
        HostedControl(view: view)
      }
      ContextMenuButton(menu: model.contextMenu)
    }
    .padding(.horizontal, 12)
    .padding(.vertical, 8)
    .fixedSize()
    .glassBackgroundEffect(in: Capsule())
    .animation(.smooth(duration: 0.25), value: model.actions.count)
    .accessibilityElement(children: .contain)
    .accessibilityLabel("Actions")
  }
}

@MainActor
@objc(GHOSTIOSVisionChrome)
public final class VisionChrome: NSObject {
  private let palette = ToolPaletteModel()
  private weak var controller: UIViewController?
  private var tools: UIHostingOrnament<ToolPalette>!
  private let toolbar = ToolbarOrnamentModel()

  @objc(initWithController:choose:)
  public init(controller: UIViewController, choose: @escaping (String) -> Void) {
    self.controller = controller
    super.init()
    palette.choose = choose
    tools = UIHostingOrnament(sceneAnchor: .leading, contentAlignment: .trailing) {
      ToolPalette(model: self.palette)
    }
    let bar = UIHostingOrnament(sceneAnchor: .bottom, contentAlignment: .top) {
      ToolbarOrnament(model: self.toolbar)
    }
    controller.ornaments = [tools, bar]
  }

  /** Blender's full context menu, at the end of the toolbar ornament. */
  @objc(setContextMenu:)
  public func setContextMenu(_ menu: UIMenu?) {
    toolbar.contextMenu = menu
  }

  /** The quick actions for what is selected, after the editing controls. */
  @objc(setActions:)
  public func setActions(_ actions: [UIView]) {
    for view in actions {
      /* Plain on the ornament's glass, not a pane of glass of its own. */
      (view as? UIButton)?.setValue("plain", forKey: "buttonStyle")
    }
    toolbar.actions = actions
  }

  /** Tools as the iPad's rail receives them: dictionaries, with a non-dictionary for each gap. */
  @objc(updateTools:images:selectedImages:menus:)
  public func updateTools(_ tools: [Any], images: [String: UIImage], selectedImages: [String: UIImage],
                          menus: [String: UIMenu])
  {
    var result: [ToolPaletteModel.Tool] = []
    var gap = false
    for entry in tools {
      guard let tool = entry as? JSON, let id = tool.string("id") else {
        gap = !result.isEmpty
        continue
      }
      result.append(.init(id: id, title: tool.string("title") ?? id, image: images[id],
                          selectedImage: selectedImages[id], menu: menus[id], startsGroup: gap))
      gap = false
    }
    palette.tools = result
  }

  @objc public func setActiveTool(_ identifier: String) {
    palette.active = identifier
  }
}

#endif
