/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The document window's toolbar: back to the documents, the editing mode, adding to the scene,
 * the mesh selection mode and undo; the active tool's settings in the middle; then the panels,
 * viewport shading, Share and More. The system lays its glass groups out and moves what does not
 * fit into its overflow menu.
 */

import SwiftUI
import UIKit

/** What the toolbar's menus do outside the window: presenting sheets, sharing and saving. */
@MainActor
final class ShellCommands {
  weak var presenter: UIViewController?
  var render: SwiftRender?
  var settings: SwiftSettings?
  let model: ShellModel
  let shell: ShellPanels
  /** Where the Share button is, for the share sheet's popover. */
  weak var shareSource: UIView?

  init(model: ShellModel, shell: ShellPanels) {
    self.model = model
    self.shell = shell
  }

  private func section(_ children: [UIMenuElement]) -> UIMenu {
    UIMenu(title: "", options: .displayInline, children: children)
  }

  /** Share: what leaves the document. Documents save themselves, so
   * there is no Save; going back to the documents keeps the work. */
  func shareMenu() -> UIMenu {
    let share = UIAction(title: "Share…", image: UIImage(systemName: "square.and.arrow.up")) { [weak self] _ in
      self?.shareDocument()
    }
    let saveAs = UIAction(title: "Save As…", image: UIImage(systemName: "doc.on.doc")) { [weak self] _ in
      self?.saveDocumentAs()
    }
    /* Blender's own Import and Export menus, so every format it and its add-ons support is
     * listed. Their file browsers open the system's document picker. */
    let importMenu = wm_apple_ios_blender_menu(
      ["title": "Import", "icon": "IMPORT", "route": ["root": "TOPBAR_MT_file_import", "path": []]],
      "blender.file.import")
    let exportMenu = wm_apple_ios_blender_menu(
      ["title": "Export", "icon": "EXPORT", "route": ["root": "TOPBAR_MT_file_export", "path": []]],
      "blender.file.export")
    let renderImage = UIAction(title: "Render Image", image: UIImage(systemName: "photo")) { [weak self] _ in
      self?.render?.renderAnimation(false)
    }
    let renderAnimation = UIAction(title: "Render Animation", image: UIImage(systemName: "film")) { [weak self] _ in
      self?.render?.renderAnimation(true)
    }
    /* Every earlier render, in the render sheet's film strip. */
    let renders = UIAction(title: "Renders", image: UIImage(systemName: "photo.stack")) { [weak self] _ in
      self?.render?.showRenders()
    }
    let saveTemplate = UIAction(title: "Save as Template…", image: UIImage(systemName: "square.on.square")) {
      [weak self] _ in self?.saveAsTemplate()
    }
    return UIMenu(children: [section([share, saveAs]), section([importMenu, exportMenu]),
                             section([renderImage, renderAnimation, renders]), section([saveTemplate])])
  }

  /** More: smaller things, with viewport options that show their current state when opened. */
  func moreMenu() -> UIMenu {
    let create = UIAction(title: "New…", image: UIImage(systemName: "doc.badge.plus")) { [weak self] _ in
      guard let presenter = self?.presenter else { return }
      wm_apple_ios_new_document(presenter)
    }
    let viewport = UIDeferredMenuElement.uncached { [weak self] completion in
      let state = self?.model.viewport
      let toggles = ViewportToggle.all
        .map { item in
          let toggle = UIAction(title: item.title, image: UIImage(systemName: item.symbol)) { _ in
            Engine.setting("viewport_toggle", item.name)
          }
          toggle.state = state?.bool(item.name) == true ? .on : .off
          toggle.attributes = state == nil ? .disabled : []
          return toggle
        }
      completion([UIMenu(title: "", options: .displayInline, children: toggles)])
    }
    let hide = UIAction(title: "Hide Interface", image: UIImage(systemName: "arrow.up.left.and.arrow.down.right")) {
      [weak self] _ in
      self?.setInterfaceHidden(true)
    }
    let desktop = UIAction(title: "Switch to Desktop Interface", image: UIImage(systemName: "macwindow")) { _ in
      Engine.setting("interface", "desktop")
    }
    let settings = UIAction(title: "Settings", image: UIImage(systemName: "gearshape")) { [weak self] _ in
      self?.settings?.present()
    }
    return UIMenu(children: [section([create]), viewport, section([hide, desktop]), section([settings])])
  }

