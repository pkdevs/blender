/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The document window: Blender's viewport with the native interface around it, in SwiftUI.
 * Blender draws only the viewport, into GHOST's Metal view at the bottom of the window; touches
 * and the Pencil that reach that view are Blender's, everything above it is SwiftUI.
 *
 * GHOST asks for the window's controller through `WM_apple_ios_window_controller`, in
 * `wm_apple_ios_window.mm`.
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
  private let shell: ShellPanels
  private let commands: ShellCommands
  private let pencilMenu: PencilMenu?
  private let pencilSource: GHOSTIOSPencilMenuDelegate
  private var stateTimer: Timer?
  private var contextMenu: UIEditMenuInteraction?
  private var menuBarItems: NSArray = []
  private var menuBarRequest: UInt64 = 0
  private var menuBarMode: String?
  private var menuBarTime = Date.distantPast
  private var touchSettingsApplied = false

  @objc(initWithMetalView:window:)
  public init(metalView: MTKView, window: UnsafeMutableRawPointer) {
    let model = ShellModel(window: window)
    let shell = ShellPanels(model: model)
    let commands = ShellCommands(model: model, shell: shell)
    let viewport = ViewportContainer(metalView: metalView)
#if os(visionOS)
    /* visionOS has no Pencil to bring a radial menu up. */
    pencilMenu = nil
#else
    pencilMenu = SwiftPencilMenu(frame: CGRect(x: 0, y: 0, width: 100, height: 100))
#endif
    pencilSource = wm_apple_ios_pencil_menu_source(window, metalView)
    self.model = model
    self.metalView = metalView
    self.shell = shell
    self.commands = commands
    super.init(rootView: DocumentWindow(model: model, viewport: viewport, shell: shell, commands: commands,
                                        pencilMenu: pencilMenu))
    commands.presenter = self
    commands.render = SwiftRender(presenter: self)
    commands.settings = SwiftSettings(presenter: self)
    commands.settings?.fingerUsesToolChanged = { [weak commands] useTool in
      commands?.setFingerUsesTool(useTool)
    }
    model.onRender = { [weak commands] status in commands?.render?.updateStatus(status) }
    model.onPreferences = { [weak commands] preferences in commands?.settings?.updatePreferences(preferences) }
    model.onToolSettings = { [weak shell] settings, ack in
      shell?.panels.updateToolSettingsStores(settings, acknowledgement: ack) ?? false
    }
    /* Industry Compatible is the iPad's default keymap; switch once, then respect the choice. */
    if !UserDefaults.standard.bool(forKey: "BlenderKeymapDefaulted") {
      UserDefaults.standard.set(true, forKey: "BlenderKeymapDefaulted")
      Engine.setting("keymap", "Industry_Compatible")
    }
    if UserDefaults.standard.bool(forKey: "BlenderDesktopInterface") {
      /* Start in the interface the person left; Blender follows from its inbox. */
      Engine.setting("interface", "desktop")
    }
    pencilMenu?.delegate = pencilSource
    wm_apple_ios_window_set_pencil_menu(window, pencilMenu)
    /* Assets dragged out of the pane drop onto the viewport; files from other apps too, see
     * #FileDrop. */
    viewport.addInteraction(shell.assets.dropInteraction)
    installContextMenu()

    NotificationCenter.default.addObserver(self, selector: #selector(snapshotsPublished),
                                           name: Notification.Name("BlenderSnapshotsPublished"),
                                           object: nil)
    NotificationCenter.default.addObserver(self, selector: #selector(templatePresentation(_:)),
                                           name: Notification.Name("BlenderTemplatePresentation"),
                                           object: nil)
    model.pullToolbar(copy: true)
    shell.pullSnapshots()
    model.refreshWindowState()
    /* Snapshots arrive by notification. Whether the viewport has drawn is read from the
     * presentation mailbox, as a held-up Blender publishes nothing. */
    stateTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated {
        self?.model.refreshWindowState()
        self?.shell.compile.refresh()
      }
    }
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

  @objc private func snapshotsPublished() {
    let desktop = model.desktop
    model.pullToolbar()
    shell.pullSnapshots()
    if desktop != model.desktop { desktopChanged() }
    refreshMenuBar()
  }

  /** Blender's own Window menu can switch to its desktop interface too. */
  private func desktopChanged() {
    let desktop = model.desktop
    UserDefaults.standard.set(desktop, forKey: "BlenderDesktopInterface")
    pencilMenu?.cancel()
    if desktop { commands.setInterfaceHidden(false) }
    /* No native controls, radial menu or bottom tray over Blender's own interface. */
    wm_apple_ios_window_set_pencil_menu(model.window, desktop ? nil : pencilMenu)
  }

  @objc private func templatePresentation(_ notification: Notification) {
    shell.panels.dismiss()
    let timeline = (notification.userInfo?["timeline"] as? NSNumber)?.boolValue ?? false
    if shell.timeline.visible != timeline { shell.timeline.toggle() }
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
    pencilMenu?.delegate = nil
    wm_apple_ios_pencil_menu_source_detach(pencilSource as! NSObject)
    model.window = nil
    wm_apple_ios_document_controller_closed(self)
  }

  override public var prefersHomeIndicatorAutoHidden: Bool { true }
  override public var prefersStatusBarHidden: Bool { model.interfaceHidden }

  override public func viewSafeAreaInsetsDidChange() {
    super.viewSafeAreaInsetsDidChange()
    /* The window's sides are kept by the layout itself, which keeps the viewport beside them. */
    var insets = view.safeAreaInsets
    insets.left = 0
    insets.right = 0
    if model.safeAreaInsets != insets { model.safeAreaInsets = insets }
  }

  override public func viewDidLayoutSubviews() {
    super.viewDidLayoutSubviews()
    if !touchSettingsApplied, view.window != nil {
      /* The iPad's saved Pencil and touch choice; a phone has no Pencil, so its finger starts
       * with the tool. */
      touchSettingsApplied = true
      commands.setFingerUsesTool(UIDevice.current.userInterfaceIdiom == .phone || SwiftSettings.fingerUsesTool)
    }
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
  let shell: ShellPanels
  let commands: ShellCommands
  let pencilMenu: PencilMenu?
  @State private var stripWidth: CGFloat = 0
  @Environment(\.horizontalSizeClass) private var sizeClass

  /** The native controls are hidden in full screen, and over Blender's own interface. */
  private var controlsShown: Bool { !model.desktop && !model.interfaceHidden }

  /** The Outliner is the sidebar, which slides in and pushes the viewport over, and
   * Properties the inspector on the other side. A phone shows both as sheets. On Apple Vision Pro
   * they are windows of their own. */
  public var body: some View {
    window
      .environment(\.blenderMenuContext, model.menuContext)
  }

  @ViewBuilder
  private var window: some View {
#if os(visionOS)
    editor
#else
    if sizeClass == .compact {
      /* A phone: both are sheets over the viewport. */
      editor
        .sheet(isPresented: hierarchySheet) {
          HierarchyPanel(store: shell.panels.hierarchy) { shell.panels.setHierarchyShown(false) }
            .environment(\.panelInWindow, true)
            .presentationDetents([.medium, .large])
        }
        .sheet(isPresented: inspector) {
          InspectorPanel(store: shell.panels.inspector) { shell.panels.setInspectorShown(false) }
            .presentationDetents([.medium, .large])
        }
    }
    else {
      columns
    }
#endif
  }

#if !os(visionOS)
  /** The Outliner and Properties slide in from the window's sides and push the viewport over, in
   * one animation with it. Only one fits beside the viewport in a narrow window. */
  private var columns: some View {
    /* The toolbar spans the window and stays put as the columns slide in
     * below it. */
    NavigationStack {
      columnLayout
        .toolbar {
          DocumentToolbar(model: model, shell: shell, commands: commands, stripWidth: $stripWidth)
        }
        .toolbar(controlsShown ? .visible : .hidden, for: .navigationBar)
        .toolbarTitleDisplayMode(.inline)
    }
    .onChange(of: sizeClass, initial: true) { _, size in model.compact = size == .compact }
  }

  private var columnLayout: some View {
    HStack(spacing: 0) {
      if model.hierarchyShown && controlsShown {
        HierarchyPanel(store: shell.panels.hierarchy) { shell.panels.setHierarchyShown(false) }
          .environment(\.panelInWindow, true)
          .frame(width: 320)
          .background(Color(uiColor: .secondarySystemBackground).ignoresSafeArea())
          .transition(.move(edge: .leading))
          .accessibilityIdentifier("blender.hierarchy.panel")
      }
      layers
      if model.inspectorShown && controlsShown {
        InspectorPanel(store: shell.panels.inspector) { shell.panels.setInspectorShown(false) }
          .environment(\.panelInWindow, true)
          .frame(width: 336)
        .background(Color(uiColor: .secondarySystemBackground).ignoresSafeArea())
        .transition(.move(edge: .trailing))
        .accessibilityIdentifier("blender.inspector.panel")
      }
    }
    .onGeometryChange(for: Bool.self) { $0.size.width < 1000 } action: { shell.panels.narrow = $0 }
  }
#endif

  private var hierarchySheet: Binding<Bool> {
    Binding(get: { model.hierarchyShown && controlsShown }, set: { shell.panels.setHierarchyShown($0) })
  }

  private var inspector: Binding<Bool> {
    Binding(get: { model.inspectorShown && controlsShown }, set: { shell.panels.setInspectorShown($0) })
  }

  /** The viewport with its controls and the toolbar. */
  private var editor: some View {
    NavigationStack {
      layers
        .toolbar {
          /* Kept while the bar is hidden: items added again lose the bar's centering of the strip. */
          DocumentToolbar(model: model, shell: shell, commands: commands, stripWidth: $stripWidth)
        }
        .toolbar(controlsShown ? .visible : .hidden, for: .navigationBar)
        .toolbarTitleDisplayMode(.inline)
        .onChange(of: sizeClass, initial: true) { _, size in model.compact = size == .compact }
#if os(visionOS)
        .visionOrnaments(model: model, shown: controlsShown)
#endif
    }
  }

  /** The viewport and the controls over it, then Properties and the Outliner, which keep below the
   * toolbar. */
  private var layers: some View {
    ZStack {
      /* Under the bars, but beside the sidebar and inspector, which are the side safe areas: the
       * viewport and its controls move with the columns as they slide. */
      viewportLayers
        .ignoresSafeArea(edges: .vertical)
      CompileIndicator(progress: shell.compile)
    }
  }

  private var viewportLayers: some View {
    ZStack {
      ViewportView(container: viewport)
        .ignoresSafeArea(edges: .vertical)
        .onDrop(of: [.data, .image], delegate: FileDrop(model: model) { shell.assets.store.dragged != nil })
      if !model.viewportReady {
        ProgressView()
          .controlSize(.large)
          .accessibilityLabel("Loading Viewport")
          .accessibilityIdentifier("blender.viewport.loading")
          .allowsHitTesting(false)
      }
      if controlsShown {
#if !os(visionOS) /* Ornaments there. */
        QuickActionsRow(model: model)
        ToolRails(model: model)
#endif
        ViewGizmoLayer(model: model)
      }
      BottomTrayLayer(model: model, timeline: shell.timeline.store, assets: shell.assets.store, shown: controlsShown)
      if let pencilMenu {
        /* Frontmost; it passes touches through while it is closed. */
        PencilMenuView(menu: pencilMenu)
          .ignoresSafeArea(edges: .vertical)
      }
      if model.interfaceHidden {
        showInterfaceButton
      }
    }
  }

  /** In full screen, faint until a Pencil or pointer comes near, so it does not compete with the
   * work. */
  private var showInterfaceButton: some View {
    VStack {
      HStack {
        Spacer()
        ShowInterfaceButton { commands.setInterfaceHidden(false) }
      }
      Spacer()
    }
    .padding(12)
  }
}

