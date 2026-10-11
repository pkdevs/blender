/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The document window: Blender's viewport with the native interface around it, in SwiftUI.
 * Blender draws only the viewport, into GHOST's Metal view at the bottom of the window; touches
 * and the Pencil that reach that view are Blender's, everything above it is SwiftUI.
 *
 * GHOST asks for the window's controller through `WM_apple_ios_window_controller`. While the
 * interface moves to SwiftUI, this window is used when the `BlenderSwiftUIShell` default is set.
 */

import MetalKit
import SwiftUI
import UIKit

/* -------------------------------------------------------------------- */
/** \name Controller
 * \{ */

@MainActor
@objc(WMIOSSwiftDocumentController)
public final class DocumentController: UIHostingController<DocumentWindow> {
  private let model: ShellModel
  private let metalView: MTKView
  private var stateTimer: Timer?
  private var contextMenu: UIEditMenuInteraction?

  @objc(initWithMetalView:window:)
  public init(metalView: MTKView, window: UnsafeMutableRawPointer) {
    model = ShellModel(window: window)
    self.metalView = metalView
    super.init(rootView: DocumentWindow(model: model, viewport: ViewportContainer(metalView: metalView)))
    installContextMenu()
    NotificationCenter.default.addObserver(self, selector: #selector(snapshotsPublished),
                                           name: Notification.Name("BlenderSnapshotsPublished"),
                                           object: nil)
    model.pullToolbar(copy: true)
    model.refreshWindowState()
    /* Snapshots arrive by notification. Whether the viewport has drawn is read from the
     * presentation mailbox, as a held-up Blender publishes nothing. */
    stateTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.model.refreshWindowState() }
    }
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

  @objc private func snapshotsPublished() {
    model.pullToolbar()
  }

  /** The launcher showed or hid the document window. */
  @objc public func updateDocumentControls() {
    model.refreshWindowState()
  }

  /** Blender is closing the window: stop calling into it. */
  @objc public func detachBlender() {
    stateTimer?.invalidate()
    stateTimer = nil
    NotificationCenter.default.removeObserver(self)
    model.window = nil
    wm_apple_ios_document_controller_closed(self)
  }

  override public var prefersHomeIndicatorAutoHidden: Bool { true }

  override public func viewSafeAreaInsetsDidChange() {
    super.viewSafeAreaInsetsDidChange()
    if model.safeAreaInsets != view.safeAreaInsets { model.safeAreaInsets = view.safeAreaInsets }
  }
}

/* Holding a finger on the viewport opens Blender's context menu there. */
extension DocumentController: UIEditMenuInteractionDelegate, UIGestureRecognizerDelegate {
  private func installContextMenu() {
    let interaction = UIEditMenuInteraction(delegate: self)
    metalView.addInteraction(interaction)
    contextMenu = interaction
    let press = UILongPressGestureRecognizer(target: self, action: #selector(contextMenuPress(_:)))
    press.minimumPressDuration = 0.42
    press.allowableMovement = 12
    press.cancelsTouchesInView = true
    press.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]
    press.delegate = self
    /* The phone holds a finger to use the tool. */
    press.isEnabled = UIDevice.current.userInterfaceIdiom != .phone
    metalView.addGestureRecognizer(press)
  }

  @objc private func contextMenuPress(_ press: UILongPressGestureRecognizer) {
    /* Blender's own interface has its own context menus. */
    guard press.state == .began, !model.desktop else { return }
    let configuration = UIEditMenuConfiguration(identifier: nil, sourcePoint: press.location(in: metalView))
    contextMenu?.presentEditMenu(with: configuration)
  }

  public func gestureRecognizer(_ recognizer: UIGestureRecognizer, shouldReceive touch: UITouch) -> Bool {
    /* Only a press that starts on the viewport itself, not on a control above it. */
    touch.type == .direct && touch.view === metalView
  }

  public func editMenuInteraction(_ interaction: UIEditMenuInteraction,
                                  menuFor configuration: UIEditMenuConfiguration,
                                  suggestedActions: [UIMenuElement]) -> UIMenu?
  {
    let menu = wm_apple_ios_context_menu()
    return menu.children.isEmpty ? nil : menu
  }

  public func editMenuInteraction(_ interaction: UIEditMenuInteraction,
                                  targetRectFor configuration: UIEditMenuConfiguration) -> CGRect
  {
    CGRect(origin: configuration.sourcePoint, size: CGSize(width: 1, height: 1))
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Viewport
 * \{ */

/** Holds GHOST's Metal view, sized to the window at the display's full resolution. */
final class ViewportContainer: UIView {
  let metalView: MTKView

  init(metalView: MTKView) {
    self.metalView = metalView
    super.init(frame: .zero)
    metalView.autoresizingMask = []
    addSubview(metalView)
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

  override func layoutSubviews() {
    super.layoutSubviews()
    metalView.frame = bounds
    let scale = max(1, window?.traitCollection.displayScale ?? traitCollection.displayScale)
    metalView.contentScaleFactor = scale
    metalView.layer.contentsScale = scale
    let size = CGSize(width: max(1, bounds.width * scale), height: max(1, bounds.height * scale))
    if metalView.drawableSize != size {
      metalView.drawableSize = size
    }
  }
}

struct ViewportView: UIViewRepresentable {
  let container: ViewportContainer

  func makeUIView(context: Context) -> ViewportContainer { container }
  func updateUIView(_ view: ViewportContainer, context: Context) {}
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Window
 * \{ */

public struct DocumentWindow: View {
  let model: ShellModel
  let viewport: ViewportContainer

  public var body: some View {
    NavigationStack {
      ZStack {
        ViewportView(container: viewport)
          .ignoresSafeArea()
        if !model.desktop {
          QuickActionsRow(model: model)
          ToolRails(model: model)
          ViewGizmoLayer(model: model)
        }
        if !model.viewportReady {
          ProgressView()
            .controlSize(.large)
            .accessibilityLabel("Loading Viewport")
            .accessibilityIdentifier("blender.viewport.loading")
            .allowsHitTesting(false)
        }
      }
      .toolbar { DocumentToolbar(model: model) }
      .toolbarTitleDisplayMode(.inline)
    }
  }
}

struct DocumentToolbar: ToolbarContent {
  let model: ShellModel

  var body: some ToolbarContent {
    ToolbarItemGroup(placement: .topBarLeading) {
      if model.documentsAvailable {
        Button("Documents", systemImage: "chevron.backward") {
          wm_apple_ios_show_documents(nil)
        }
        .accessibilityIdentifier("blender.documents")
      }
      Button("Undo", systemImage: "arrow.uturn.backward") {
        Engine.operatorCall("ED_OT_undo")
      }
      .disabled(!model.canUndo)
      .accessibilityIdentifier("blender.undo")
    }
  }
}

/** \} */