  /** A narrow window's one menu: the panels, the view, One Finger, the selection mode and tool
   * settings, then Share and More. */
  func compactMenu() -> UIMenu {
    func toggle(_ title: String, _ image: UIImage?, _ on: Bool, _ action: @escaping () -> Void) -> UIAction {
      let toggle = UIAction(title: title, image: image) { _ in action() }
      toggle.state = on ? .on : .off
      return toggle
    }
    let panels = [
      toggle("Outliner", BlenderIcon.image("OUTLINER"), model.hierarchyShown) { [shell] in
        shell.panels.toggleHierarchy()
      },
      toggle("Timeline", UIImage(named: "blender.time")?.withRenderingMode(.alwaysTemplate), model.timelineShown) {
        [shell] in shell.timeline.toggle()
      },
      toggle("Properties", BlenderIcon.image("PROPERTIES"), model.inspectorShown) { [shell] in
        shell.panels.toggleInspector()
      },
    ]
    let shading = UIAction(title: "Viewport Shading", image: UIImage(systemName: "circle.lefthalf.filled")) {
      [model] _ in model.shadingOpen = true
    }
    let view = UIMenu(title: "View", image: UIImage(systemName: "eye"), children: ViewCommand.pie.map { command in
      UIAction(title: command.title, image: command.image) { _ in command.run() }
    })
    let finger = UIMenu(title: "One Finger", image: UIImage(systemName: "hand.point.up"), children: [
      toggle("Use Tool", wm_apple_ios_icon("control.finger_tool", "cursorarrow"), model.fingerUsesTool) {
        [weak self] in self?.setFingerUsesTool(true)
      },
      toggle("Orbit View", wm_apple_ios_icon("operator.VIEW3D_OT_rotate", "rotate.3d"), !model.fingerUsesTool) {
        [weak self] in self?.setFingerUsesTool(false)
      },
    ])
    var editing: [UIMenuElement] = []
    if model.mode == "mesh_edit" {
      let modes = [("VERT", "Vertex Select", "circle.fill"), ("EDGE", "Edge Select", "line.diagonal"),
                   ("FACE", "Face Select", "square.fill")]
      editing.append(UIMenu(title: "Select Mode", image: wm_apple_ios_icon("mesh.VERT", "circle.fill"),
                            children: modes.map { value, title, symbol in
        toggle(title, wm_apple_ios_icon("mesh.\(value)", symbol), model.meshSelectMode == value) {
          Engine.setting("mesh_select_mode", value)
        }
      }))
    }
    if transformTools.contains(model.activeTool) {
      editing += TransformSetting.all.map { setting in
        UIMenu(title: setting.title, image: wm_apple_ios_icon("control.\(setting.title)", setting.symbol),
               children: setting.options.map { option in
          toggle(option.title, setting.image(option), model.transformSettings[setting.key] == option.value) {
            Engine.setting(setting.key, option.value)
          }
        })
      }
    }
    else if model.toolSettingsShown {
      editing.append(UIAction(title: "Tool Settings", image: UIImage(systemName: "slider.horizontal.3")) {
        [model] _ in model.toolSettingsOpen = true
      })
    }
    let share = UIMenu(title: "Share", image: UIImage(systemName: "square.and.arrow.up"),
                       children: shareMenu().children)
    return UIMenu(children: [section(panels), section([shading, view, finger]), section(editing),
                             section([share])] + moreMenu().children)
  }

  func setFingerUsesTool(_ useTool: Bool) {
    model.fingerUsesTool = useTool
    wm_apple_ios_window_set_finger_tool(model.window, useTool)
  }

  func setInterfaceHidden(_ hidden: Bool) {
    withAnimation(UIAccessibility.isReduceMotionEnabled ? nil : .easeInOut(duration: 0.25)) {
      model.interfaceHidden = hidden
    }
#if !os(visionOS)
    presenter?.setNeedsStatusBarAppearanceUpdate()
#endif
  }

  private func saveDocumentAs() {
    guard let document = wm_apple_ios_open_document() else { return }
    /* Save a copy wherever the person picks, as Files apps do for Save As. */
    document.save(to: document.fileURL, for: .forOverwriting) { [weak self] _ in
      let picker = UIDocumentPickerViewController(forExporting: [document.fileURL], asCopy: true)
      self?.presenter?.present(picker, animated: true)
    }
  }

