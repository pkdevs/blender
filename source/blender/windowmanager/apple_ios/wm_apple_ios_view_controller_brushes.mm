/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * The brush rail, in paint and sculpt modes: brush previews, their selection and docking.
 */

#import "wm_apple_ios_view_controller.hh"

@implementation WMIOSViewController (Brushes)

#pragma mark - Brush Rail

- (void)installBrushRail
{
  _brush_buttons = [[NSMutableArray alloc] init];
  _brush_rail = [[[WMIOSPassThroughVisualEffectView alloc] initWithEffect:_tool_rail_glass_effect]
      autorelease];
  _brush_rail.translatesAutoresizingMaskIntoConstraints = NO;
  _brush_rail.clipsToBounds = YES;
  _brush_rail.layer.cornerRadius = 32.0;
  _brush_rail.layer.cornerCurve = kCACornerCurveContinuous;
  _brush_rail.accessibilityIdentifier = @"blender.brushbar";
  _brush_rail.hidden = YES;
  _brush_rail.alpha = 0.0;
  [_overlay_view addSubview:_brush_rail];

  UIPanGestureRecognizer *dock_pan = [[UIPanGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleBrushDockPan:)];
  dock_pan.maximumNumberOfTouches = 1;
  dock_pan.cancelsTouchesInView = YES;
  dock_pan.delaysTouchesBegan = NO;
  dock_pan.delegate = self;
  [_brush_rail addGestureRecognizer:dock_pan];
  [dock_pan release];

  _brush_drag_handle = [self toolbarDragHandle];
  _brush_drag_handle.accessibilityLabel = @"Collapse Brushes";
  _brush_drag_handle.accessibilityIdentifier = @"blender.brushbar.handle";
  for (id<UIInteraction> interaction in [[_brush_drag_handle.interactions copy] autorelease]) {
    if ([(id)interaction isKindOfClass:UIToolTipInteraction.class]) {
      [_brush_drag_handle removeInteraction:interaction];
    }
  }
  [_brush_drag_handle addInteraction:[[[UIToolTipInteraction alloc]
                                         initWithDefaultToolTip:@"Collapse Brushes"] autorelease]];
  [_brush_drag_handle removeTarget:self action:nil forControlEvents:UIControlEventAllEvents];
  [_brush_drag_handle addTarget:self
                         action:@selector(collapseBrushRailFromHandle:)
               forControlEvents:UIControlEventTouchUpInside];
  for (UIGestureRecognizer *gesture in _brush_drag_handle.gestureRecognizers) {
    [gesture removeTarget:self action:nil];
    [gesture addTarget:self action:@selector(handleBrushHandlePan:)];
  }
  [_brush_rail.contentView addSubview:_brush_drag_handle];
  _brush_drag_handle_height_constraint = [_brush_drag_handle.heightAnchor
      constraintEqualToConstant:0.0];

  _brush_scroll = [[WMIOSButtonScrollView alloc] initWithFrame:CGRectZero];
  UIHoverGestureRecognizer *hover = [[UIHoverGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleBrushHover:)];
  [_brush_scroll addGestureRecognizer:hover];
  [hover release];
  _brush_scroll.translatesAutoresizingMaskIntoConstraints = NO;
  _brush_scroll.backgroundColor = UIColor.clearColor;
  _brush_scroll.clipsToBounds = YES;
  _brush_scroll.showsVerticalScrollIndicator = NO;
  _brush_scroll.alwaysBounceVertical = YES;
  _brush_scroll.canCancelContentTouches = YES;
  _brush_scroll.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
  [_brush_rail.contentView addSubview:_brush_scroll];

  _brush_stack = [[WMIOSToolStackView alloc] initWithFrame:CGRectZero];
  _brush_stack.axis = UILayoutConstraintAxisVertical;
  _brush_stack.alignment = UIStackViewAlignmentFill;
  _brush_stack.distribution = UIStackViewDistributionEqualSpacing;
  _brush_stack.spacing = 2.0;
  _brush_stack.translatesAutoresizingMaskIntoConstraints = NO;
  [_brush_scroll addSubview:_brush_stack];
}

