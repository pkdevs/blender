/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The asset pane along the bottom of the viewport: the assets Blender publishes, by type and
 * catalog, searchable. Tapping one adds it; dragging one into the viewport runs Blender's own asset
 * drag and drop through the engine inbox. Answers the `GHOSTIOSAssets` protocol's messages; the
 * view controller finds it by name, see `GHOST_WindowIOS.mm`.
 */

import SwiftUI
import UIKit

/* Matches #IOSAssetDragPhase in `wm_apple_ios_ui.cc`. */
private enum AssetDragPhase: Int32 {
  case begin = 0, move = 1, drop = 2, cancel = 3, add = 4
}

private let assetsHeight: CGFloat = 212
private let assetsCompactHeight: CGFloat = 184
private let assetsMaxWidth: CGFloat = 1600
private let assetTile: CGFloat = 96
/* The pane opens on the Starter library's basic shapes. */
private let startType = "Object"
private let startCategory = "Primitives"

/** Data-block types Blender publishes, with their Blender icon and a readable plural. */
private let assetTypes: [(type: String, icon: String, plural: String)] = [
  ("Object", "OBJECT_DATA", "Objects"), ("Collection", "OUTLINER_COLLECTION", "Collections"),
  ("Material", "MATERIAL", "Materials"), ("World", "WORLD", "Worlds"), ("Modifier", "MODIFIER", "Modifiers"),
]

private func assetTypeIcon(_ type: String?) -> UIImage? {
  if let entry = assetTypes.first(where: { $0.type == type }) { return BlenderIcon.image(entry.icon) }
  return UIImage(systemName: "shippingbox")
}

/** What tapping an asset does, matching where Blender's drop boxes accept it. */
private func assetAddTitle(_ type: String?) -> String {
  switch type {
    case "Material": return "Apply to Active Object"
    case "Modifier": return "Add to Active Object"
    case "World": return "Use as World"
    default: return "Add at 3D Cursor"
  }
}

/** Where a drop lands, shown in the drag preview over the viewport. */
private func assetDropHint(_ type: String?) -> String {
  type == "Material" || type == "Modifier" ? "Drop on an object" : "Drop to place"
}

/** Catalogs are browsed by name, merging libraries. */
private func assetCategory(_ item: JSON) -> String? {
  guard let catalog = item.string("catalog"), !catalog.isEmpty else { return nil }
  return (catalog as NSString).lastPathComponent
}

@MainActor
final class AssetsStore: ObservableObject {
  @Published var items: [JSON] = []
  @Published var loading = true
  @Published var type = startType
  @Published var category: String? = startCategory
  @Published var query = ""
  var close: () -> Void = {}
  /** The asset being dragged out of the pane, for the viewport's drop interaction. */
  var dragged: JSON?

  private var terms: [String] { query.split(whereSeparator: \.isWhitespace).map(String.init) }

  var types: [String] {
    let present = Set(items.compactMap { $0.string("type") })
    return assetTypes.map(\.type).filter(present.contains)
  }

  var categories: [String] {
    var names = Array(Set(items.filter { $0.string("type") == type }.compactMap(assetCategory)))
    names.sort { $0.localizedStandardCompare($1) == .orderedAscending }
    /* The basic shapes lead, as the place to start. */
    if let index = names.firstIndex(of: startCategory) { names.insert(names.remove(at: index), at: 0) }
    return names
  }

  /** A search looks through every category of the type. */
  var shownCategory: String? { terms.isEmpty ? category : nil }

  var filtered: [JSON] {
    let terms = self.terms
    let category = shownCategory
    return items.filter { item in
      guard item.string("type") == type, category == nil || assetCategory(item) == category else { return false }
      /* Every word must match the name, tags, catalog, description or author, like the Asset
       * Browser's filter. */
      let haystack = ([item.string("name"), item.string("catalog"), item.string("description"),
                       item.string("author"), item.string("type")].compactMap { $0 } + item.strings("tags"))
        .joined(separator: "\n")
      return terms.allSatisfy { haystack.range(of: $0, options: [.caseInsensitive, .diacriticInsensitive]) != nil }
    }
  }

  func update(_ snapshot: JSON) {
    loading = snapshot.bool("loading")
    let next = snapshot.array("items")
    if !(next as NSArray).isEqual(items as NSArray) { items = next }
    /* Nothing of that type: show the first type there is. */
    if !types.isEmpty && !types.contains(type) {
      type = types[0]
      category = nil
    }
    if let category, !items.isEmpty && !categories.contains(category) { self.category = nil }
  }

