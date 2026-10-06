/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spuserpref
 */

#pragma once

#include "UI_tree_view.hh"

namespace blender {

struct AnyAssetLibraryDefinition {
  eAssetLibraryType type;
  bUserAssetLibrary *user_library;
  /**
   * The repository of extension defined libraries.
   * Set without a `user_library` for the row grouping the repository's libraries.
   */
  bUserExtensionRepo *extension_repo = nullptr;

  bool is_extension_repo() const
  {
    return (user_library == nullptr) && (extension_repo != nullptr);
  }
};

struct AssetLibraryListItemCommon : public ui::AbstractTreeViewItem {
  AnyAssetLibraryDefinition library;
  int index_in_list = 0;

  AssetLibraryListItemCommon(const AnyAssetLibraryDefinition &library, const int index_in_list);

  bool supports_renaming() const override;
  bool rename(const bContext &C, StringRefNull new_name) override;
};

struct AssetLibraryListItem : public AssetLibraryListItemCommon {
  /* Use the constructor from AssetLibraryListItemCommon. */
  using AssetLibraryListItemCommon::AssetLibraryListItemCommon;

  void build_row(ui::Layout &row) override;
  void on_activate(bContext & /*C*/) override;
  std::optional<bool> should_be_active() const override;
};

struct AssetLibraryProject : public ui::AbstractTreeViewItem {
  AssetLibraryProject() {};
  void build_row(ui::Layout &row) override;
};

/**
 * A list of asset libraries, `AssetLibraryRepoItemType` is used for the rows grouping the
 * libraries of an extension repository, see #AnyAssetLibraryDefinition::is_extension_repo.
 */
template<typename AssetLibraryListItemType,
         typename AssetLibraryRepoItemType = AssetLibraryListItemType>
struct AssetLibraryList : public ui::AbstractTreeView {
  Vector<AnyAssetLibraryDefinition> libraries;

  AssetLibraryList(const Vector<AnyAssetLibraryDefinition> libraries) : libraries(libraries) {};

  void build_tree() override
  {
    /* Group the libraries of an extension repository under its row. */
    AssetLibraryRepoItemType *repo_parent = nullptr;
    AssetLibraryProject *project_group = nullptr;

    int i = 0;
    for (const AnyAssetLibraryDefinition &library : libraries) {
      if (library.is_extension_repo()) {
        repo_parent = &add_tree_item<AssetLibraryRepoItemType>(library, i++);
        continue;
      }
      ui::TreeViewOrItem *parent = this;
      if (repo_parent && library.extension_repo == repo_parent->library.extension_repo) {
        parent = repo_parent;
      }
      if constexpr (std::is_same_v<AssetLibraryListItemType, AssetLibraryListItem>) {
        const bool project_library = library.user_library &&
                                     (library.user_library->flag & ASSET_LIBRARY_PROJECT_DEFINED);
        if (project_library) {
          if (!project_group) {
            project_group = &add_tree_item<AssetLibraryProject>();
          }
          parent = project_group;
        }
      }
      parent->add_tree_item<AssetLibraryListItemType>(library, i++);
    }
    this->is_flat_ = (!repo_parent && !project_group);
  }
};

template<typename AssetLibraryListItemType,
         typename AssetLibraryRepoItemType = AssetLibraryListItemType>
void draw_library_list(const bContext &C,
                       ui::Layout &layout,
                       Vector<AnyAssetLibraryDefinition> &libraries,
                       StringRef view_description)
{
  ui::Block *block = layout.block();

  ui::AbstractTreeView *tree_view = ui::block_add_view(
      *block,
      view_description,
      std::make_unique<AssetLibraryList<AssetLibraryListItemType, AssetLibraryRepoItemType>>(
          libraries));
  tree_view->set_default_rows(5);

  ui::TreeViewBuilder::build_tree_view(C, *tree_view, layout);
}

void draw_active_library_settings(const bContext *C,
                                  ui::Layout &layout,
                                  const AnyAssetLibraryDefinition &library);

}  // namespace blender