  private func shareDocument() {
    guard let document = wm_apple_ios_open_document() else { return }
    /* Share the file as it is now. */
    document.save(to: document.fileURL, for: .forOverwriting) { [weak self] _ in
      let activity = UIActivityViewController(activityItems: [document.fileURL], applicationActivities: nil)
      activity.popoverPresentationController?.sourceView = self?.shareSource
      self?.presenter?.present(activity, animated: true)
    }
  }

  private func saveAsTemplate() {
    let alert = UIAlertController(title: "Save as Template", message: nil, preferredStyle: .alert)
    alert.addTextField { field in
      field.placeholder = "Template Name"
      field.text = "My Template"
    }
    alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
    alert.addAction(UIAlertAction(title: "Save", style: .default) { [weak self, weak alert] _ in
      guard let self, let name = alert?.textFields?.first?.text?.trimmingCharacters(in: .whitespacesAndNewlines),
            !name.isEmpty
      else { return }
      let progress = UIViewController()
      progress.modalPresentationStyle = .formSheet
      progress.isModalInPresentation = true
      progress.preferredContentSize = CGSize(width: 320, height: 180)
      progress.view.backgroundColor = .systemBackground
      var loading = UIContentUnavailableConfiguration.loading()
      loading.text = "Saving Template"
      progress.contentUnavailableConfiguration = loading
      presenter?.present(progress, animated: true) {
        wm_apple_ios_save_template(name, ["timeline": self.model.timelineShown]) { error in
          progress.dismiss(animated: true) {
            let result = UIAlertController(
              title: error == nil ? "Template Saved" : "Could Not Save Template",
              message: error ?? "Available in My Templates when creating a new document.", preferredStyle: .alert)
            result.addAction(UIAlertAction(title: "OK", style: .default))
            self.presenter?.present(result, animated: true)
          }
        }
      }
    })
    presenter?.present(alert, animated: true)
  }
}

/* -------------------------------------------------------------------- */
/** \name Toolbar
 * \{ */

/** The transform tools, whose settings are the orientation, pivot, snapping and proportional
 * editing menus. */
private let transformTools: Set<String> = ["builtin.move", "builtin.rotate", "builtin.scale", "builtin.scale_cage",
                                           "builtin.transform", "builtin.shear"]

private struct SettingOption {
  let title: String
  let symbol: String
  let value: String
}

private struct TransformSetting {
  let key: String
  let title: String
  let symbol: String
  let options: [SettingOption]

  static let all = [
    TransformSetting(key: "orientation", title: "Transform Orientation", symbol: "rotate.3d", options: [
      SettingOption(title: "Global", symbol: "globe", value: "global"),
      SettingOption(title: "Local", symbol: "cube", value: "local"),
      SettingOption(title: "Normal", symbol: "arrow.up.forward", value: "normal"),
      SettingOption(title: "Gimbal", symbol: "gyroscope", value: "gimbal"),
      SettingOption(title: "View", symbol: "eye", value: "view"),
      SettingOption(title: "Cursor", symbol: "scope", value: "cursor"),
      SettingOption(title: "Parent", symbol: "arrow.triangle.branch", value: "parent"),
    ]),
    TransformSetting(key: "pivot", title: "Pivot Point", symbol: "viewfinder", options: [
      SettingOption(title: "Median Point", symbol: "circle.grid.cross", value: "median"),
      SettingOption(title: "Bounding Box Center", symbol: "square.dashed", value: "bounds"),
      SettingOption(title: "3D Cursor", symbol: "scope", value: "cursor"),
      SettingOption(title: "Individual Origins", symbol: "circle.grid.2x2", value: "individual"),
      SettingOption(title: "Active Element", symbol: "record.circle", value: "active"),
    ]),
    TransformSetting(key: "snap", title: "Snapping", symbol: "magnet", options: [
      SettingOption(title: "Off", symbol: "magnet", value: "off"),
      SettingOption(title: "Increment", symbol: "grid", value: "increment"),
      SettingOption(title: "Grid", symbol: "square.grid.3x3", value: "grid"),
      SettingOption(title: "Vertex", symbol: "point.topleft.down.curvedto.point.bottomright.up", value: "vertex"),
      SettingOption(title: "Edge", symbol: "line.diagonal", value: "edge"),
      SettingOption(title: "Face", symbol: "square", value: "face"),
      SettingOption(title: "Volume", symbol: "cube.transparent", value: "volume"),
    ]),
    TransformSetting(key: "proportional", title: "Proportional Editing", symbol: "circle.circle", options: [
      SettingOption(title: "Off", symbol: "circle", value: "off"),
      SettingOption(title: "On", symbol: "largecircle.fill.circle", value: "on"),
      SettingOption(title: "Connected Only", symbol: "link.circle", value: "connected"),
      SettingOption(title: "Projected", symbol: "square.3.layers.3d", value: "projected"),
    ]),
  ]

