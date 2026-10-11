/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * The navigation gizmo and its pie of views.
 */

#import "wm_apple_ios_view_controller.hh"

@implementation WMIOSViewController (ViewGizmo)

- (UIButton *)viewPieButtonWithTitle:(NSString *)title
                         systemImage:(NSString *)systemImage
                             command:(NSInteger)command
{
  UIButton *button = blenderIOSSwiftButton();
  button.tintColor = UIColor.labelColor;
  /* The pills float over the always-dark viewport, whatever the app's appearance. */
  button.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  button.translatesAutoresizingMaskIntoConstraints = YES;
  button.accessibilityLabel = title;
  button.tag = command;

  UIImage *image = blenderIOSCommandIcon(command, systemImage);
  if (image == nil) {
    image = [UIImage systemImageNamed:@"circle"];
  }
  /* Glass, prominent blue glass while hovered. */
  button.buttonTitle = title;
  button.buttonImage = image;
  button.imagePadding = 7.0;
  button.buttonInsets = NSDirectionalEdgeInsetsMake(8.0, 14.0, 8.0, 14.0);

  [button addTarget:self
                action:@selector(handleOverlayCommand:)
      forControlEvents:UIControlEventTouchUpInside];

  button.hidden = YES;
  button.alpha = 0.0;
  button.transform = CGAffineTransformConcat(CGAffineTransformMakeTranslation(56.0, 56.0),
                                             CGAffineTransformMakeScale(0.72, 0.72));

  [button setNeedsUpdateConfiguration];
  [_view_pie_buttons addObject:button];
  return button;
}

- (void)setViewPieHoverButton:(UIButton *)hoverButton
{
  if (_view_pie_hover_button == hoverButton) {
    return;
  }

  UIButton *previous_hover_button = _view_pie_hover_button;
  _view_pie_hover_button = hoverButton;
  previous_hover_button.selected = NO;
  [previous_hover_button setNeedsUpdateConfiguration];
  _view_pie_hover_button.selected = YES;
  [_view_pie_hover_button setNeedsUpdateConfiguration];
#if !defined(APPLE_TARGET_VISIONOS)
  if (hoverButton) {
    [_view_gizmo_selection_feedback selectionChanged];
  }
#endif
}

- (void)updateViewGizmoFeedback
{
  const BOOL active = _view_pie_visible;
  const BOOL highlighted = active || _view_gizmo_hovered || _view_gizmo_pressed;
  _view_gizmo_feedback.layer.borderColor =
      [UIColor.labelColor colorWithAlphaComponent:active ? 0.5 : 0.2].CGColor;
  _view_gizmo_feedback.backgroundColor = [UIColor.labelColor
      colorWithAlphaComponent:active ? 0.12 : 0.05];
  _view_gizmo_hotspot.accessibilityValue = active ? @"View menu open" : nil;
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0.0 : 0.22
                        delay:0.0
       usingSpringWithDamping:0.78
        initialSpringVelocity:0.0
                      options:UIViewAnimationOptionBeginFromCurrentState |
                              UIViewAnimationOptionAllowUserInteraction
                   animations:^{
                     self->_view_gizmo_feedback.alpha = highlighted ? 1.0 : 0.0;
                     CGFloat scale = active ? 1.08 : (self->_view_gizmo_pressed ? 0.94 : 1.0);
                     self->_view_gizmo_feedback.transform = CGAffineTransformMakeScale(scale,
                                                                                       scale);
                   }
                   completion:nil];
}

- (void)handleViewGizmoHover:(UIHoverGestureRecognizer *)gesture
{
  BOOL hovered = gesture.state == UIGestureRecognizerStateBegan ||
                 gesture.state == UIGestureRecognizerStateChanged;
  if (_view_gizmo_hovered == hovered) {
    return;
  }
  _view_gizmo_hovered = hovered;
  [self updateViewGizmoFeedback];
}

- (void)handleViewGizmoPress:(UIControl *)control
{
  _view_gizmo_pressed = YES;
#if !defined(APPLE_TARGET_VISIONOS)
  [_view_gizmo_open_feedback prepare];
#endif
  [self updateViewGizmoFeedback];
}

- (void)handleViewGizmoRelease:(UIControl *)control
{
  _view_gizmo_pressed = NO;
  [self updateViewGizmoFeedback];
}