  func add(_ item: JSON) {
    WM_apple_ios_ui_asset_drag(AssetDragPhase.add.rawValue, item.string("id") ?? "", 0, 0)
    Haptics.impact()
  }
}

struct AssetsPane: View {
  @ObservedObject var store: AssetsStore

  var body: some View {
    VStack(spacing: 4) {
      HStack(spacing: 8) {
        typeMenu
        if !store.categories.isEmpty { categoryMenu }
        AssetSearchField(text: $store.query)
          .frame(maxWidth: .infinity)
          .frame(height: 28)
        Button(action: store.close) {
          Image(systemName: "xmark")
            .font(.system(size: 20, weight: .medium))
            .foregroundStyle(.primary)
            .frame(width: 36, height: 36)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Hide Assets")
      }
      .frame(height: 40)
      .padding(.leading, 14)
      .padding(.trailing, 12)
      .padding(.top, 10)
      grid
        .padding(.bottom, 8)
    }
  }

  private var typeMenu: some View {
    Menu {
      Picker("Type", selection: Binding(get: { store.type }, set: { store.type = $0; store.category = nil })) {
        ForEach(store.types, id: \.self) { type in
          Label {
            Text(assetTypes.first { $0.type == type }?.plural ?? type)
          } icon: {
            if let image = assetTypeIcon(type) { Image(uiImage: image) }
          }
          .tag(type)
        }
      }
      .pickerStyle(.inline)
    } label: {
      menuLabel(store.type)
    }
    .buttonStyle(.glass)
    .accessibilityIdentifier("blender.assets.type")
  }

  private var categoryMenu: some View {
    Menu {
      Picker("Category", selection: Binding(get: { store.shownCategory ?? "" },
                                            set: { store.category = $0.isEmpty ? nil : $0 })) {
        Text("All Categories").tag("")
        Section {
          ForEach(store.categories, id: \.self) { Text($0).tag($0) }
        }
      }
      .pickerStyle(.inline)
    } label: {
      menuLabel(store.shownCategory ?? "All Categories")
    }
    .buttonStyle(.glass)
    .accessibilityIdentifier("blender.assets.category")
  }

  private func menuLabel(_ title: String) -> some View {
    HStack(spacing: 4) {
      Text(title).lineLimit(1)
      Image(systemName: "chevron.up.chevron.down").font(.caption)
    }
    .frame(maxWidth: 200)
    .fixedSize()
  }

  @ViewBuilder
  private var grid: some View {
    let items = store.filtered
    ScrollView(.horizontal, showsIndicators: false) {
      LazyHStack(spacing: 10) {
        ForEach(Array(items.enumerated()), id: \.element.assetID) { _, item in
          AssetTile(store: store, item: item)
        }
      }
      .padding(.horizontal, 14)
      .frame(maxHeight: .infinity, alignment: .top)
    }
    .scrollDismissesKeyboard(.immediately)
    .overlay {
      if items.isEmpty {
        Text(store.loading && store.items.isEmpty ? "Loading Assets…" :
               store.query.isEmpty ? "No Assets" : "No Results for \u{201C}\(store.query)\u{201D}")
          .font(.subheadline)
          .foregroundStyle(.secondary)
          .multilineTextAlignment(.center)
      }
    }
    .accessibilityIdentifier("blender.assets.grid")
  }
}

private extension Dictionary where Key == String, Value == Any {
  var assetID: String { string("id") ?? "" }
}

struct AssetTile: View {
  @ObservedObject var store: AssetsStore
  let item: JSON

  var body: some View {
    Button {
      store.add(item)
    } label: {
      VStack(spacing: 6) {
        thumbnail
        Text(item.string("name") ?? "")
          .font(.caption)
          .lineLimit(1)
          .truncationMode(.middle)
          .foregroundStyle(.primary)
          .frame(width: assetTile + 8)
      }
    }
    .buttonStyle(AssetTileStyle())
    .accessibilityLabel(item.string("name") ?? "")
    .accessibilityHint(assetAddTitle(item.string("type")))
    .accessibilityIdentifier("blender.asset." + (item.string("name") ?? ""))
    .contextMenu { menu }
    .onDrag {
      store.dragged = item
      return NSItemProvider(object: (item.string("name") ?? "") as NSString)
    } preview: {
      thumbnail
    }
  }

  private var thumbnail: some View {
    Group {
      if let image = PreviewCache.image(item.string("preview")) {
        Image(uiImage: image).resizable().scaledToFill()
      }
      else if let icon = assetTypeIcon(item.string("type")) {
        Image(uiImage: icon).foregroundStyle(.secondary)
      }
    }
    .frame(width: assetTile, height: assetTile)
    .background(PanelStyle.field)
    .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
  }