- (void)installBrushRailLayout
{
  UILayoutGuide *safe_area = _overlay_view.safeAreaLayoutGuide;
  /* The brush rail shares the tool rail's column, stacked directly above it.
   * #updateBrushRailGeometry divides the column's height between both rails. */
  _brush_rail_leading_constraint = [_brush_rail.leadingAnchor
      constraintEqualToAnchor:safe_area.leadingAnchor
                     constant:14.0];
  _brush_rail_width_constraint = [_brush_rail.widthAnchor constraintEqualToConstant:0.0];
  _brush_rail_height_constraint = [_brush_rail.heightAnchor constraintEqualToConstant:0.0];
  _brush_rail_bottom_constraint = [[_brush_rail.bottomAnchor
      constraintEqualToAnchor:_tool_rail.topAnchor
                     constant:0.0] retain];
  [NSLayoutConstraint activateConstraints:@[
    _brush_rail_leading_constraint,
    _brush_rail_width_constraint,
    _brush_rail_height_constraint,
    _brush_rail_bottom_constraint,

    [_brush_drag_handle.leadingAnchor
        constraintEqualToAnchor:_brush_rail.contentView.leadingAnchor],
    [_brush_drag_handle.trailingAnchor
        constraintEqualToAnchor:_brush_rail.contentView.trailingAnchor],
    [_brush_drag_handle.topAnchor constraintEqualToAnchor:_brush_rail.contentView.topAnchor],
    _brush_drag_handle_height_constraint,
    [_brush_scroll.leadingAnchor constraintEqualToAnchor:_brush_rail.contentView.leadingAnchor],
    [_brush_scroll.trailingAnchor constraintEqualToAnchor:_brush_rail.contentView.trailingAnchor],
    [_brush_scroll.topAnchor constraintEqualToAnchor:_brush_drag_handle.bottomAnchor],
    [_brush_scroll.bottomAnchor constraintEqualToAnchor:_brush_rail.contentView.bottomAnchor],

    [_brush_stack.leadingAnchor
        constraintEqualToAnchor:_brush_scroll.contentLayoutGuide.leadingAnchor
                       constant:6.0],
    [_brush_stack.trailingAnchor
        constraintEqualToAnchor:_brush_scroll.contentLayoutGuide.trailingAnchor
                       constant:-6.0],
    [_brush_stack.topAnchor constraintEqualToAnchor:_brush_scroll.contentLayoutGuide.topAnchor
                                           constant:6.0],
    [_brush_stack.bottomAnchor
        constraintEqualToAnchor:_brush_scroll.contentLayoutGuide.bottomAnchor
                       constant:-6.0],
    [_brush_stack.widthAnchor constraintEqualToAnchor:_brush_scroll.frameLayoutGuide.widthAnchor
                                             constant:-12.0],
  ]];
}

- (BOOL)isBrushListVisible
{
  return _brush_rail_expanded || _brush_rail_docked_left_center;
}

