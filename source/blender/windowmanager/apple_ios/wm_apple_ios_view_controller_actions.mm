/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Quick actions for what is selected, the context menu, transform axes and Adjust Last
 * Operation.
 */

#import "wm_apple_ios_view_controller.hh"

@implementation WMIOSViewController (Actions)

- (NSArray<NSDictionary *> *)radialItemsFromBlenderMenu:(NSArray *)sources
{
  NSMutableArray *items = [NSMutableArray array];
  for (NSDictionary *source in sources) {
    if (![source isKindOfClass:NSDictionary.class]) {
      continue;
    }
    if ([source[@"heading"] isKindOfClass:NSString.class]) {
      /* The menu's own name, shown in the radial menu's center. */
      [items addObject:@{@"heading" : source[@"heading"]}];
      continue;
    }
    NSString *op = source[@"operator"];
    NSString *title = source[@"title"];
    if (![title isKindOfClass:NSString.class] || !title.length ||
        ![source[@"route"] isKindOfClass:NSDictionary.class] ||
        (![source[@"action"] boolValue] && ![source[@"submenu"] boolValue]))
    {
      continue;
    }
    /* Keep source order, full titles, and distinct property-bearing routes, even
     * when several entries invoke the same operator (e.g. Set Origin or Delete). */
    NSMutableDictionary *item = [[source mutableCopy] autorelease];
    item[@"destructive"] = @([op isEqual:@"OBJECT_OT_delete"] || [op isEqual:@"MESH_OT_delete"]);
    UIImage *image = blenderIOSMenuIcon(source);
    if (!image && [source[@"submenu"] boolValue]) {
      image = [UIImage systemImageNamed:@"chevron.right"];
    }
    if (image) {
      item[@"image"] = image;
    }
    [items addObject:item];
  }
  return items;
}

- (void)pencilMenu:(UIView<GHOSTIOSPencilMenu> *)menu
    requestItemsForRoute:(NSDictionary *)route
                 atPoint:(CGPoint)point
              completion:(void (^)(NSArray<NSDictionary *> *))completion
{
  if (!_ghost_window) {
    completion(@[]);
    return;
  }
  CGPoint viewport_point = [menu convertPoint:point toView:_view];
  CGPoint pixels = GHOST_ios_window_pixels(_ghost_window, viewport_point);
  NSString *request_route = blenderIOSMenuRoute(route);
  const uint64_t request = WM_apple_ios_ui_menu_at_position_request(
      request_route.UTF8String, false, pixels.x, pixels.y);
  const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 60.0;
  NSTimer *poll = [NSTimer
      timerWithTimeInterval:0.025
                    repeats:YES
                      block:^(NSTimer *timer) {
                        NSArray *result = blenderIOSMenuResult(request);
                        if (result || CFAbsoluteTimeGetCurrent() >= deadline) {
                          [timer invalidate];
                          completion([self radialItemsFromBlenderMenu:result]);
                        }
                      }];
  [NSRunLoop.mainRunLoop addTimer:poll forMode:NSRunLoopCommonModes];
}

- (void)pencilMenu:(UIView<GHOSTIOSPencilMenu> *)menu
     didChooseItem:(NSDictionary *)item
           atPoint:(CGPoint)point
{
  if (!_ghost_window || [item[@"disabled"] boolValue] ||
      ![item[@"route"] isKindOfClass:NSDictionary.class])
  {
    return;
  }
  CGPoint viewport_point = [menu convertPoint:point toView:_view];
  CGPoint pixels = GHOST_ios_window_pixels(_ghost_window, viewport_point);
  NSString *route = blenderIOSMenuRoute(item[@"route"]);
  WM_apple_ios_ui_menu_at_position_request(route.UTF8String, true, pixels.x, pixels.y);
}

- (UIMenu *)nativeBlenderContextMenu
{
  /* Resolve the same live context menu as Blender and the Pencil radial menu.
   * A deferred provider keeps UIKit responsive while the engine is busy. */
  return blenderIOSNativeSubmenu(@{
    @"title" : @"Actions",
    @"route" : @{@"root" : @"context", @"path" : @[]},
  },
                                 nil);
}