  @ViewBuilder
  private var menu: some View {
    let subtitle = (item.string("catalog") ?? "").isEmpty ? item.string("library") ?? "" :
      "\(item.string("library") ?? "") · \(item.string("catalog") ?? "")"
    if let description = item.string("description"), !description.isEmpty { Text(description) }
    Section(subtitle) {
      Button { store.add(item) } label: { Label(assetAddTitle(item.string("type")), systemImage: "plus") }
      if let category = assetCategory(item) {
        Button {
          store.query = ""
          store.type = item.string("type") ?? store.type
          store.category = category
        } label: {
          Label("Show \(category)", systemImage: "square.grid.2x2")
        }
      }
    }
  }
}

private struct AssetTileStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label.opacity(configuration.isPressed ? 0.6 : 1)
  }
}

/* -------------------------------------------------------------------- */
/** \name Pane
 * \{ */

@MainActor
@objc(GHOSTIOSSwiftAssets)
public final class SwiftAssets: NSObject, UIDropInteractionDelegate {
  private weak var overlay: UIView?
  private let store = AssetsStore()
  private let panel: UIVisualEffectView
  private let host: UIHostingController<AssetsPane>
  @objc public let button: UIBarButtonItem
  @objc public private(set) var visible = false
  @objc public private(set) var dropInteraction: UIDropInteraction!
  @objc public var clientPoint: ((CGPoint) -> CGPoint)?
  @objc public var visibilityDidChange: (() -> Void)?

  private var dragging = false
  private var dropped = false
  private var overViewport = false
  private var dropPoint = CGPoint.zero

  @objc(initWithOverlay:)
  public init(overlay: UIView) {
    self.overlay = overlay
    /* Adding things to the scene starts here. */
    button = UIBarButtonItem(image: UIImage(systemName: "plus"), style: .plain, target: nil, action: nil)
    panel = UIVisualEffectView(effect: UIGlassEffect(style: .regular))
    host = UIHostingController(rootView: AssetsPane(store: store))
    super.init()
    button.target = self
    button.action = #selector(toggle(_:))
    button.title = "Add"
    button.accessibilityLabel = "Add"
    button.accessibilityIdentifier = "blender.assets"
    panel.accessibilityIdentifier = "blender.assets.panel"
    panel.layer.cornerRadius = 28
    panel.layer.cornerCurve = .continuous
    panel.clipsToBounds = true
    panel.isHidden = true
    panel.alpha = 0
    host.view.backgroundColor = .clear
    host.sizingOptions = []
    host.safeAreaRegions = []
    host.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    panel.contentView.addSubview(host.view)
    overlay.addSubview(panel)
    dropInteraction = UIDropInteraction(delegate: self)
    store.close = { [weak self] in self?.toggle(nil) }
  }

  @objc public var view: UIView { panel }

  @objc public var height: CGFloat {
    overlay?.traitCollection.horizontalSizeClass == .compact ? assetsCompactHeight : assetsHeight
  }

  @objc(updateSnapshot:)
  public func updateSnapshot(_ snapshot: Any?) {
    /* Blender's JSON may hold null, which arrives as NSNull. */
    guard let snapshot = snapshot as? JSON else { return }
    store.update(snapshot)
  }

  @objc(toggle:)
  public func toggle(_ sender: Any?) {
    visible.toggle()
    button.isSelected = visible
    Engine.setting("assets", visible ? "show" : "hide")
    if !visible { panel.endEditing(true) }
    let reduceMotion = UIAccessibility.isReduceMotionEnabled
    let hidden = reduceMotion ? .identity : CGAffineTransform(translationX: 0, y: 18).scaledBy(x: 0.98, y: 0.98)
    panel.isUserInteractionEnabled = visible
    if visible {
      overlay?.bringSubviewToFront(panel)
      if panel.isHidden {
        panel.alpha = 0
        panel.transform = hidden
      }
      panel.isHidden = false
    }
    let shown = visible
    UIView.animate(withDuration: reduceMotion ? 0 : 0.3, delay: 0, usingSpringWithDamping: 0.9,
                   initialSpringVelocity: 0, options: [.beginFromCurrentState, .allowUserInteraction])
    {
      self.panel.alpha = shown ? 1 : 0
      self.panel.transform = shown ? .identity : hidden
    } completion: { finished in
      if finished && self.panel.alpha == 0 { self.panel.isHidden = true }
    }
    visibilityDidChange?()
  }