  func image(_ option: SettingOption) -> UIImage? {
    wm_apple_ios_icon("setting.\(key).\(option.value)", option.symbol)
  }
}

/** Viewport options in the More menu, by Blender's name. */
struct ViewportToggle {
  let name: String
  let title: String
  let symbol: String

  static let all = [
    ViewportToggle(name: "overlays", title: "Overlays", symbol: "circle.dashed.inset.filled"),
    ViewportToggle(name: "xray", title: "X-Ray", symbol: "square.on.square.intersection.dashed"),
    ViewportToggle(name: "gizmos", title: "Gizmos", symbol: "move.3d"),
    ViewportToggle(name: "statistics", title: "Statistics", symbol: "chart.bar"),
    ViewportToggle(name: "wireframe", title: "Wireframe", symbol: "cube"),
  ]
}

/** Blender's icon for an image, or the symbol, as a SwiftUI image. */
private func iconImage(_ image: UIImage?) -> Image {
  image.map { Image(uiImage: $0) } ?? Image(systemName: "questionmark")
}

struct DocumentToolbar: ToolbarContent {
  @Bindable var model: ShellModel
  let shell: ShellPanels
  let commands: ShellCommands
  @Binding var stripWidth: CGFloat

  var body: some ToolbarContent {
    if model.documentsAvailable {
      /* Back to the documents is navigation, not a tool: its own glass, as the system's back
       * button. */
      ToolbarItem(placement: .topBarLeading) {
        Button("Documents", systemImage: "chevron.backward") { wm_apple_ios_show_documents(nil) }
          .accessibilityIdentifier("blender.documents")
      }
#if !os(visionOS)
      ToolbarSpacer(.fixed, placement: .topBarLeading)
#endif
    }
    ToolbarItemGroup(placement: .topBarLeading) {
      modeMenu
      Button("Add", systemImage: "plus") { shell.assets.toggle() }
        .accessibilityIdentifier("blender.assets")
    }
    if model.mode == "mesh_edit" && !model.compact {
#if os(visionOS)
      ToolbarItem(placement: .topBarLeading) { meshSelection }
#else
      ToolbarSpacer(.fixed, placement: .topBarLeading)
      ToolbarItem(placement: .topBarLeading) { meshSelection }
        .sharedBackgroundVisibility(.hidden)
#endif
    }
#if !os(visionOS)
    ToolbarSpacer(.fixed, placement: .topBarLeading)
#endif
    ToolbarItem(placement: .topBarLeading) {
      Button { Engine.operatorCall("ED_OT_undo") } label: {
        Label { Text("Undo") } icon: { iconImage(wm_apple_ios_icon("operator.ED_OT_undo", "arrow.uturn.backward")) }
      }
        .disabled(!model.canUndo)
        .accessibilityIdentifier("blender.undo")
    }
    if model.compact {
      /* Everything else in one menu, as nothing else fits beside the most used items. */
      ToolbarItem(placement: .topBarTrailing) {
        UIMenuButton(title: nil, image: UIImage(systemName: "ellipsis"), style: .plain,
                     accessibilityLabel: "More", tint: .label, deferred: false) { commands.compactMenu() }
          .frame(width: 44, height: 44)
          .background(ShareSource(commands: commands))
          .shadingPopover(model: model, shell: shell)
          .popover(isPresented: $model.toolSettingsOpen) {
            shell.panels.toolSettingsContent { model.toolSettingsOpen = false }
          }
          .accessibilityIdentifier("blender.actions")
      }
    }
    else {
      regularTrailing
    }
  }