- (void)cancelViewGizmoInteraction
{
  [self hideToolbarLabelAnimated:NO];
  _view_gizmo_hovered = NO;
  _view_gizmo_pressed = NO;
  [self setViewPieVisible:NO animated:NO];
}

- (void)layoutViewPie
{
  CGRect viewport = _overlay_view.bounds;
  CGFloat radius = _view_gizmo_geometry ?
                       [_view_gizmo_geometry[2] doubleValue] * viewport.size.width :
                       40;
  CGPoint center = _view_gizmo_geometry ?
                       CGPointMake([_view_gizmo_geometry[0] doubleValue] * viewport.size.width,
                                   [_view_gizmo_geometry[1] doubleValue] * viewport.size.height) :
                       CGPointMake(CGRectGetMaxX(viewport) - 50, CGRectGetMaxY(viewport) - 50);
  CGFloat target_radius = MAX(44.0, radius + 10.0);
  _view_gizmo_hotspot.bounds = CGRectMake(0, 0, target_radius * 2, target_radius * 2);
  _view_gizmo_hotspot.center = center;
  _view_gizmo_feedback.bounds = CGRectMake(0, 0, (radius + 2) * 2, (radius + 2) * 2);
  _view_gizmo_feedback.center = CGPointMake(target_radius, target_radius);
  _view_gizmo_feedback.layer.cornerRadius = radius + 2;
  CGRect safe = _overlay_view.safeAreaLayoutGuide.layoutFrame;
  if (CGRectIsEmpty(safe)) {
    return;
  }
  const BOOL compact = safe.size.height < 540.0 || safe.size.width < 430.0;
  CGFloat gap = CGRectGetMaxX(safe) - CGRectGetMinX(_view_gizmo_hotspot.frame);
  const CGFloat trailing[] = {gap + 16, gap + 26, gap + 42, gap + 42, gap + 16, 20, 20, 20};
  const CGFloat bottom[] = {12, 72, 132, 192, 244, 304, 364, 424};
  CGFloat width = MIN(180.0, (safe.size.width - 36.0) / 2.0);
  CGFloat base = MIN(136.0, MAX(68.0, safe.size.height - 300.0));
  for (NSUInteger i = 0; i < _view_pie_buttons.count; i++) {
    UIButton *button = _view_pie_buttons[i];
    CGFloat x = compact ? 12.0 + (i % 2) * (width + 12.0) : trailing[i];
    CGFloat y = compact ? base + (i / 2) * 52.0 : bottom[i];
    CGFloat w = compact ? width : MIN(180.0, [button sizeThatFits:CGSizeMake(180, 44)].width);
    /* Bounds/center remain stable while the reveal transform is animating. */
    button.bounds = CGRectMake(0, 0, w, 44);
    button.center = CGPointMake(CGRectGetMaxX(safe) - x - w / 2.0, CGRectGetMaxY(safe) - y - 22.0);
  }
}

- (UIButton *)viewPieButtonAtGestureLocation:(UIGestureRecognizer *)gesture
{
  CGPoint location = [gesture locationInView:_overlay_view];
  for (UIButton *button in _view_pie_buttons) {
    if (button.hidden || button.alpha < 0.05) {
      continue;
    }
    CGPoint local_point = [button convertPoint:location fromView:_overlay_view];
    CGRect hit_bounds = CGRectInset(button.bounds, -14.0, -10.0);
    if (CGRectContainsPoint(hit_bounds, local_point)) {
      return button;
    }
  }
  return nil;
}