  @objc(layoutInRect:)
  public func layout(in rect: CGRect) {
    let width = min(assetsMaxWidth, max(0, rect.width))
    /* Frame is undefined while transformed; layout must not disturb the presentation animation. */
    panel.bounds = CGRect(x: 0, y: 0, width: width, height: height)
    panel.center = CGPoint(x: rect.midX, y: rect.maxY - height / 2)
    host.view.frame = panel.contentView.bounds
  }

  /* -------------------------------------------------------------------- */
  /** \name Dropping into the viewport
   * \{ */

  private func asset(_ session: UIDropSession) -> JSON? {
    guard session.localDragSession != nil, session.items.count == 1, let item = store.dragged,
          !(item.string("id") ?? "").isEmpty
    else { return nil }
    return item
  }

  /** Whether the drag is over the viewport rather than this pane or other native controls. */
  private func isOverViewport(_ session: UIDropSession) -> Bool {
    guard let view = dropInteraction.view else { return false }
    var hit = view.hitTest(session.location(in: view), with: nil)
    while let current = hit {
      if current === panel || current is UIControl { return false }
      hit = current.superview
    }
    return true
  }

  private func sendDrag(_ phase: AssetDragPhase, _ session: UIDropSession) {
    guard let view = dropInteraction.view else { return }
    let point = session.location(in: view)
    let client = clientPoint?(point) ?? point
    WM_apple_ios_ui_asset_drag(phase.rawValue, asset(session)?.string("id") ?? "", Int32(client.x.rounded()),
                               Int32(client.y.rounded()))
  }

  /** Over the viewport the lifted thumbnail becomes a compact pill beside the finger, so the
   * surface under it, and Blender's preview of where the asset lands, stay visible. */
  private func pillPreview(_ item: JSON) -> UIDragPreview {
    let pill = UIHostingController(rootView: AssetDropPill(item: item))
    pill.view.backgroundColor = .clear
    let size = pill.sizeThatFits(in: CGSize(width: 320, height: 56))
    let gap: CGFloat = 20
    /* The preview's center sits under the finger: place the pill in the upper left quarter. */
    let container = UIView(frame: CGRect(x: 0, y: 0, width: 2 * (size.width + gap), height: 2 * (56 + gap)))
    container.backgroundColor = .clear
    container.overrideUserInterfaceStyle = panel.traitCollection.userInterfaceStyle
    pill.view.frame = CGRect(x: 0, y: 0, width: size.width, height: 56)
    container.addSubview(pill.view)
    let parameters = UIDragPreviewParameters()
    let shape = UIBezierPath(roundedRect: pill.view.frame, cornerRadius: 28)
    parameters.visiblePath = shape
    parameters.shadowPath = shape
    parameters.backgroundColor = .clear
    return UIDragPreview(view: container, parameters: parameters)
  }

  /** The thumbnail as it was lifted from the pane. */
  private func thumbnailPreview(_ item: JSON) -> UIDragPreview {
    let image = UIImageView(image: PreviewCache.image(item.string("preview")) ?? assetTypeIcon(item.string("type")))
    image.frame = CGRect(x: 0, y: 0, width: assetTile, height: assetTile)
    image.contentMode = PreviewCache.image(item.string("preview")) != nil ? .scaleAspectFill : .center
    image.backgroundColor = .tertiarySystemFill
    let parameters = UIDragPreviewParameters()
    parameters.visiblePath = UIBezierPath(roundedRect: image.bounds, cornerRadius: 14)
    parameters.backgroundColor = .clear
    return UIDragPreview(view: image, parameters: parameters)
  }

  /** Crossing between the pane and the viewport: swap the preview, let the scene show through
   * the pane, and give a light tick so the change is felt. */
  private func setOverViewport(_ over: Bool, _ session: UIDropSession) {
    guard over != overViewport else { return }
    overViewport = over
    if let item = asset(session) {
      let preview = over ? pillPreview(item) : thumbnailPreview(item)
      for drag in session.localDragSession?.items ?? [] { drag.previewProvider = { preview } }
    }
    if over { Haptics.selection() }
    UIView.animate(withDuration: UIAccessibility.isReduceMotionEnabled ? 0 : 0.2, delay: 0,
                   options: [.beginFromCurrentState, .allowUserInteraction]) {
      self.panel.alpha = over ? 0.35 : 1
    }
  }

  public func dropInteraction(_ interaction: UIDropInteraction, canHandle session: UIDropSession) -> Bool {
    asset(session) != nil
  }

  public func dropInteraction(_ interaction: UIDropInteraction, sessionDidEnter session: UIDropSession) {
    dropped = false
  }