  @ToolbarContentBuilder
  private var regularTrailing: some ToolbarContent {
    ToolbarItem(placement: .principal) { centerSettings }
    ToolbarItemGroup(placement: .topBarTrailing) {
      panelToggle("Outliner", image: BlenderIcon.image("OUTLINER"), isOn: model.hierarchyShown,
                  identifier: "blender.hierarchy") { shell.panels.toggleHierarchy() }
      panelToggle("Timeline", image: UIImage(named: "blender.time")?.withRenderingMode(.alwaysTemplate),
                  isOn: model.timelineShown, identifier: "blender.timeline") { shell.timeline.toggle() }
      panelToggle("Properties", image: BlenderIcon.image("PROPERTIES"), isOn: model.inspectorShown,
                  identifier: "blender.inspector") { shell.panels.toggleInspector() }
    }
#if !os(visionOS)
    ToolbarSpacer(.fixed, placement: .topBarTrailing)
#endif
    ToolbarItem(placement: .topBarTrailing) { shadingButton }
#if !os(visionOS)
    ToolbarSpacer(.fixed, placement: .topBarTrailing)
#endif
    ToolbarItemGroup(placement: .topBarTrailing) {
      UIMenuButton(title: nil, image: UIImage(systemName: "square.and.arrow.up"), style: .plain,
                   accessibilityLabel: "Share", tint: .label, deferred: false) { commands.shareMenu() }
        .frame(width: 44, height: 44)
        .background(ShareSource(commands: commands))
        .accessibilityIdentifier("blender.share")
      Menu("More", systemImage: "ellipsis") { moreMenu }
        .accessibilityIdentifier("blender.actions")
    }
  }

  /** More: smaller things, with viewport options showing their current state. */
  @ViewBuilder
  private var moreMenu: some View {
    Button("New…", systemImage: "doc.badge.plus") {
      if let presenter = commands.presenter { wm_apple_ios_new_document(presenter) }
    }
    Section {
      ForEach(ViewportToggle.all, id: \.name) { toggle in
        Toggle(toggle.title, systemImage: toggle.symbol, isOn: Binding(
          get: { model.viewport?.bool(toggle.name) == true },
          set: { _ in Engine.setting("viewport_toggle", toggle.name) }))
          .disabled(model.viewport == nil)
      }
    }
    Section {
      Button("Hide Interface", systemImage: "arrow.up.left.and.arrow.down.right") {
        commands.setInterfaceHidden(true)
      }
      Button("Switch to Desktop Interface", systemImage: "macwindow") { Engine.setting("interface", "desktop") }
    }
    Button("Settings", systemImage: "gearshape") { commands.settings?.present() }
  }

  /* -------------------------------------------------------------------- */
  /* Mode */

  /** The modes the active object has, as Blender's header lists them. */
  private var modeMenu: some View {
    let current = model.modes.first { $0.string("id") == model.objectMode }
    let title = current.flatMap { $0.string("label") } ?? "Object Mode"
    return Menu {
      Picker("Mode", selection: Binding(
        get: { model.objectMode },
        set: { Engine.setting("object_mode", $0) })) {
        ForEach(Array(model.modes.enumerated()), id: \.offset) { _, mode in
          let id = mode.string("id") ?? ""
          Label {
            Text(mode.string("label").flatMap { $0.isEmpty ? nil : $0 } ?? id)
          } icon: {
            iconImage(BlenderIcon.image(mode.string("icon")))
          }
          .tag(id)
        }
      }
    } label: {
      Label {
        Text(title)
      } icon: {
        iconImage(BlenderIcon.image(current?.string("icon") ?? "OBJECT_DATAMODE"))
      }
    }
    .accessibilityLabel("Mode")
    .accessibilityValue(title)
    .accessibilityIdentifier("blender.mode")
  }

  /* -------------------------------------------------------------------- */
  /* Mesh selection mode */

  private var meshSelection: some View {
    let modes = [("VERT", "Vertex Select", "circle.fill"), ("EDGE", "Edge Select", "line.diagonal"),
                 ("FACE", "Face Select", "square.fill")]
    return SegmentedControl(
      segments: modes.map { value, title, symbol in
        SegmentedControl.Segment(value: value, title: title, image: wm_apple_ios_icon("mesh.\(value)", symbol))
      },
      selection: model.meshSelectMode, accessibilityLabel: "Mesh Selection Mode")
    { Engine.setting("mesh_select_mode", $0) }
      .frame(width: 144, height: 36)
      .accessibilityIdentifier("blender.mesh-selection")
  }

