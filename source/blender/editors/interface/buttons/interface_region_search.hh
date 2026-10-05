/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Search-box region for search buttons, see `interface_region_search.cc`.
 */

#pragma once

#include "BLI_compiler_attrs.hh"

namespace blender {

struct ARegion;
struct bContext;
struct wmEvent;

namespace ui {

struct Button;
struct ButtonSearch;

/**
 * Search-box for string button.
 */
ARegion *searchbox_create_generic(bContext *C, ARegion *butregion, ButtonSearch *search_but);
ARegion *searchbox_create_operator(bContext *C, ARegion *butregion, ButtonSearch *search_but);
ARegion *searchbox_create_menu(bContext *C, ARegion *butregion, ButtonSearch *search_but);

/**
 * x and y in screen-coords.
 */
bool searchbox_inside(ARegion *region, const int xy[2]) ATTR_NONNULL(1, 2);
int searchbox_find_index(ARegion *region, const char *name);
/**
 * Region is the search box itself.
 */
void searchbox_update(bContext *C, ARegion *region, Button *but, bool reset);
int searchbox_autocomplete(bContext *C, ARegion *region, Button *but, char *str);
bool searchbox_event(
    bContext *C, ARegion *region, Button *but, ARegion *butregion, const wmEvent *event);
/**
 * String validated to be of correct length (but->hardmax).
 */
bool searchbox_apply(Button *but, ARegion *region);
/**
 * Whether the search box currently has a highlighted match, i.e. whether #searchbox_apply()
 * would apply something if called right now.
 */
bool searchbox_has_active_item(ARegion *region);
/**
 * The display text of the search box's currently highlighted match, or null if none is
 * highlighted (see #searchbox_has_active_item).
 */
const char *searchbox_active_item_name(ARegion *region);
/**
 * Hide the search box's results without ending the text edit they belong to
 */
void searchbox_hide(ARegion *region);
/**
 * Extend the text currently being completed (as set by #search_items_set_replace_range) up to
 * the point where the visible matches diverge, like shell/file-browser path completion. Doesn't
 * require (or change) which item is highlighted, and never inserts a full match by itself, so
 * the caller can keep the search box open for further typing.
 *
 * Returns true if the button's text was changed.
 */
bool searchbox_autocomplete_common_prefix(Button *but, ARegion *region);
void searchbox_free(bContext *C, ARegion *region);
/**
 * XXX weak: search_func adds all partial matches.
 */
void button_search_refresh(ButtonSearch *but);

}  // namespace ui
}  // namespace blender