- (void)updateBrushRailGeometry
{
  if (_brush_rail_width_constraint == nil) {
    return;
  }
  const BOOL list_visible = [self isBrushListVisible];
  _brush_rail_width_constraint.constant = !_brush_rail_available ? 0.0 :
                                          _brush_rail_expanded   ? [self expandedToolbarWidth] :
                                                                   64.0;
  if (!_brush_rail_available) {
    /* The tool rail owns the column alone, with its own docking constraints. */
    _brush_rail_height_constraint.constant = 0.0;
    _brush_rail_bottom_constraint.constant = 0.0;
    _tool_rail_bottom_constraint.constant = -12.0 - _bottom_tray;
    _tool_rail_center_y_constraint.constant = -_bottom_tray / 2;
    _tool_rail_center_y_constraint.active = NO;
    _tool_rail_bottom_constraint.active = !_tool_rail_docked_left_center;
    _tool_rail_center_y_constraint.active = _tool_rail_docked_left_center;
  }
  else {
    /* Both rails share one column without overlapping: compact rails stay 64pt, an open list
     * takes the remaining height, and two open lists split it. */
    const CGFloat gap = 10.0, compact = 64.0, top_inset = 16.0, bottom_inset = 12.0;
    CGFloat safe_height = _overlay_view.safeAreaLayoutGuide.layoutFrame.size.height;
    if (safe_height <= 0.0) {
      safe_height = self.view.bounds.size.height;
    }
    /* The rails share the column above the open asset pane. */
    safe_height = MAX(0.0, safe_height - _bottom_tray);
    const CGFloat column = MAX(2 * compact, safe_height - top_inset - bottom_inset - gap);
    const CGFloat max_list = [self expandedToolbarHeight];
    const BOOL tool_list = [self isToolbarListVisible];
    CGFloat tool_height = compact, brush_height = compact;
    if (tool_list && list_visible) {
      tool_height = brush_height = MAX(compact, MIN(max_list, floor(column / 2)));
    }
    else if (tool_list) {
      tool_height = MAX(compact, MIN(max_list, column - compact));
    }
    else if (list_visible) {
      brush_height = MAX(compact, MIN(max_list, column - compact));
    }
    _tool_rail_height_constraint.constant = tool_height;
    _brush_rail_height_constraint.constant = brush_height;
    _brush_rail_bottom_constraint.constant = -gap;
    /* Docking either rail to the center centers the pair; otherwise it rests at the bottom. */
    const BOOL centered = _tool_rail_docked_left_center || _brush_rail_docked_left_center;
    const CGFloat group = tool_height + gap + brush_height;
    _tool_rail_center_y_constraint.active = NO;
    _tool_rail_bottom_constraint.active = YES;
    _tool_rail_bottom_constraint.constant =
        -(centered ? MAX(bottom_inset, (safe_height - group) / 2) : bottom_inset) - _bottom_tray;
  }
  _brush_drag_handle.hidden = !list_visible;
  _brush_drag_handle_height_constraint.constant = list_visible ? 44.0 : 0.0;
  _brush_scroll.scrollEnabled = list_visible;
}

- (void)updateBrushRailWithModel:(id)model
{
  NSArray *items = [model isKindOfClass:NSDictionary.class] ? model[@"items"] : nil;
  NSString *active = [model isKindOfClass:NSDictionary.class] ? model[@"active"] : nil;
  if (![items isKindOfClass:NSArray.class]) {
    items = @[];
  }
  if (![active isKindOfClass:NSString.class]) {
    active = @"";
  }
  /* While asset libraries reload, keep showing the rail rather than flickering it away. */
  const BOOL loading = [model isKindOfClass:NSDictionary.class] && [model[@"loading"] boolValue];
  [self setBrushRailAvailable:items.count > 0 || (loading && _brush_rail_available)];
  if (loading && items.count == 0) {
    return;
  }

  /* Previews arrive a few at a time: the brushes themselves decide when to rebuild the rail, and
   * new previews only replace the images of the buttons there are. */
  NSMutableArray *brushes = [NSMutableArray arrayWithCapacity:items.count];
  for (NSDictionary *item in items) {
    NSMutableDictionary *brush = [[item mutableCopy] autorelease];
    [brush removeObjectForKey:@"preview"];
    [brushes addObject:brush];
  }
  NSData *data = [NSJSONSerialization dataWithJSONObject:brushes
                                                 options:NSJSONWritingSortedKeys
                                                   error:nil];
  NSString *signature = [[[NSString alloc] initWithData:data
                                               encoding:NSUTF8StringEncoding] autorelease];
  if (_brush_signature == nil || ![_brush_signature isEqualToString:signature ?: @""]) {
    [_brush_signature release];
    _brush_signature = [signature ?: @"" copy];
    [self rebuildBrushRailWithItems:items];
  }
  else {
    for (NSDictionary *item in items) {
      for (UIButton *button in _brush_buttons) {
        if ([button.accessibilityIdentifier isEqual:item[@"id"]]) {
          NSString *preview = [item[@"preview"] isKindOfClass:NSString.class] ? item[@"preview"] :
                                                                                @"";
          if (![objc_getAssociatedObject(button, &blender_brush_preview_key) isEqual:preview]) {
            objc_setAssociatedObject(
                button, &blender_brush_preview_key, preview, OBJC_ASSOCIATION_COPY_NONATOMIC);
            objc_setAssociatedObject(button,
                                     &blender_brush_image_key,
                                     blenderIOSBrushImage(preview, 36.0),
                                     OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [button setNeedsUpdateConfiguration];
          }
          break;
        }
      }
    }
  }
  [self setActiveBrushIdentifier:active];
}

- (void)setBrushRailAvailable:(BOOL)available
{
  if (_brush_rail_available == available) {
    return;
  }
  _brush_rail_available = available;
  if (!available) {
    _brush_rail_expanded = NO;
    _brush_press_select_active = NO;
    [self setBrushHoverButton:nil];
    if ([_toolbar_label_button isDescendantOfView:_brush_rail]) {
      [self hideToolbarLabelAnimated:NO];
    }
  }
  if (available) {
    _brush_rail.hidden = NO;
  }
  /* Recomputes the tool rail's own height first, then the shared column. */
  [self updateToolbarHeightForCurrentState];
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0.0 : 0.24
      delay:0.0
      options:UIViewAnimationOptionBeginFromCurrentState |
              UIViewAnimationOptionAllowUserInteraction
      animations:^{
        self->_brush_rail.alpha = available ? 1.0 : 0.0;
        [self->_overlay_view layoutIfNeeded];
      }
      completion:^(BOOL finished) {
        if (!self->_brush_rail_available) {
          self->_brush_rail.hidden = YES;
        }
      }];
}

