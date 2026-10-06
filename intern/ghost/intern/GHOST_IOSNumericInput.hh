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

static inline void GHOSTIOSNumberFieldAppearance(UITextField *field, bool scrubbing)
{
  const bool active = field.isFirstResponder || scrubbing;
  field.borderStyle = UITextBorderStyleRoundedRect;
  field.backgroundColor = active ? [field.tintColor colorWithAlphaComponent:0.12] :
                                   UIColor.tertiarySystemFillColor;
  field.layer.cornerRadius = 8;
  field.layer.cornerCurve = kCACornerCurveContinuous;
  field.layer.borderWidth = active ? 1.5 : 0;
  field.layer.borderColor = field.tintColor.CGColor;
  field.textColor = field.enabled ? UIColor.labelColor : UIColor.tertiaryLabelColor;
}