private struct ShowInterfaceButton: View {
  let show: () -> Void
  @State private var near = false

  var body: some View {
    Button(action: show) {
      Image(systemName: "arrow.down.right.and.arrow.up.left")
        .frame(width: 44, height: 44)
    }
    .glassButtonStyle()
    .buttonBorderShape(.circle)
    .opacity(near ? 1 : 0.35)
    .onHover { inside in withAnimation(.easeOut(duration: 0.15)) { near = inside } }
    .accessibilityLabel("Show Interface")
    .accessibilityIdentifier("blender.interface.show")
  }
}

/** The timeline and asset pane along the bottom, the timeline resting on the pane, as far from the
 * window's sides as from its bottom. The controls resting on the bottom edge, and Blender's own
 * overlays such as the navigation gizmo, rise above them. */
struct BottomTrayLayer: View {
  let model: ShellModel
  let timeline: TimelineStore
  @ObservedObject var assets: AssetsStore
  /** Hidden in full screen and over Blender's own interface. */
  let shown: Bool

  private var tray: CGFloat {
    guard shown else { return 0 }
#if os(visionOS)
    let timelineTray: CGFloat = 0 /* A window of its own. */
#else
    let timelineTray = timeline.visible ? TimelinePanel.height(expanded: timeline.expanded) + 8 : 0
#endif
    return timelineTray + (assets.visible ? AssetsPanel.height(compact: model.compact) + 8 : 0)
  }