- (UIMenu *)editMenuInteraction:(UIEditMenuInteraction *)interaction
           menuForConfiguration:(UIEditMenuConfiguration *)configuration
               suggestedActions:(NSArray<UIMenuElement *> *)suggestedActions
{
  UIMenu *menu = [self nativeBlenderContextMenu];
  return menu.children.count == 0 ? nil : menu;
}

- (CGRect)editMenuInteraction:(UIEditMenuInteraction *)interaction
    targetRectForConfiguration:(UIEditMenuConfiguration *)configuration
{
  const CGPoint point = configuration.sourcePoint;
  return CGRectMake(point.x, point.y, 1.0, 1.0);
}

- (void)handleActionsMenuPress:(UILongPressGestureRecognizer *)gesture
{
  /* Blender's own interface has its own context menus. */
  if (gesture.state != UIGestureRecognizerStateBegan || _pencil_menu.active || _desktop_interface)
  {
    return;
  }

  UIMenu *menu = [self nativeBlenderContextMenu];
  if (menu.children.count == 0) {
    return;
  }

  const CGPoint location = [gesture locationInView:_view];
  UIEditMenuConfiguration *configuration = [UIEditMenuConfiguration
      configurationWithIdentifier:nil
                      sourcePoint:location];
  [_actions_edit_menu_interaction presentEditMenuWithConfiguration:configuration];
}

- (void)updateAxisConstraintButton:(UIButton *)button selected:(BOOL)selected
{
  NSString *axis = button.accessibilityHint;
  UIColor *color = [axis isEqualToString:@"X"] ? UIColor.systemRedColor :
                   [axis isEqualToString:@"Y"] ? UIColor.systemGreenColor :
                                                 UIColor.systemBlueColor;
  button.selected = selected;
  button.tintColor = color;
  button.buttonTitle = axis;
  button.buttonFont = [UIFont systemFontOfSize:17.0 weight:UIFontWeightSemibold];
  /* Selected lettering in black stays readable on the bright axis-colored glass. */
  button.buttonImage =
      selected ?
          [[UIImage systemImageNamed:@"checkmark"]
              imageWithConfiguration:[UIImageSymbolConfiguration
                                         configurationWithPointSize:11.0
                                                             weight:UIImageSymbolWeightBold]] :
          nil;
  button.imageTrailing = YES;
  button.imagePadding = selected ? 4.0 : 0.0;
  button.buttonInsets = NSDirectionalEdgeInsetsMake(8.0, 8.0, 8.0, 8.0);
  button.foreground = color;
  button.selectedTint = color;
  button.selectedForeground = UIColor.blackColor;
  button.accessibilityLabel = [NSString stringWithFormat:@"%@ axis constraint", axis];
  button.accessibilityValue = selected ? @"On" : @"Off";
  button.accessibilityTraits = UIAccessibilityTraitButton |
                               (selected ? UIAccessibilityTraitSelected : 0);
}

- (void)updateAxisConstraintsWithActions:(NSArray *)actions
{
  for (UIButton *button in _action_buttons) {
    if ([button.accessibilityIdentifier hasPrefix:@"blender.action."]) {
      /* Toggles such as X-Ray show their state as a filled button. */
      for (NSDictionary *action in actions) {
        if ([action isKindOfClass:NSDictionary.class] &&
            [action[@"action"] isKindOfClass:NSString.class] &&
            [button.accessibilityIdentifier
                isEqualToString:[@"blender.action." stringByAppendingString:action[@"action"]]])
        {
          const BOOL selected = [action[@"selected"] respondsToSelector:@selector(boolValue)] &&
                                [action[@"selected"] boolValue];
          if (button.selected != selected) {
            button.selected = selected;
            UIButtonConfiguration *configuration = button.configuration;
            configuration.baseBackgroundColor = selected ? UIColor.systemBlueColor : nil;
            configuration.baseForegroundColor = selected ? UIColor.whiteColor : nil;
            button.configuration = configuration;
            button.accessibilityTraits = selected ? UIAccessibilityTraitButton |
                                                        UIAccessibilityTraitSelected :
                                                    UIAccessibilityTraitButton;
          }
          break;
        }
      }
      continue;
    }
    NSString *axis = button.accessibilityHint;
    if (![@[ @"X", @"Y", @"Z" ] containsObject:axis ?: @""]) {
      continue;
    }
    for (NSDictionary *action in actions) {
      if ([action isKindOfClass:NSDictionary.class] && [action[@"key"] isEqual:axis]) {
        const BOOL selected = [action[@"selected"] respondsToSelector:@selector(boolValue)] &&
                              [action[@"selected"] boolValue];
        if (button.selected != selected) {
          [self updateAxisConstraintButton:button selected:selected];
        }
        break;
      }
    }
  }
}