static char blender_brush_preview_key;
static char blender_brush_image_key;

- (UIButton *)brushButtonForItem:(NSDictionary *)item
{
  NSString *title = item[@"title"];
  NSString *preview = [item[@"preview"] isKindOfClass:NSString.class] ? item[@"preview"] : @"";

  UIButton *button = blenderIOSSwiftButton();
  /* The preview can arrive later, see #updateBrushRailWithModel:. */
  objc_setAssociatedObject(
      button, &blender_brush_preview_key, preview, OBJC_ASSOCIATION_COPY_NONATOMIC);
  objc_setAssociatedObject(button,
                           &blender_brush_image_key,
                           blenderIOSBrushImage(preview, 36.0),
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  button.tintColor = UIColor.labelColor;
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
  button.accessibilityLabel = title;
  button.accessibilityIdentifier = item[@"id"];
  button.accessibilityHint = @"Touch and hold to choose from all brushes";
  button.configurationUpdateHandler = ^(UIButton *updated_button) {
    UIImage *image = objc_getAssociatedObject(updated_button, &blender_brush_image_key);
    const BOOL shows_text = self->_brush_rail_expanded;
    const BOOL is_emphasized = [self isBrushListVisible] &&
                               (updated_button.selected ||
                                updated_button == self->_brush_hover_button);
    updated_button.contentHorizontalAlignment = shows_text ?
                                                    UIControlContentHorizontalAlignmentLeading :
                                                    UIControlContentHorizontalAlignmentCenter;
    updated_button.buttonStyle = is_emphasized && !updated_button.selected ? @"glass" : @"plain";
    updated_button.selectedTint = UIColor.clearColor;
    updated_button.foreground = UIColor.labelColor;
    updated_button.selectedForeground = UIColor.whiteColor;
    updated_button.buttonTitle = shows_text ? title : nil;
    updated_button.buttonImage = image;
    updated_button.imagePadding = shows_text ? 10.0 : 0.0;
    updated_button.buttonInsets = shows_text ? NSDirectionalEdgeInsetsMake(6.0, 8.0, 6.0, 14.0) :
                                               NSDirectionalEdgeInsetsMake(6.0, 6.0, 6.0, 6.0);
    updated_button.contentLeading = shows_text;
  };
  [button addTarget:self
                action:@selector(handleBrushButton:)
      forControlEvents:UIControlEventTouchUpInside];

  UILongPressGestureRecognizer *press_select = [[UILongPressGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleBrushPressSelectGesture:)];
  press_select.minimumPressDuration = 0.22;
  press_select.allowableMovement = 10.0;
  press_select.cancelsTouchesInView = YES;
  press_select.delegate = self;
  [button addGestureRecognizer:press_select];
  [press_select release];

  [button.heightAnchor constraintEqualToConstant:52.0].active = YES;
  [button setNeedsUpdateConfiguration];
  return button;
}

- (void)rebuildBrushRailWithItems:(NSArray *)items
{
  NSString *label_brush = [_toolbar_label_button isDescendantOfView:_brush_rail] ?
                              [[_toolbar_label_button.accessibilityIdentifier copy] autorelease] :
                              nil;
  if (label_brush) {
    _toolbar_label_button = nil;
  }
  for (UIView *view in [[_brush_stack.arrangedSubviews copy] autorelease]) {
    [_brush_stack removeArrangedSubview:view];
    [view removeFromSuperview];
  }
  [_brush_buttons removeAllObjects];
  _brush_hover_button = nil;

  NSString *previous_catalog = nil;
  for (NSDictionary *item in items) {
    if (![item isKindOfClass:NSDictionary.class] || ![item[@"id"] isKindOfClass:NSString.class] ||
        ![item[@"title"] isKindOfClass:NSString.class] || [item[@"id"] length] == 0)
    {
      continue;
    }
    /* Separate catalogs the way the tool rail separates tool groups. */
    NSString *catalog = [item[@"catalog"] isKindOfClass:NSString.class] ? item[@"catalog"] : @"";
    UIView *previous = _brush_stack.arrangedSubviews.lastObject;
    if (previous && ![catalog isEqualToString:previous_catalog ?: @""]) {
      [_brush_stack setCustomSpacing:12.0 afterView:previous];
    }
    previous_catalog = catalog;

    UIButton *button = [self brushButtonForItem:item];
    [_brush_buttons addObject:button];
    [_brush_stack addArrangedSubview:button];
    if ([button.accessibilityIdentifier isEqualToString:label_brush]) {
      _toolbar_label_button = button;
    }
  }
  [self updateBrushRailVisibilityAnimated:NO];
  if (_toolbar_label_button) {
    [self layoutToolbarLabel];
  }
  else if (label_brush) {
    [self hideToolbarLabelAnimated:NO];
  }
}

- (void)setActiveBrushIdentifier:(NSString *)brushIdentifier
{
  if ([_active_brush_identifier isEqualToString:brushIdentifier]) {
    return;
  }
  [_active_brush_identifier release];
  _active_brush_identifier = [brushIdentifier copy];
  [self updateBrushRailVisibilityAnimated:NO];
  [UIView performWithoutAnimation:^{
    for (UIButton *button in self->_brush_buttons) {
      [button updateConfiguration];
    }
  }];
}

- (void)updateBrushRailVisibilityAnimated:(BOOL)animated
{
  const BOOL list_visible = [self isBrushListVisible];
  UIButton *active_button = nil;
  for (UIButton *button in _brush_buttons) {
    if ([button.accessibilityIdentifier isEqualToString:_active_brush_identifier]) {
      active_button = button;
      break;
    }
  }
  /* The compact rail shows the active brush, or the first one while none is active. */
  if (active_button == nil) {
    active_button = _brush_buttons.firstObject;
  }

  for (UIButton *button in _brush_buttons) {
    button.hidden = NO;
    if (list_visible && button.alpha < 1.0) {
      button.alpha = 0.0;
    }
    [button setNeedsUpdateConfiguration];
  }

  void (^applyVisibility)(void) = ^{
    for (UIButton *button in self->_brush_buttons) {
      const BOOL is_active = button == active_button;
      button.selected = list_visible && is_active &&
                        [button.accessibilityIdentifier
                            isEqualToString:self->_active_brush_identifier];
      [button setNeedsUpdateConfiguration];
      button.alpha = (list_visible || is_active) ? 1.0 : 0.0;
    }
    [self->_brush_stack setNeedsLayout];
  };
  void (^finishCompact)(void) = ^{
    if ([self isBrushListVisible]) {
      return;
    }
    for (UIButton *button in self->_brush_buttons) {
      button.selected = NO;
      button.hidden = button != active_button;
      [button setNeedsUpdateConfiguration];
    }
    [self->_brush_scroll setContentOffset:CGPointZero animated:NO];
  };

  [self updateBrushRailGeometry];
  if (!animated) {
    applyVisibility();
    finishCompact();
    [_overlay_view layoutIfNeeded];
    if (list_visible && active_button) {
      [_brush_scroll scrollRectToVisible:CGRectInset(active_button.frame, 0, -12) animated:NO];
    }
    return;
  }
  [UIView animateWithDuration:0.32
      delay:0.0
      usingSpringWithDamping:0.84
      initialSpringVelocity:0.0
      options:UIViewAnimationOptionCurveEaseInOut | UIViewAnimationOptionAllowUserInteraction |
              UIViewAnimationOptionBeginFromCurrentState
      animations:^{
        applyVisibility();
        [self->_overlay_view layoutIfNeeded];
        if (list_visible && active_button) {
          [self->_brush_scroll scrollRectToVisible:CGRectInset(active_button.frame, 0, -12)
                                          animated:NO];
        }
      }
      completion:^(BOOL finished) {
        finishCompact();
      }];
}

- (void)setBrushRailExpanded:(BOOL)expanded animated:(BOOL)animated
{
  if (_brush_rail_expanded == expanded && animated) {
    return;
  }
  _brush_rail_expanded = expanded;
  [self hideToolbarLabelAnimated:NO];
  [self updateBrushRailVisibilityAnimated:animated];
}

- (void)setBrushRailDockedLeftCenter:(BOOL)dockedLeftCenter animated:(BOOL)animated
{
  if (_brush_rail_bottom_constraint == nil) {
    return;
  }
  _brush_rail_docked_left_center = dockedLeftCenter;
  [self updateBrushRailVisibilityAnimated:animated];
  void (^applyDock)(void) = ^{
    self->_brush_rail.transform = CGAffineTransformIdentity;
    [self->_overlay_view layoutIfNeeded];
  };
  if (!animated) {
    applyDock();
    return;
  }
  [UIView animateWithDuration:0.34
                        delay:0.0
       usingSpringWithDamping:0.82
        initialSpringVelocity:0.0
                      options:UIViewAnimationOptionCurveEaseInOut |
                              UIViewAnimationOptionAllowUserInteraction |
                              UIViewAnimationOptionBeginFromCurrentState
                   animations:applyDock
                   completion:nil];
}

- (void)handleBrushDockPan:(UIPanGestureRecognizer *)gesture
{
  if ([self isBrushListVisible]) {
    return;
  }
  CGPoint translation = [gesture translationInView:_overlay_view];
  if (gesture.state == UIGestureRecognizerStateChanged) {
    _brush_rail.transform = CGAffineTransformMakeTranslation(0.0, translation.y);
    return;
  }
  if (gesture.state != UIGestureRecognizerStateEnded &&
      gesture.state != UIGestureRecognizerStateCancelled &&
      gesture.state != UIGestureRecognizerStateFailed)
  {
    return;
  }
  /* Same flick threshold as the tool rail. The rail sits above the tool rail, so dock it
   * on a clear upward drag rather than by its distance to the column's center. */
  CGPoint velocity = [gesture velocityInView:_overlay_view];
  BOOL dock_left_center = translation.y < -40.0;
  if (velocity.y < -520.0) {
    dock_left_center = YES;
  }
  else if (velocity.y > 520.0) {
    dock_left_center = NO;
  }
  [self setBrushRailDockedLeftCenter:dock_left_center animated:YES];
}

- (void)collapseBrushRailFromHandle:(id)sender
{
  UNUSED_VARS(sender);
  [self hideToolbarLabelAnimated:YES];
  _brush_press_select_active = NO;
  _brush_rail_expanded = NO;
  [self setBrushHoverButton:nil];
  [self setBrushRailDockedLeftCenter:NO animated:YES];
}

- (void)handleBrushHandlePan:(UIPanGestureRecognizer *)gesture
{
  CGPoint translation = [gesture translationInView:_overlay_view];
  if (gesture.state == UIGestureRecognizerStateChanged) {
    _brush_rail.transform = CGAffineTransformMakeTranslation(
        0.0, MIN(32.0, MAX(0.0, translation.y * 0.3)));
  }
  else if (gesture.state == UIGestureRecognizerStateEnded ||
           gesture.state == UIGestureRecognizerStateCancelled ||
           gesture.state == UIGestureRecognizerStateFailed)
  {
    CGPoint velocity = [gesture velocityInView:_overlay_view];
    if (gesture.state == UIGestureRecognizerStateEnded &&
        (translation.y > 36.0 || (translation.y > 8.0 && velocity.y > 420.0)))
    {
      [self collapseBrushRailFromHandle:gesture];
    }
    else {
      [UIView animateWithDuration:0.2
                       animations:^{
                         self->_brush_rail.transform = CGAffineTransformIdentity;
                       }];
    }
  }
}

- (void)activateBrushButton:(UIButton *)button
{
  NSString *brush_id = button.accessibilityIdentifier;
  if (![brush_id isKindOfClass:NSString.class] || brush_id.length == 0) {
    return;
  }
  [self setActiveBrushIdentifier:brush_id];
  WM_apple_ios_ui_brush_activate(brush_id.UTF8String);
}

- (void)handleBrushButton:(UIButton *)sender
{
  if (![self isBrushListVisible]) {
    /* A tap on the collapsed rail opens it to pick a different brush. */
    [self setBrushRailExpanded:YES animated:YES];
    return;
  }
  [self activateBrushButton:sender];
  if (_brush_rail_expanded && !_brush_rail_docked_left_center) {
    /* Opened by a tap: picking closes it again. A docked list stays open. */
    [self setBrushRailExpanded:NO animated:YES];
  }
  [self showToolbarLabelForButton:sender transient:YES];
}

- (UIButton *)brushButtonAtGestureLocation:(UIGestureRecognizer *)gesture
{
  CGPoint location = [gesture locationInView:_overlay_view];
  for (UIButton *button in _brush_buttons) {
    if (button.hidden || button.alpha < 0.05) {
      continue;
    }
    CGPoint local_point = [button convertPoint:location fromView:_overlay_view];
    if (CGRectContainsPoint(CGRectInset(button.bounds, -10.0, -6.0), local_point)) {
      return button;
    }
  }
  return nil;
}

- (void)setBrushHoverButton:(UIButton *)hoverButton
{
  if (_brush_hover_button == hoverButton) {
    return;
  }
  UIButton *previous = _brush_hover_button;
  _brush_hover_button = hoverButton;
  previous.highlighted = NO;
  [previous setNeedsUpdateConfiguration];
  _brush_hover_button.highlighted = YES;
  [_brush_hover_button setNeedsUpdateConfiguration];
}

- (void)handleBrushPressSelectGesture:(UILongPressGestureRecognizer *)gesture
{
  switch (gesture.state) {
    case UIGestureRecognizerStateBegan:
      if (_brush_rail_expanded) {
        return;
      }
      _brush_press_select_active = YES;
      [self setBrushRailExpanded:YES animated:YES];
      [self setBrushHoverButton:[self brushButtonAtGestureLocation:gesture]];
      break;
    case UIGestureRecognizerStateChanged:
      if (_brush_press_select_active) {
        [self setBrushHoverButton:[self brushButtonAtGestureLocation:gesture]];
      }
      break;
    case UIGestureRecognizerStateEnded: {
      if (!_brush_press_select_active) {
        return;
      }
      _brush_press_select_active = NO;
      UIButton *button = [self brushButtonAtGestureLocation:gesture] ?: _brush_hover_button;
      [self setBrushHoverButton:nil];
      if (button) {
        [self activateBrushButton:button];
      }
      [self setBrushRailExpanded:NO animated:YES];
      if (button) {
        [self showToolbarLabelForButton:button transient:YES];
      }
      break;
    }
    case UIGestureRecognizerStateCancelled:
    case UIGestureRecognizerStateFailed:
      if (!_brush_press_select_active) {
        return;
      }
      _brush_press_select_active = NO;
      [self setBrushHoverButton:nil];
      [self setBrushRailExpanded:NO animated:YES];
      break;
    default:
      break;
  }
}

- (void)handleBrushHover:(UIHoverGestureRecognizer *)gesture
{
  UIButton *hovered = nil;
  if (gesture.state == UIGestureRecognizerStateBegan ||
      gesture.state == UIGestureRecognizerStateChanged)
  {
    CGPoint location = [gesture locationInView:_brush_scroll];
    if (CGRectContainsPoint(_brush_scroll.bounds, location)) {
      for (UIView *hit = [_brush_scroll hitTest:location withEvent:nil];
           hit && hit != _brush_scroll;
           hit = hit.superview)
      {
        if ([_brush_buttons containsObject:hit]) {
          hovered = (UIButton *)hit;
          break;
        }
      }
    }
  }
  if (hovered) {
    [self showToolbarLabelForButton:hovered transient:NO];
  }
  else if (!_toolbar_label_transient) {
    [self hideToolbarLabelAnimated:YES];
  }
}

@end