  /* -------------------------------------------------------------------- */
  /* Tool settings */

  @ViewBuilder
  private var centerSettings: some View {
    if transformTools.contains(model.activeTool) {
      transformSettings
    }
    else if model.toolSettingsShown && model.toolStripRoom < 160 {
      /* No room for the strip beside the other items: all the settings in a popover. */
      Button("Tool Settings", systemImage: "slider.horizontal.3") { model.toolSettingsOpen = true }
        .labelStyle(.iconOnly)
        .frame(width: 44, height: 44)
        .panelGlass(in: Circle())
        .popover(isPresented: $model.toolSettingsOpen) {
          shell.panels.toolSettingsContent { model.toolSettingsOpen = false }
        }
        .background(ToolStripMarker { model.toolStripRoom = $0 })
        .accessibilityIdentifier("blender.tool-settings.more")
    }
    else if model.toolSettingsShown {
      toolSettings
    }
  }

  private var transformSettings: some View {
    HStack(spacing: 0) {
      ForEach(TransformSetting.all, id: \.key) { setting in
        let value = model.transformSettings[setting.key]
        let selected = setting.options.first { $0.value == value }
        Menu {
          Picker(setting.title, selection: Binding(
            get: { value ?? "" },
            set: { Engine.setting(setting.key, $0) })) {
            ForEach(setting.options, id: \.value) { option in
              Label { Text(option.title) } icon: { iconImage(setting.image(option)) }
                .tag(option.value)
            }
          }
        } label: {
          iconImage(selected.flatMap { setting.image($0) } ??
                    (setting.key == "snap" && value == "mixed" ? wm_apple_ios_icon("control.Snapping", "magnet") :
                     wm_apple_ios_icon("control.\(setting.title)", setting.symbol)))
            .frame(width: 44, height: 44)
        }
        .accessibilityLabel(setting.title)
        .accessibilityValue(selected?.title ?? (setting.key == "orientation" && value == "custom" ?
                                                  "Custom Orientation" : "Multiple or Other Elements"))
        .accessibilityIdentifier("blender.setting.\(setting.title)")
      }
    }
    .padding(.horizontal, 4)
    /* One glass group, as the bar's other groups. */
    .panelGlass(in: Capsule())
  }

  /** The tool's settings as a strip in the room the bar leaves between its other items; when they
   * do not all fit they scroll, and all of them are in a popover. */
  private var toolSettings: some View {
    /* The room less the capsule's padding, and the overflow button when it is shown. */
    let room = model.toolStripRoom - 24
    let overflow = stripWidth > room + 0.5
    return HStack(spacing: 4) {
      ScrollView(.horizontal, showsIndicators: false) {
        ToolStrip(store: shell.panels.toolStripStore)
          .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { stripWidth = $0 }
      }
      .scrollBounceBehavior(.basedOnSize, axes: .horizontal)
      .frame(width: max(1, overflow ? room - 36 : stripWidth))
      if overflow {
        Button("More Tool Settings", systemImage: "ellipsis") { model.toolSettingsOpen = true }
          .labelStyle(.iconOnly)
          .foregroundStyle(PanelStyle.tint)
          .frame(width: 32)
          .accessibilityIdentifier("blender.tool-settings.more")
      }
    }
    .popover(isPresented: $model.toolSettingsOpen) {
      shell.panels.toolSettingsContent { model.toolSettingsOpen = false }
    }
    .tint(PanelStyle.tint)
    .padding(.horizontal, 12)
    .frame(height: 44)
    /* Its own glass, as the bar's other groups have. */
    .panelGlass(in: Capsule())
    .background(ToolStripMarker { model.toolStripRoom = $0 })
  }

  /* -------------------------------------------------------------------- */
  /* Panels and shading */

  private func panelToggle(_ title: String, image: UIImage?, isOn: Bool, identifier: String,
                           toggle: @escaping () -> Void) -> some View
  {
    Toggle(isOn: Binding(get: { isOn }, set: { _ in toggle() })) {
      Label { Text(title) } icon: { iconImage(image) }
    }
    .toggleStyle(.button)
    .accessibilityIdentifier(identifier)
  }