- (UIButton *)actionButtonWithTitle:(NSString *)title
                        systemImage:(NSString *)systemImage
                        operator
                                   :(NSString *)opName
                           property:(NSString *)propertyName
                              value:(NSString *)propertyValue
                                key:(NSString *)keyName
{
  UNUSED_VARS(systemImage);
  UIButton *button = blenderIOSSwiftButton();
  button.tintColor = UIColor.systemBlueColor;
  button.accessibilityLabel = title;
  button.accessibilityIdentifier = opName;
  button.accessibilityValue = propertyName;
  button.restorationIdentifier = propertyValue;
  button.accessibilityHint = keyName;
  button.translatesAutoresizingMaskIntoConstraints = NO;
  [button setContentHuggingPriority:UILayoutPriorityRequired
                            forAxis:UILayoutConstraintAxisHorizontal];
  [button setContentCompressionResistancePriority:UILayoutPriorityRequired
                                          forAxis:UILayoutConstraintAxisHorizontal];

  UIImage *image = keyName.length == 0 ?
                       blenderIOSActionIcon(opName, propertyName, propertyValue) :
                       nil;

  button.buttonTitle = title;
  button.buttonImage = image;
  /* Glass buttons draw their title in the label color, whatever the tint. */
  button.foreground = UIColor.labelColor;
  button.imagePadding = image != nil ? 5.0 : 0.0;
  button.buttonInsets = NSDirectionalEdgeInsetsMake(8.0, 10.0, 8.0, 10.0);
  if ([@[ @"X", @"Y", @"Z" ] containsObject:keyName ?: @""]) {
    [self updateAxisConstraintButton:button selected:NO];
    [button.widthAnchor constraintEqualToConstant:60.0].active = YES;
  }

  [button addTarget:self
                action:@selector(handleActionButton:)
      forControlEvents:UIControlEventTouchUpInside];

  [NSLayoutConstraint activateConstraints:@[
    [button.heightAnchor constraintEqualToConstant:44.0],
  ]];

  [_action_buttons addObject:button];
  return button;
}

/** A quick action's look, opening Blender's whole context menu. */
- (UIButton *)contextMenuButton
{
  UIButton *button = blenderIOSSwiftButton();
  button.accessibilityLabel = @"Actions";
  button.accessibilityIdentifier = @"blender.context-menu";
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.buttonImage = [UIImage systemImageNamed:@"ellipsis"];
  button.foreground = UIColor.labelColor;
  button.buttonInsets = NSDirectionalEdgeInsetsMake(8.0, 10.0, 8.0, 10.0);
  button.menu = [self nativeBlenderContextMenu];
  button.showsMenuAsPrimaryAction = YES;
  [button.heightAnchor constraintEqualToConstant:44.0].active = YES;
  return button;
}

/** Show the context menu button while the quick actions are open and have one. */
- (void)updateContextMenuButton
{
  if (!_context_menu_button) {
    return;
  }
  const BOOL visible = _context_menu_available && !_actions_collapsed;
  _context_menu_width_constraint.constant = visible ? 44.0 : 0.0;
  _context_menu_spacing_constraint.constant = visible ? -8.0 : 0.0;
  _context_menu_button.hidden = !_context_menu_available;
  _context_menu_button.userInteractionEnabled = visible;
  _context_menu_button.accessibilityElementsHidden = !visible;
  const CGFloat redo_width = _redo_strip.collapsedWidth;
  _action_expanded_width_constraint.constant = 60 + (redo_width ? redo_width + 8 : 0) +
                                               (visible ? 52 : 0);
}

