/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Blender's confirmation dialogs with options, such as Save Changes with its toggles, as a form
 * sheet. Dialogs without options are system alerts. Created by name from `wm_apple_ios_alerts.mm`,
 * which keeps the queue and reports the choice to Blender.
 */

import SwiftUI
import UIKit

struct DialogSheet: View {
  let title: String
  let message: String
  let toggles: [String]
  @State var selected: [Bool]
  let actions: [String]
  /** 0: default, 1: cancel, 2: destructive. */
  let roles: [Int]
  let preferred: Int
  let choose: (Int, UInt64) -> Void

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 20) {
        Text(title).font(.title2)
        Text(message).font(.body)
        ForEach(Array(toggles.enumerated()), id: \.offset) { index, name in
          Toggle(name, isOn: $selected[index])
            .tint(Color(uiColor: .systemGreen))
        }
        ForEach(Array(actions.enumerated()), id: \.offset) { index, name in
          let button = Button {
            choose(index, mask)
          } label: {
            Text(name)
              .foregroundStyle(roles[index] == 2 ? Color.red : index == preferred ? .white : .accentColor)
              .frame(maxWidth: .infinity, minHeight: 34)
          }
          if index == preferred { button.buttonStyle(.borderedProminent) }
          else { button.buttonStyle(.bordered) }
        }
      }
      .padding(24)
    }
    .background(Color(uiColor: .systemBackground))
    .accessibilityIdentifier("blender.nativeDialog")
  }

  private var mask: UInt64 {
    var result: UInt64 = 0
    for (index, on) in selected.enumerated() where on { result |= 1 << UInt64(index) }
    return result
  }
}

@MainActor
@objc(WMIOSSwiftDialogSheet)
final class DialogSheetController: UIHostingController<DialogSheet> {
  @objc init(title: String, message: String, toggles: [String], selected: [NSNumber],
                    actions: [String], roles: [NSNumber], preferred: Int,
                    choose: @escaping (Int, UInt64) -> Void)
  {
    super.init(rootView: DialogSheet(title: title, message: message, toggles: toggles,
                                     selected: selected.map(\.boolValue), actions: actions,
                                     roles: roles.map(\.intValue), preferred: preferred, choose: choose))
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }
}
