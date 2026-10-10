/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Choose a template for a new document: Blender's templates and the user's own, as a searchable
 * grid of real previews, with asset credits a tap away. Created by name from
 * `GHOST_IOSLauncher.mm`, which provides the templates and keeps the document handling.
 */

import SwiftUI
import UIKit

@MainActor
final class TemplateStore: ObservableObject {
  @Published var templates: [JSON] = []
  @Published var query = ""
  let load: () -> [NSDictionary]
  let manage: (NSDictionary, String, String?, @escaping (Bool) -> Void) -> Void
  let finish: (String?) -> Void

  init(load: @escaping () -> [NSDictionary],
       manage: @escaping (NSDictionary, String, String?, @escaping (Bool) -> Void) -> Void,
       finish: @escaping (String?) -> Void)
  {
    self.load = load
    self.manage = manage
    self.finish = finish
    reload()
  }

  func reload() {
    templates = load().compactMap { $0 as? JSON }
  }

  var sections: [(title: String, items: [JSON])] {
    let query = self.query.trimmingCharacters(in: .whitespacesAndNewlines)
    let shown = templates.filter {
      query.isEmpty || ($0.string("title") ?? "").range(of: query, options: [.caseInsensitive, .diacriticInsensitive]) != nil
    }
    var result: [(title: String, items: [JSON])] = []
    let bundled = shown.filter { !$0.bool("personal") }
    let personal = shown.filter { $0.bool("personal") }
    if !bundled.isEmpty { result.append(("Blender", bundled)) }
    if !personal.isEmpty { result.append(("My Templates", personal)) }
    return result
  }
}

struct TemplatePicker: View {
  @ObservedObject var store: TemplateStore
  @State private var renaming: JSON?
  @State private var name = ""
  @State private var deleting: JSON?
  @State private var failed = false

  var body: some View {
    NavigationStack {
      GeometryReader { geometry in
        let largeText = UIApplication.shared.preferredContentSizeCategory.isAccessibilityCategory
        let columns = largeText || geometry.size.width < 360 ? 1 : geometry.size.width > 700 ? 3 : 2
        ScrollView {
          LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 16, alignment: .top), count: columns),
                    alignment: .leading, spacing: 16, pinnedViews: [])
          {
            ForEach(store.sections, id: \.title) { section in
              Section {
                ForEach(Array(section.items.enumerated()), id: \.offset) { _, item in
                  TemplateCell(item: item) { store.finish(item.string("id")) }
                    .contextMenu { menu(item) }
                }
              } header: {
                Text(section.title)
                  .font(.headline)
                  .frame(maxWidth: .infinity, alignment: .leading)
                  .padding(.top, 8)
                  .padding(.bottom, 4)
              }
            }
          }
          .padding(.horizontal, 20)
          .padding(.bottom, 20)
        }
        .overlay {
          if store.sections.isEmpty { ContentUnavailableView.search(text: store.query) }
        }
      }
      .background(Color(uiColor: .systemBackground))
      .navigationTitle("Choose a Template")
      .navigationBarTitleDisplayMode(.inline)
      .searchable(text: $store.query, placement: .toolbar, prompt: "Search Templates")
      .toolbar {
        ToolbarItem(placement: .cancellationAction) {
          Button(role: .close) { store.finish(nil) }
        }
      }
      .navigationDestination(for: CreditPage.self) { page in TemplateCredits(credit: page.credit) }
    }
    .alert("Rename Template", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
      TextField("Name", text: $name)
      Button("Cancel", role: .cancel) { renaming = nil }
      Button("Rename") {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if let entry = renaming, !trimmed.isEmpty { manage(entry, "rename", trimmed) }
        renaming = nil
      }
    }
    .alert("Delete Template?", isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } })) {
      Button("Cancel", role: .cancel) { deleting = nil }
      Button("Delete", role: .destructive) {
        if let entry = deleting { manage(entry, "delete", nil) }
        deleting = nil
      }
    } message: {
      Text("Documents created from this template will not be deleted.")
    }
    .alert("Could Not Update Template", isPresented: $failed) {
      Button("OK") {}
    } message: {
      Text("Check available storage and try again.")
    }
  }

  @ViewBuilder
  private func menu(_ entry: JSON) -> some View {
    Button { manage(entry, "duplicate", nil) } label: { Label("Duplicate", systemImage: "plus.square.on.square") }
    if entry.bool("personal") {
      Button {
        name = entry.string("title") ?? ""
        renaming = entry
      } label: {
        Label("Rename", systemImage: "pencil")
      }
      Button(role: .destructive) { deleting = entry } label: { Label("Delete", systemImage: "trash") }
    }
  }

  private func manage(_ entry: JSON, _ action: String, _ title: String?) {
    store.manage(entry as NSDictionary, action, title) { success in
      if success { store.reload() }
      else { failed = true }
    }
  }
}

