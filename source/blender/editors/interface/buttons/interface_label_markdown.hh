/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup buttons
 */

#pragma once

#include "BLI_rect.hh"

namespace blender::ui {
struct Block;
struct Button;
struct ButtonLabel;
struct Layout;

bool button_label_is_markdown(const Button *button);

/**
 * Parses and lays out markdown for a label button, adjusting its height.
 * Tries to reuse layout cache from previous redraws.
 */
void label_markdown_resolve(ButtonLabel *button);

/**
 * Creates invisible link buttons for markdown labels in \a block.
 * Call after layout resolve so button positions are final.
 */
void label_markdown_create_link_buttons(Block *block);

/** Draws markdown label contents into \a rect (pixel space). */
void label_markdown_draw(const ButtonLabel *button, const uchar color[4], const rcti *rect);

void label_markdown_dev_config(Layout &layout);

}  // namespace blender::ui
