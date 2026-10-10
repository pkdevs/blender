/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The iPad's Settings: the keyboard shortcuts, how Apple Pencil and fingers work, and which
 * controls the interface shows. Pencil and interface settings belong to the iPad and are kept in
 * its user defaults; the keymap is Blender's preference.
 */

import SwiftUI
import UIKit

/* -------------------------------------------------------------------- */
/** \name Model
 * \{ */

enum TouchInput: String, CaseIterable, Identifiable {
  case pencilOnly
  case pencilAndFingers
  var id: String { rawValue }
}

@MainActor
final class SettingsModel: ObservableObject {
  static let fingerUsesToolKey = "BlenderFingerUsesTool"
  static let showQuickActionsKey = "BlenderShowQuickActions"

  /** Blender's active keymap preset, from the toolbar snapshot. */
  @Published var keymap = "Industry_Compatible"
  @Published var touchInput: TouchInput {
    didSet {
      guard touchInput != oldValue else { return }
      UserDefaults.standard.set(touchInput == .pencilAndFingers, forKey: Self.fingerUsesToolKey)
      fingerUsesToolChanged?(touchInput == .pencilAndFingers)
    }
  }
  @Published var showQuickActions: Bool {
    didSet {
      guard showQuickActions != oldValue else { return }
      UserDefaults.standard.set(showQuickActions, forKey: Self.showQuickActionsKey)
      showQuickActionsChanged?(showQuickActions)
    }
  }

  var fingerUsesToolChanged: ((Bool) -> Void)?
  var showQuickActionsChanged: ((Bool) -> Void)?
  var done: (() -> Void)?

  init() {
    let defaults = UserDefaults.standard
    touchInput = defaults.bool(forKey: Self.fingerUsesToolKey) ? .pencilAndFingers : .pencilOnly
    showQuickActions = defaults.object(forKey: Self.showQuickActionsKey) as? Bool ?? true
  }

  func setKeymap(_ name: String) {
    guard name != keymap else { return }
    keymap = name
    Engine.setting("keymap", name)
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name View
 * \{ */

/** A choice with an explanation below its title, as in the system's Settings. */
private struct ChoiceRow: View {
  let title: String
  let detail: String
  let symbol: String
  let selected: Bool
  let action: () -> Void

  var body: some View {
    Button(action: action) {
      HStack(alignment: .firstTextBaseline, spacing: 12) {
        Image(systemName: symbol)
          .frame(width: 28)
          .foregroundStyle(.tint)
        VStack(alignment: .leading, spacing: 2) {
          Text(title).foregroundStyle(.primary)
          Text(detail).font(.footnote).foregroundStyle(.secondary)
        }
        Spacer(minLength: 8)
        if selected {
          Image(systemName: "checkmark")
            .fontWeight(.semibold)
            .foregroundStyle(.tint)
        }
      }
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .accessibilityElement(children: .combine)
    .accessibilityAddTraits(selected ? .isSelected : [])
  }
}

struct SettingsView: View {
  @ObservedObject var model: SettingsModel

  var body: some View {
    NavigationStack {
      Form {
        Section {
          ChoiceRow(title: "Apple Pencil Only",
                    detail: "Fingers move the view. Apple Pencil uses the active tool.",
                    symbol: "applepencil",
                    selected: model.touchInput == .pencilOnly) { model.touchInput = .pencilOnly }
          ChoiceRow(title: "Pencil and Fingers",
                    detail: "One finger also uses the active tool. Two fingers move the view.",
                    symbol: "hand.point.up.left",
                    selected: model.touchInput == .pencilAndFingers) {
            model.touchInput = .pencilAndFingers
          }
        } header: {
          Text("Apple Pencil & Touch")
        }

        Section {
          ChoiceRow(title: "Industry Compatible",
                    detail: "Shortcuts familiar from other 3D apps.",
                    symbol: "keyboard",
                    selected: model.keymap == "Industry_Compatible") {
            model.setKeymap("Industry_Compatible")
          }
          ChoiceRow(title: "Blender",
                    detail: "Blender's own shortcuts, as on a desktop computer.",
                    symbol: "keyboard.badge.ellipsis",
                    selected: model.keymap == "Blender") { model.setKeymap("Blender") }
        } header: {
          Text("Keyboard Shortcuts")
        } footer: {
          Text("Used with a connected keyboard. Touch and Pencil gestures work the same with either.")
        }

        Section {
          Toggle("Quick Actions", isOn: $model.showQuickActions)
        } header: {
          Text("Interface")
        } footer: {
          Text("Buttons for common actions on the selection, above the bottom of the viewport.")
        }
      }
      .navigationTitle("Settings")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        ToolbarItem(placement: .confirmationAction) {
          Button("Done") { model.done?() }
        }
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Controller
 * \{ */

@MainActor
@objc(GHOSTIOSSwiftSettings)
public final class SwiftSettings: NSObject, UIAdaptivePresentationControllerDelegate {
  private weak var presenter: UIViewController?
  private var sheet: UIViewController?
  private let model = SettingsModel()

  @objc(initWithPresenter:)
  public init(presenter: UIViewController) {
    self.presenter = presenter
    super.init()
    model.done = { [weak self] in self?.close() }
  }

  /** Whether one finger uses the active tool, as last chosen. */
  @objc public static var fingerUsesTool: Bool {
    UserDefaults.standard.bool(forKey: SettingsModel.fingerUsesToolKey)
  }

  /** Whether the quick action pills are shown, as last chosen. */
  @objc public static var showQuickActions: Bool {
    UserDefaults.standard.object(forKey: SettingsModel.showQuickActionsKey) as? Bool ?? true
  }

  @objc public var fingerUsesToolChanged: ((Bool) -> Void)? {
    get { model.fingerUsesToolChanged }
    set { model.fingerUsesToolChanged = newValue }
  }

  @objc public var showQuickActionsChanged: ((Bool) -> Void)? {
    get { model.showQuickActionsChanged }
    set { model.showQuickActionsChanged = newValue }
  }

  @objc public func present() {
    guard sheet == nil, let presenter, presenter.presentedViewController == nil else { return }
    let controller = UIHostingController(rootView: SettingsView(model: model))
    controller.modalPresentationStyle = .formSheet
    controller.preferredContentSize = CGSize(width: 540, height: 620)
    controller.presentationController?.delegate = self
    sheet = controller
    presenter.present(controller, animated: true)
  }

  /** The `preferences` entry of the toolbar snapshot. */
  @objc(updatePreferences:)
  public func updatePreferences(_ preferences: Any?) {
    guard let preferences = preferences as? JSON, let keymap = preferences.string("keymap"),
          keymap != model.keymap else { return }
    model.keymap = keymap
  }

  public func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
    sheet = nil
  }

  private func close() {
    let sheet = self.sheet
    self.sheet = nil
    sheet?.dismiss(animated: true)
  }
}

/** \} */