  var body: some View {
    let safe = model.safeAreaInsets
    let edge = safe.bottom + 6
    let transition = AnyTransition.opacity.combined(with: .offset(y: 14)).combined(with: .scale(scale: 0.98))
    GeometryReader { proxy in
      VStack(spacing: 8) {
        Spacer(minLength: 0)
        if shown {
#if !os(visionOS) /* A window of its own there. */
          if timeline.visible {
            TimelinePanel(store: timeline)
              .transition(transition)
          }
#endif
          if assets.visible {
            AssetsPanel(store: assets, compact: model.compact)
              .transition(transition)
          }
        }
      }
      .padding(.leading, max(safe.left, edge))
      .padding(.trailing, max(safe.right, edge))
      .padding(.bottom, edge)
      .onChange(of: [tray, proxy.size.height], initial: true) { _, values in
        trayChanged(values[0], height: values[1])
      }
    }
    .ignoresSafeArea(edges: .vertical)
  }

  private func trayChanged(_ tray: CGFloat, height: CGFloat) {
    if abs(tray - model.bottomTray) > 0.5 {
      withAnimation(SwiftTimeline.animation) { model.bottomTray = tray }
    }
    guard height > 0 else { return }
    /* Blender keeps its own overlays, such as the navigation gizmo, above the tray. */
    let top = height - model.safeAreaInsets.bottom - 6 - (tray > 0 ? tray - 8 : 0)
    WM_apple_ios_ui_bottom_tray_set(tray > 0 ? Float((height - top) / height) : 0)
  }
}

/** The Pencil's radial menu, which visionOS does not have. */
typealias PencilMenu = UIView & GHOSTIOSPencilMenu

/** The Pencil's radial menu, over everything else. */
struct PencilMenuView: UIViewRepresentable {
  let menu: PencilMenu

  func makeUIView(context: Context) -> PencilMenu { menu }
  func updateUIView(_ menu: PencilMenu, context: Context) {}
}

/** \} */