struct CreditPage: Hashable {
  let id: String
  let credit: JSON

  static func == (a: CreditPage, b: CreditPage) -> Bool { a.id == b.id }
  func hash(into hasher: inout Hasher) { hasher.combine(id) }
}

/** A real preview of the starting scene, with its title and asset credit. */
struct TemplateCell: View {
  let item: JSON
  let choose: () -> Void
  @State private var preview: UIImage?

  var body: some View {
    VStack(spacing: 12) {
      Button(action: choose) {
        VStack(spacing: 12) {
          ZStack {
            Color.clear
            if let preview { Image(uiImage: preview).resizable().scaledToFit() }
          }
          .aspectRatio(640.0 / 420.0, contentMode: .fit)
          .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
          Text(item.string("title") ?? "")
            .font(.headline)
            .multilineTextAlignment(.center)
            .foregroundStyle(.primary)
            .frame(maxWidth: .infinity)
        }
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .accessibilityIdentifier("blender.template." + (item.string("id") ?? ""))
      if let credit = item.dict("attribution") {
        NavigationLink(value: CreditPage(id: item.string("id") ?? "", credit: credit)) {
          Text("\(credit.string("title") ?? "") · \(credit.string("author") ?? "")\n\(credit.string("license") ?? "") · Adapted")
            .font(.caption)
            .multilineTextAlignment(.center)
            .frame(maxWidth: .infinity)
        }
        .accessibilityLabel("\(credit.string("title") ?? ""). Asset credits and license")
        .accessibilityIdentifier("blender.template.attribution")
      }
    }
    .padding(10)
    .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
    .hoverEffect(.highlight)
    .task(id: item.string("id")) {
      guard let url = item["previewURL"] as? URL else { return }
      preview = await Task.detached(priority: .userInitiated) {
        await UIImage(contentsOfFile: url.path)?.byPreparingForDisplay()
      }.value
    }
  }
}

/** Attribution stays available without leaving the chooser or opening a document. */
struct TemplateCredits: View {
  let credit: JSON

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 16) {
        Text("\(credit.string("title") ?? "")\n\(credit.string("author") ?? "")")
        link(credit.string("licenseURL"))
        link(credit.string("sourceURL"))
        Text(credit.string("changes") ?? "")
        Text("Adaptation does not imply endorsement by the original creators.")
      }
      .font(.body)
      .frame(maxWidth: .infinity, alignment: .leading)
      .padding(20)
      .textSelection(.enabled)
    }
    .background(Color(uiColor: .systemBackground))
    .navigationTitle("Asset Credits")
  }

  @ViewBuilder
  private func link(_ text: String?) -> some View {
    if let text, let url = URL(string: text), url.scheme == "https" {
      Link(text, destination: url)
    }
    else if let text {
      Text(text)
    }
  }
}

@MainActor
@objc(GHOSTIOSSwiftTemplatePicker)
final class TemplatePickerController: UIHostingController<TemplatePicker>, UIAdaptivePresentationControllerDelegate {
  private let store: TemplateStore
  private var finished = false
  private let completion: (String?) -> Void

  @objc init(templates: @escaping () -> [NSDictionary],
             manage: @escaping (NSDictionary, String, String?, @escaping (Bool) -> Void) -> Void,
             completion: @escaping (String?) -> Void)
  {
    self.completion = completion
    var finish: (String?) -> Void = { _ in }
    store = TemplateStore(load: templates, manage: manage) { finish($0) }
    super.init(rootView: TemplatePicker(store: store))
    finish = { [weak self] template in self?.finish(template) }
    sizingOptions = []
  }

  /* The sheet's size, not the grid's: 640 points below a navigation bar. */
  override var preferredContentSize: CGSize {
    get { CGSize(width: 780, height: 693) }
    set {}
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

  private func finish(_ template: String?) {
    guard !finished else { return }
    finished = true
    if let presenting = presentingViewController {
      presenting.dismiss(animated: true) { self.completion(template) }
    }
    else {
      completion(template)
    }
  }

  /** Choose a template as tapping it does, for the device tests. */
  @objc(chooseTemplate:) func choose(_ template: String) {
    finish(template)
  }

  /* Swiping the sheet away cancels, like Cancel. */
  func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
    finish(nil)
  }
}