  public func dropInteraction(_ interaction: UIDropInteraction,
                              sessionDidUpdate session: UIDropSession) -> UIDropProposal
  {
    let over = isOverViewport(session)
    setOverViewport(over, session)
    if over && !dragging {
      /* Start Blender's drag where the asset enters the viewport, so it previews from there. */
      dragging = true
      sendDrag(.begin, session)
    }
    else if over {
      sendDrag(.move, session)
    }
    else if dragging {
      /* Back over the pane: stop previewing in the viewport. */
      dragging = false
      WM_apple_ios_ui_asset_drag(AssetDragPhase.cancel.rawValue, nil, 0, 0)
    }
    return UIDropProposal(operation: over ? .copy : .cancel)
  }

  public func dropInteraction(_ interaction: UIDropInteraction, performDrop session: UIDropSession) {
    guard dragging else { return }
    dropped = true
    dragging = false
    if let view = interaction.view { dropPoint = session.location(in: view) }
    sendDrag(.drop, session)
    Haptics.impact(.medium)
  }

  public func dropInteraction(_ interaction: UIDropInteraction, sessionDidExit session: UIDropSession) {
    setOverViewport(false, session)
    if dragging {
      dragging = false
      WM_apple_ios_ui_asset_drag(AssetDragPhase.cancel.rawValue, nil, 0, 0)
    }
  }

  public func dropInteraction(_ interaction: UIDropInteraction, sessionDidEnd session: UIDropSession) {
    if dragging && !dropped { WM_apple_ios_ui_asset_drag(AssetDragPhase.cancel.rawValue, nil, 0, 0) }
    dragging = false
    overViewport = false
    store.dragged = nil
    UIView.animate(withDuration: UIAccessibility.isReduceMotionEnabled ? 0 : 0.25) { self.panel.alpha = 1 }
  }

  public func dropInteraction(_ interaction: UIDropInteraction, previewForDropping item: UIDragItem,
                              withDefault defaultPreview: UITargetedDragPreview) -> UITargetedDragPreview?
  {
    /* The asset lands in the scene: shrink the thumbnail into the drop point. */
    guard let view = interaction.view else { return defaultPreview }
    let target = UIDragPreviewTarget(container: view, center: dropPoint,
                                     transform: CGAffineTransform(scaleX: 0.05, y: 0.05))
    return defaultPreview.retargetedPreview(with: target)
  }

  /** \} */
}

/** The drag preview over the viewport: thumbnail, name and where the drop lands. */
private struct AssetDropPill: View {
  let item: JSON

  var body: some View {
    HStack(spacing: 4) {
      Group {
        if let image = PreviewCache.image(item.string("preview")) {
          Image(uiImage: image).resizable().scaledToFill()
        }
        else if let icon = assetTypeIcon(item.string("type")) {
          Image(uiImage: icon).foregroundStyle(.secondary)
        }
      }
      .frame(width: 44, height: 44)
      .background(Color(uiColor: .tertiarySystemFill))
      .clipShape(Circle())
      .padding(6)
      VStack(alignment: .leading, spacing: 0) {
        Text(item.string("name") ?? "").font(.headline).lineLimit(1)
        Text(assetDropHint(item.string("type"))).font(.caption).foregroundStyle(.secondary)
      }
      .padding(.trailing, 16)
    }
    .frame(height: 56)
    .frame(maxWidth: 320)
    .background(Color(uiColor: .secondarySystemBackground), in: Capsule())
  }
}

/** \} */

/** The system's search field, as the asset shelf has always used. */
struct AssetSearchField: UIViewRepresentable {
  @Binding var text: String

  func makeUIView(context: Context) -> UISearchTextField {
    let field = UISearchTextField()
    field.placeholder = "Search Assets"
    field.accessibilityIdentifier = "blender.assets.search"
    field.returnKeyType = .search
    field.autocorrectionType = .no
    field.delegate = context.coordinator
    field.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let coordinator = context.coordinator
    field.addAction(UIAction { [weak field] _ in
      coordinator.text.wrappedValue = field?.text ?? ""
    }, for: .editingChanged)
    return field
  }

  func updateUIView(_ field: UISearchTextField, context: Context) {
    context.coordinator.text = $text
    if field.text != text { field.text = text }
  }

  func makeCoordinator() -> Coordinator { Coordinator(text: $text) }

  final class Coordinator: NSObject, UITextFieldDelegate {
    var text: Binding<String>

    init(text: Binding<String>) { self.text = text }

    func textFieldShouldReturn(_ textField: UITextField) -> Bool {
      textField.resignFirstResponder()
      return true
    }
  }
}