- (NSString *)actionSignatureForActions:(NSArray *)actions
{
  NSMutableString *signature = [NSMutableString string];
  for (NSDictionary *action in actions) {
    if (![action isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    for (NSString *json_key in
         @[ @"title", @"action", @"operator", @"property", @"value", @"key" ])
    {
      NSString *value = action[json_key];
      if ([value isKindOfClass:[NSString class]]) {
        [signature appendString:@"|"];
        [signature appendString:value];
      }
    }
  }
  return signature;
}

- (void)rebuildActionStripWithActions:(NSArray *)actions
{
  /* Settings can hide the quick actions; Adjust Last Operation stays, it is not one of them. */
  if (_settings && ![_settings.class showQuickActions]) {
    actions = @[];
  }
  NSArray *arranged_subviews = [[_action_stack.arrangedSubviews copy] autorelease];
  for (UIView *view in arranged_subviews) {
    [_action_stack removeArrangedSubview:view];
    [view removeFromSuperview];
  }
  [_action_buttons removeAllObjects];

  for (NSDictionary *action in actions) {
    if (![action isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    NSString *title = action[@"title"];
    NSString *op_name = action[@"operator"];
    NSString *symbol = action[@"symbol"];
    NSString *property = action[@"property"];
    NSString *value = action[@"value"];
    NSString *key = action[@"key"];
    NSString *action_id = [action[@"action"] isKindOfClass:NSString.class] ? action[@"action"] :
                                                                             @"";
    NSString *icon = [action[@"icon"] isKindOfClass:NSString.class] ? action[@"icon"] : @"";
    if (![title isKindOfClass:[NSString class]] || ![symbol isKindOfClass:[NSString class]] ||
        title.length == 0)
    {
      continue;
    }
    if (![op_name isKindOfClass:[NSString class]]) {
      op_name = @"";
    }
    if (![property isKindOfClass:[NSString class]]) {
      property = @"";
    }
    if (![value isKindOfClass:[NSString class]]) {
      value = @"";
    }
    if (![key isKindOfClass:[NSString class]]) {
      key = @"";
    }
    if (op_name.length == 0 && key.length == 0) {
      continue;
    }
    UIButton *button = [self actionButtonWithTitle:title
                                       systemImage:symbol
                                          operator:op_name
                                          property:property
                                             value:value
                                               key:key];
    if (action_id.length) {
      /* Chosen by `bl_ui.native_actions`, which also runs it by this identifier. */
      button.accessibilityIdentifier = [@"blender.action." stringByAppendingString:action_id];
      if (icon.length) {
        UIImage *image = WMIOSBlenderIcon(icon, NO);
        button.buttonImage = image;
        button.imagePadding = image ? 5.0 : 0.0;
      }
    }
#if !defined(APPLE_TARGET_VISIONOS)
    [_action_stack addArrangedSubview:button];
#endif
  }
#if !defined(APPLE_TARGET_VISIONOS)
  /* Everything else for the selection: Blender's full context menu, after the quick actions.
   * Not while a transform takes keys (the axes), which are actions too. */
  BOOL axis_keys = NO;
  for (UIButton *button in _action_buttons) {
    axis_keys |= button.accessibilityHint.length > 0;
  }
  _context_menu_available = _action_buttons.count && !axis_keys;
  [self updateContextMenuButton];
#endif
#if defined(APPLE_TARGET_VISIONOS)
  [_vision_chrome setActions:_action_buttons];
#endif

  _action_rail.hidden = [self actionRailEmpty:_redo_strip.collapsedWidth];
  _action_rail.alpha = _action_rail.hidden ? 0.0 : 1.0;
  [self updateAxisConstraintsWithActions:actions];
  [_action_scroll setContentOffset:CGPointZero animated:NO];
}

/* Fixed so the Actions pill never resizes as the redo pill comes and goes. */
#if defined(APPLE_TARGET_VISIONOS)
/* The quick actions are in the toolbar ornament: no Actions pill. */
static const CGFloat WMIOSActionsPillWidth = 0.0;
#else
static const CGFloat WMIOSActionsPillWidth = 88.0;
#endif

/** Nothing for the rail to show: no quick actions or Adjust Last Operation pill. */
- (BOOL)actionRailEmpty:(CGFloat)redo_pill_width
{
#if defined(APPLE_TARGET_VISIONOS)
  return redo_pill_width == 0;
#else
  return _action_buttons.count == 0 && redo_pill_width == 0;
#endif
}

- (void)installActionStripToggle
{
  _action_group = [[[WMIOSPassThroughOverlayView alloc] init] autorelease];
  _action_group.translatesAutoresizingMaskIntoConstraints = NO;
  [_action_rail.contentView addSubview:_action_group];
  [_action_group addSubview:_action_scroll];

  _action_toggle = [UIButton buttonWithType:UIButtonTypeSystem];
  _action_toggle.translatesAutoresizingMaskIntoConstraints = NO;
  _action_toggle.tintColor = UIColor.labelColor;
  _action_toggle.accessibilityIdentifier = @"blender.quick-actions.toggle";
  _action_toggle.titleLabel.adjustsFontSizeToFitWidth = YES;
  _action_toggle.titleLabel.minimumScaleFactor = 0.7;
  [_action_toggle addTarget:self
                     action:@selector(toggleActionStrip:)
           forControlEvents:UIControlEventTouchUpInside];
  [_action_group addSubview:_action_toggle];
#if defined(APPLE_TARGET_VISIONOS)
  _action_toggle.hidden = YES;
#endif
  UIButton *redo_pill = _redo_strip.collapsedButton;
  redo_pill.translatesAutoresizingMaskIntoConstraints = NO;
  [_action_group addSubview:redo_pill];
  _redo_pill_width_constraint = [[redo_pill.widthAnchor constraintEqualToConstant:0] retain];
  _redo_pill_width_constraint.priority = UILayoutPriorityDefaultHigh - 1;
  _redo_pill_min_width_constraint = [[redo_pill.widthAnchor
      constraintGreaterThanOrEqualToConstant:0] retain];
#if defined(APPLE_TARGET_VISIONOS)
  _action_scroll_trailing_constraint = [[_action_scroll.trailingAnchor
      constraintEqualToAnchor:redo_pill.leadingAnchor] retain];
#else
  /* The context menu sits after the scrolling actions, so it stays in view. */
  _context_menu_button = [[self contextMenuButton] retain];
  [_action_group addSubview:_context_menu_button];
  _context_menu_width_constraint = [[_context_menu_button.widthAnchor constraintEqualToConstant:0]
      retain];
  _context_menu_spacing_constraint = [[_action_scroll.trailingAnchor
      constraintEqualToAnchor:_context_menu_button.leadingAnchor] retain];
  _action_scroll_trailing_constraint = [[_context_menu_button.trailingAnchor
      constraintEqualToAnchor:redo_pill.leadingAnchor] retain];
  [NSLayoutConstraint activateConstraints:@[
    _context_menu_width_constraint,
    _context_menu_spacing_constraint,
    [_context_menu_button.centerYAnchor constraintEqualToAnchor:_action_group.centerYAnchor],
  ]];
#endif
  __block WMIOSViewController *owner = self;
  _redo_strip.presentationDidChange = ^(BOOL animated) {
    [owner updateRedoPillLayout:animated];
  };

  _action_expanded_width_constraint = [[_action_group.widthAnchor
      constraintEqualToAnchor:_action_stack.widthAnchor
                     constant:60.0] retain];
  _action_expanded_width_constraint.priority = UILayoutPriorityDefaultHigh;
  _action_collapsed_width_constraint = [[_action_group.widthAnchor
      constraintEqualToConstant:WMIOSActionsPillWidth] retain];
  _action_collapsed_width_constraint.priority = UILayoutPriorityDefaultHigh;
  _action_toggle_width_constraint = [[_action_toggle.widthAnchor constraintEqualToConstant:44.0]
      retain];
  _action_toggle_width_constraint.priority = UILayoutPriorityRequired - 1;
  _action_toggle_trailing_constraint = [[_action_toggle.trailingAnchor
      constraintEqualToAnchor:redo_pill.leadingAnchor] retain];
  /* Expanded controls center on the viewport; collapsed pills rest bottom-leading. */
  _action_group_leading_constraint = [[_action_group.leadingAnchor
      constraintEqualToAnchor:_action_rail.contentView.leadingAnchor] retain];
  _action_group_center_constraint = [[_action_group.centerXAnchor
      constraintEqualToAnchor:_overlay_view.centerXAnchor] retain];
  _action_group_center_constraint.priority = UILayoutPriorityRequired - 2;
  NSLayoutConstraint *minimum_width = [_action_group.widthAnchor
      constraintGreaterThanOrEqualToConstant:WMIOSActionsPillWidth];
  minimum_width.priority = UILayoutPriorityRequired - 1;
  [NSLayoutConstraint activateConstraints:@[
    minimum_width,
    [_action_group.leadingAnchor
        constraintGreaterThanOrEqualToAnchor:_action_rail.contentView.leadingAnchor],
    [_action_group.trailingAnchor
        constraintLessThanOrEqualToAnchor:_action_rail.contentView.trailingAnchor],
    [_action_group.topAnchor constraintEqualToAnchor:_action_rail.contentView.topAnchor],
    [_action_group.bottomAnchor constraintEqualToAnchor:_action_rail.contentView.bottomAnchor],
    [_action_toggle.leadingAnchor constraintEqualToAnchor:_action_group.leadingAnchor],
    [_action_toggle.centerYAnchor constraintEqualToAnchor:_action_group.centerYAnchor],
    [_action_toggle.heightAnchor constraintEqualToConstant:44.0],
    _redo_pill_width_constraint,
    _redo_pill_min_width_constraint,
    _action_scroll_trailing_constraint,
    [redo_pill.trailingAnchor constraintEqualToAnchor:_action_group.trailingAnchor],
    [redo_pill.centerYAnchor constraintEqualToAnchor:_action_group.centerYAnchor],
    [redo_pill.heightAnchor constraintEqualToConstant:44.0],
  ]];
  /* Quick actions start folded into the Actions pill, leaving the viewport clear. */
  [self setActionsCollapsed:YES animated:NO];
}

- (void)layoutRedoStrip
{
  CGRect rect = _redo_lane_rect;
  if (_redo_lane_wide) {
    /* Share the pills' row, starting after Actions, so the strip grows sideways
     * out of its own pill and never crosses another control. */
    const CGRect row = [_action_rail convertRect:_action_rail.bounds toView:_overlay_view];
    const CGRect group = [_action_group convertRect:_action_group.bounds toView:_overlay_view];
    const CGFloat left = _action_rail.hidden ? CGRectGetMinX(rect) :
                                               MAX(CGRectGetMinX(rect), CGRectGetMaxX(group) + 8);
    const CGFloat top = MIN(CGRectGetMidY(row) - 30,
                            CGRectGetMinY(_overlay_view.keyboardLayoutGuide.layoutFrame) - 68);
    rect = CGRectMake(left, top, MAX(0, CGRectGetMaxX(rect) - left), 60);
  }
  [_redo_strip layoutInRect:rect centerX:CGRectGetMidX(_overlay_view.bounds)];
}

- (void)updateRedoPillLayout:(BOOL)animated
{
  [_action_rail layoutIfNeeded];
  /* Only one of the two may be open: opening the strip folds the quick actions. */
  if (_redo_strip.expanded && !_actions_collapsed) {
    [self setActionsCollapsed:YES animated:animated];
  }
  const CGFloat width = _redo_strip.collapsedWidth;
  if (_redo_pill_width_constraint.constant == width) {
    [self layoutRedoStrip];
    return;
  }
  const CGFloat reserved = width ? width + 8 : 0;
  _redo_pill_width_constraint.constant = width;
  _redo_pill_min_width_constraint.constant = width ? 44 : 0;
  _action_scroll_trailing_constraint.constant = width ? -8 : 0;
  _action_toggle_trailing_constraint.constant = width ? -8 : 0;
  _action_toggle_width_constraint.constant = _actions_collapsed ? WMIOSActionsPillWidth : 44;
  _action_collapsed_width_constraint.constant = WMIOSActionsPillWidth + reserved;
  _action_expanded_width_constraint.constant = 60 + reserved;
  [self updateContextMenuButton];
  _action_rail.hidden = [self actionRailEmpty:width];
  _action_rail.alpha = _action_rail.hidden ? 0 : 1;
  [UIView animateWithDuration:animated && !UIAccessibilityIsReduceMotionEnabled() ? 0.3 : 0
                        delay:0
       usingSpringWithDamping:0.86
        initialSpringVelocity:0
                      options:UIViewAnimationOptionBeginFromCurrentState |
                              UIViewAnimationOptionAllowUserInteraction
                   animations:^{
                     [self->_action_rail layoutIfNeeded];
                   }
                   completion:nil];
  /* Model frames are final here, so the strip targets its settled row position. */
  [self layoutRedoStrip];
}

- (void)toggleActionStrip:(UIButton *)sender
{
  [self setActionsCollapsed:!_actions_collapsed animated:YES];
  if (!_actions_collapsed && _redo_strip.expanded) {
    /* Fold the strip after the actions settle so it lands on the pill's new slot. */
    [_redo_strip collapse];
  }
}

- (void)setActionsCollapsed:(BOOL)collapsed animated:(BOOL)animated
{
  [_action_rail layoutIfNeeded];
  _actions_collapsed = collapsed;
  const NSUInteger generation = ++_action_transition_generation;
  const BOOL animate = animated && !UIAccessibilityIsReduceMotionEnabled();
  _action_expanded_width_constraint.active = NO;
  _action_collapsed_width_constraint.active = NO;
  _action_toggle_width_constraint.active = NO;
  _action_toggle_trailing_constraint.active = NO;
  _action_group_leading_constraint.active = NO;
  _action_group_center_constraint.active = NO;
  _action_group_leading_constraint.active = collapsed;
  _action_group_center_constraint.active = !collapsed;
  _action_expanded_width_constraint.active = !collapsed;
  _action_collapsed_width_constraint.active = collapsed;
  _action_toggle_width_constraint.constant = collapsed ? WMIOSActionsPillWidth : 44;
  _action_toggle_width_constraint.active = YES;
  _action_toggle_trailing_constraint.active = collapsed;

  UIButtonConfiguration *configuration = WMIOSGlassButtonConfiguration(NO);
  configuration.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  configuration.title = collapsed ? @"Actions" : nil;
  configuration.titleLineBreakMode = NSLineBreakByTruncatingTail;
  configuration.image = collapsed ? nil : [UIImage systemImageNamed:@"xmark"];
  configuration.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration
      configurationWithPointSize:13
                          weight:UIImageSymbolWeightSemibold];
  configuration.contentInsets = NSDirectionalEdgeInsetsMake(8, 10, 8, 10);
  /* Dissolve between the title and close icon instead of snapping mid-spring. */
  [UIView transitionWithView:_action_toggle
                    duration:animate ? 0.22 : 0.0
                     options:UIViewAnimationOptionTransitionCrossDissolve |
                             UIViewAnimationOptionBeginFromCurrentState |
                             UIViewAnimationOptionAllowUserInteraction
                  animations:^{
                    self->_action_toggle.configuration = configuration;
                  }
                  completion:nil];
  _action_toggle.titleLabel.numberOfLines = 1;
  _action_toggle.accessibilityLabel = collapsed ? @"Show Quick Actions" : @"Hide Quick Actions";
  _action_toggle.accessibilityValue = collapsed ? @"Collapsed" : @"Expanded";
  _action_toggle.toolTip = _action_toggle.accessibilityLabel;
  [self updateContextMenuButton];
  _action_scroll.hidden = NO;
  _action_scroll.userInteractionEnabled = !collapsed;
  _action_scroll.accessibilityElementsHidden = collapsed;

  /* Keep the native glass control while its bounds change. Individual glass
   * buttons stay inside the scroll clip as the row contracts into the pill. */
  [UIView animateWithDuration:animate ? 0.36 : 0.0
      delay:0.0
      usingSpringWithDamping:0.86
      initialSpringVelocity:0.0
      options:UIViewAnimationOptionBeginFromCurrentState |
              UIViewAnimationOptionAllowUserInteraction
      animations:^{
        [self->_action_rail layoutIfNeeded];
        self->_action_scroll.alpha = collapsed ? 0.0 : 1.0;
        self->_context_menu_button.alpha = collapsed ? 0.0 : 1.0;
        self->_action_scroll.transform = collapsed && animate ?
                                             CGAffineTransformMakeTranslation(-20.0, 0.0) :
                                             CGAffineTransformIdentity;
      }
      completion:^(BOOL finished) {
        if (self->_action_transition_generation == generation) {
          self->_action_scroll.hidden = self->_actions_collapsed;
        }
      }];
}

- (void)updatePhoneActionStripLayout
{
  /* Folding or rotating a phone should narrow the scrolling lane, not lift it
   * into the viewport. Reserve the actual gizmo footprint beside the lane. */
  CGRect safe = _overlay_view.safeAreaLayoutGuide.layoutFrame;
  _action_phone_bottom_constraint.constant = -6.0 - _bottom_tray;
  _action_phone_trailing_constraint.constant = -MAX(
      12.0, CGRectGetMaxX(safe) - CGRectGetMinX(_view_gizmo_hotspot.frame) + 12.0);
}

- (void)installActionStripLayout
{
  const BOOL phone = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone;
  UILayoutGuide *safe = _overlay_view.safeAreaLayoutGuide;
  NSLayoutConstraint *bottom = [_action_rail.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor
                                                                         constant:-6.0];
  NSLayoutConstraint *trailing = [_action_rail.trailingAnchor
      constraintEqualToAnchor:phone ? safe.trailingAnchor : _view_gizmo_hotspot.leadingAnchor
                     constant:-12.0];
  [NSLayoutConstraint activateConstraints:@[
    bottom,
    trailing,
    [_action_rail.leadingAnchor constraintEqualToAnchor:_tool_rail.trailingAnchor constant:12.0],
    [_action_rail.heightAnchor constraintEqualToConstant:56.0],
  ]];
  _action_bottom_constraint = bottom;
  if (phone) {
    _action_phone_bottom_constraint = bottom;
    _action_phone_trailing_constraint = trailing;
    [self updatePhoneActionStripLayout];
  }
}

- (void)handleActionButton:(UIButton *)sender
{
  NSString *op_name = sender.accessibilityIdentifier;
  if ([op_name hasPrefix:@"blender.action."]) {
    WM_apple_ios_ui_setting_enqueue(
        "native_action", [op_name substringFromIndex:@"blender.action.".length].UTF8String);
    return;
  }
  NSString *property = sender.accessibilityValue;
  NSString *value = sender.restorationIdentifier;
  NSString *key = sender.accessibilityHint;
  if ([@[ @"X", @"Y", @"Z" ] containsObject:key ?: @""]) {
    WM_apple_ios_ui_setting_enqueue("constraint_axis", key.UTF8String);
    return;
  }
  if ([key isKindOfClass:[NSString class]] && key.length > 0) {
    [self sendBlenderKeyNamed:key];
    return;
  }

  if (![op_name isKindOfClass:[NSString class]] || op_name.length == 0) {
    return;
  }

  if (![property isKindOfClass:[NSString class]]) {
    property = @"";
  }
  if (![value isKindOfClass:[NSString class]]) {
    value = @"";
  }
  WM_apple_ios_ui_operator_enum_enqueue(op_name.UTF8String, property.UTF8String, value.UTF8String);
}

- (GHOST_TKey)ghostKeyForName:(NSString *)keyName
{
  if ([keyName isEqualToString:@"X"]) {
    return GHOST_kKeyX;
  }
  if ([keyName isEqualToString:@"Y"]) {
    return GHOST_kKeyY;
  }
  if ([keyName isEqualToString:@"Z"]) {
    return GHOST_kKeyZ;
  }
  return GHOST_kKeyUnknown;
}

- (void)sendBlenderKeyNamed:(NSString *)keyName
{
  const GHOST_TKey key = [self ghostKeyForName:keyName];
  if (key == GHOST_kKeyUnknown || _ghost_window == nullptr) {
    return;
  }

  GHOST_ios_window_send_key(_ghost_window, key);
}

@end