- (void)setViewPieVisible:(BOOL)visible animated:(BOOL)animated
{
  _view_pie_visible = visible;
  if (!visible) {
    [self setViewPieHoverButton:nil];
  }
  [self updateViewGizmoFeedback];
  if (visible) {
    [self layoutViewPie];
    for (UIButton *button in _view_pie_buttons) {
      if (button.hidden) {
        CGPoint hub = _view_gizmo_hotspot.center;
        button.transform = CGAffineTransformConcat(
            CGAffineTransformMakeTranslation((hub.x - button.center.x) * 0.18,
                                             (hub.y - button.center.y) * 0.18),
            CGAffineTransformMakeScale(0.82, 0.82));
      }
      button.hidden = NO;
    }
  }

  void (^applyVisibility)(void) = ^{
    for (UIButton *button in self->_view_pie_buttons) {
      button.alpha = visible ? 1.0 : 0.0;
      CGPoint hub = self->_view_gizmo_hotspot.center;
      CGFloat travel = visible ? 0.0 : 0.18;
      button.transform = CGAffineTransformConcat(
          CGAffineTransformMakeTranslation((hub.x - button.center.x) * travel,
                                           (hub.y - button.center.y) * travel),
          CGAffineTransformMakeScale(visible ? 1.0 : 0.82, visible ? 1.0 : 0.82));
    }
  };

  void (^finishVisibility)(BOOL) = ^(BOOL finished) {
    if (!visible && !self->_view_pie_visible) {
      for (UIButton *button in self->_view_pie_buttons) {
        button.hidden = YES;
      }
    }
  };

  if (!animated || UIAccessibilityIsReduceMotionEnabled()) {
    applyVisibility();
    finishVisibility(YES);
    return;
  }

  [UIView animateWithDuration:0.28
                        delay:0.0
       usingSpringWithDamping:0.86
        initialSpringVelocity:0.0
                      options:UIViewAnimationOptionCurveEaseOut |
                              UIViewAnimationOptionAllowUserInteraction |
                              UIViewAnimationOptionBeginFromCurrentState
                   animations:applyVisibility
                   completion:finishVisibility];
}

- (void)handleViewGizmoDrag:(UIPanGestureRecognizer *)gesture
{
  if (gesture.state == UIGestureRecognizerStateBegan) {
    [self setViewPieVisible:YES animated:YES];
    [self setViewPieHoverButton:[self viewPieButtonAtGestureLocation:gesture]];
    return;
  }

  if (gesture.state == UIGestureRecognizerStateChanged) {
    [self setViewPieHoverButton:[self viewPieButtonAtGestureLocation:gesture]];
    return;
  }

  if (gesture.state == UIGestureRecognizerStateEnded) {
    UIButton *button = [self viewPieButtonAtGestureLocation:gesture];
    if (button != nil) {
      WM_apple_ios_ui_command_enqueue((int)button.tag);
    }
    [self setViewPieVisible:NO animated:YES];
    return;
  }

  if (gesture.state == UIGestureRecognizerStateCancelled ||
      gesture.state == UIGestureRecognizerStateFailed)
  {
    [self setViewPieVisible:NO animated:YES];
  }
}

- (void)handleViewGizmoHold:(UILongPressGestureRecognizer *)gesture
{
  if (gesture.state == UIGestureRecognizerStateBegan) {
    /* Consume the pending Pencil tap before it can become a Blender click. */
    [(UIWindow<GHOST_IOSInputWindow> *)_view.window cancelPencilStroke];
    [self setViewPieVisible:YES animated:YES];
#if !defined(APPLE_TARGET_VISIONOS)
    [_view_gizmo_open_feedback impactOccurred];
    [_view_gizmo_selection_feedback prepare];
#endif
  }
  else if (gesture.state == UIGestureRecognizerStateChanged) {
    [self setViewPieHoverButton:[self viewPieButtonAtGestureLocation:gesture]];
  }
  else if (gesture.state == UIGestureRecognizerStateEnded) {
    UIButton *button = [self viewPieButtonAtGestureLocation:gesture];
    if (button) {
      WM_apple_ios_ui_command_enqueue((int)button.tag);
    }
    [self setViewPieVisible:NO animated:YES];
  }
  else if (gesture.state == UIGestureRecognizerStateCancelled ||
           gesture.state == UIGestureRecognizerStateFailed)
  {
    [self setViewPieVisible:NO animated:YES];
  }
}

- (void)handleViewGizmoTap:(UITapGestureRecognizer *)gesture
{
  [(UIWindow<GHOST_IOSInputWindow> *)_view.window handleNavigationTap:gesture];
}

- (void)handleOverlayCommand:(UIButton *)sender
{
  if ([_view_pie_buttons containsObject:sender]) {
    [self setViewPieVisible:NO animated:YES];
  }
  WM_apple_ios_ui_command_enqueue((int)sender.tag);
}

@end
