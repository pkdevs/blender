/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <cmath>

static inline double GHOSTIOSNumberSensitivity(double vertical)
{
  return std::exp2(fmin(4.0, fmax(-4.0, -vertical / 80.0)));
}

/* Rotation values cover a much larger range than distances; scrub them faster. */
static const double GHOSTIOSAngleScrubScale = 10.0;

/* Rounded number-field look: tinted while editing or scrubbing, softly filled under a
 * hovering Pencil or pointer. Scrubbing also shows the drag-sensitivity indicator. */
void GHOSTIOSNumberFieldAppearance(UITextField *field, bool scrubbing);

/* Show left/right chevrons and a hover fill while a Pencil or pointer hovers the field. */
void GHOSTIOSNumberFieldInstallHover(UITextField *field);

/* Update the sensitivity indicator during a scrub; `vertical` is the drag's vertical offset
 * from where it began, as passed to #GHOSTIOSNumberSensitivity. */
void GHOSTIOSNumberScrubIndicatorUpdate(UITextField *field, double vertical);