  private var shadingButton: some View {
    let commands: [String: (Int32, String, String)] = [
      "wireframe": (20, "Wireframe", "circle.dashed"), "solid": (21, "Solid", "cube"),
      "material": (22, "Material Preview", "circle.lefthalf.filled"), "rendered": (23, "Rendered", "camera.aperture"),
    ]
    let current = commands[model.shading] ?? commands["solid"]!
    return Button {
      model.shadingOpen = true
    } label: {
      Label { Text("Viewport Shading") } icon: { iconImage(wm_apple_ios_command_icon(current.0, current.2)) }
    }
    .accessibilityValue(current.1)
    .accessibilityIdentifier("blender.shading")
    .shadingPopover(model: model, shell: shell)
  }
}

extension View {
  /** Viewport shading's popover; Blender sends its settings while it is open. */
  func shadingPopover(model: ShellModel, shell: ShellPanels) -> some View {
    @Bindable var model = model
    return popover(isPresented: $model.shadingOpen) {
      shell.panels.shadingContent { model.shadingOpen = false }
    }
    .onChange(of: model.shadingOpen) { _, shown in shell.panels.setShadingShown(shown) }
  }
}

/**
 * Measures the room the navigation bar leaves for the tool settings strip it centers: twice the
 * distance from the bar's middle to the nearest control on either side, not counting the strip.
 * SwiftUI's frames of toolbar items are each in their own hosting view, so this asks UIKit.
 */
struct ToolStripMarker: UIViewRepresentable {
  let changed: (CGFloat) -> Void

  final class Marker: UIView {
    var changed: ((CGFloat) -> Void)?
    private var pending = false
    private var lastRoom: CGFloat = 0

    override func didMoveToWindow() {
      super.didMoveToWindow()
      remeasure()
      /* The bar brings its other items in with an animation: measure again once they are in. */
      DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in self?.remeasure() }
    }

    override func layoutSubviews() {
      super.layoutSubviews()
      remeasure()
    }

    func remeasure() {
      if pending || window == nil { return }
      pending = true
      DispatchQueue.main.async { [weak self] in
        guard let self else { return }
        pending = false
        guard let room = measure(), abs(room - lastRoom) > 0.5 else { return }
        lastRoom = room
        changed?(room)
      }
    }

    private func measure() -> CGFloat? {
      var bar: UIView? = superview
      while let view = bar, !(view is UINavigationBar) { bar = view.superview }
      guard let bar else { return nil }
      /* The strip sits behind its own controls: skip what is inside it. */
      let strip = convert(bounds, to: bar).insetBy(dx: -1, dy: -1)
      let center = bar.bounds.midX
      var left: CGFloat?
      var right: CGFloat?
      var pending: [UIView] = [bar]
      while let view = pending.popLast() {
        if view.isHidden || view.alpha < 0.01 { continue }
        if view !== bar, isDescendant(of: view) {
          if view is UIControl { continue }
          pending.append(contentsOf: view.subviews)
          continue
        }
        let other = view.convert(view.bounds, to: bar)
        if strip.contains(other) { continue }
        if view is UIControl && other.width < bar.bounds.width / 2 {
          if other.midX < center { left = max(left ?? other.maxX, other.maxX) }
          else { right = min(right ?? other.minX, other.minX) }
          continue
        }
        pending.append(contentsOf: view.subviews)
      }
      /* Until the items on both sides are in, keep the last room: a strip measured too wide makes
       * the bar give up centering it. */
      guard let left, let right else { return nil }
      return max(80, 2 * min(center - left, right - center) - 32)
    }
  }

  func makeUIView(context: Context) -> Marker {
    let marker = Marker()
    marker.isUserInteractionEnabled = false
    marker.changed = changed
    return marker
  }

  func updateUIView(_ marker: Marker, context: Context) {
    marker.changed = changed
    marker.remeasure()
    /* The bar's items change with an animation: measure again once they are in. */
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak marker] in marker?.remeasure() }
  }
}

/** Remembers the Share button's view, which the share sheet points at. */
private struct ShareSource: UIViewRepresentable {
  let commands: ShellCommands

  func makeUIView(context: Context) -> UIView {
    let view = UIView()
    view.isUserInteractionEnabled = false
    commands.shareSource = view
    return view
  }

  func updateUIView(_ view: UIView, context: Context) {}
}

/** \} */
