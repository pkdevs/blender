/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * The tool rail: Blender's tools as buttons, their groups, labels and docking.
 */

#import "wm_apple_ios_view_controller.hh"

@implementation WMIOSViewController (Tools)

- (UIButton *)overlayButtonWithTitle:(NSString *)title
                         systemImage:(NSString *)systemImage
                             command:(NSInteger)command
                     clearGlassStyle:(BOOL)clearGlassStyle
{
  UIButton *button = blenderIOSSwiftButton();
  button.tag = command;
  button.tintColor = UIColor.systemBlueColor;
  button.accessibilityLabel = title;
  button.translatesAutoresizingMaskIntoConstraints = NO;

  button.buttonImage = blenderIOSCommandIcon(command, systemImage);
  button.buttonStyle = clearGlassStyle ? @"clear" : @"glass";
  button.foreground = UIColor.systemBlueColor;
  button.buttonInsets = NSDirectionalEdgeInsetsMake(8.0, 8.0, 8.0, 8.0);

  [button addTarget:self
                action:@selector(handleOverlayCommand:)
      forControlEvents:UIControlEventTouchUpInside];

  [NSLayoutConstraint activateConstraints:@[
    [button.widthAnchor constraintEqualToConstant:52.0],
    [button.heightAnchor constraintEqualToConstant:52.0],
  ]];

  return button;
}

- (UIButton *)overlayButtonWithTitle:(NSString *)title
                         systemImage:(NSString *)systemImage
                             command:(NSInteger)command
{
  return [self overlayButtonWithTitle:title
                          systemImage:systemImage
                              command:command
                      clearGlassStyle:YES];
}

- (UIButton *)toolbarButtonWithTitle:(NSString *)title
                         systemImage:(NSString *)systemImage
                      toolIdentifier:(NSString *)toolIdentifier
                                mode:(NSString *)mode
{
  UIButton *button = blenderIOSSwiftButton();
  button.tintColor = UIColor.labelColor;
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;

  UIImageView *group_indicator = [[[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:@"arrowtriangle.down.fill"]] autorelease];
  group_indicator.tag = 9001;
  group_indicator.tintColor = UIColor.secondaryLabelColor;
  group_indicator.translatesAutoresizingMaskIntoConstraints = NO;
  group_indicator.userInteractionEnabled = NO;
  group_indicator.isAccessibilityElement = NO;
  [button addSubview:group_indicator];
  [NSLayoutConstraint activateConstraints:@[
    [group_indicator.widthAnchor constraintEqualToConstant:7.0],
    [group_indicator.heightAnchor constraintEqualToConstant:7.0],
    [group_indicator.trailingAnchor constraintEqualToAnchor:button.trailingAnchor constant:-6.0],
    [group_indicator.bottomAnchor constraintEqualToAnchor:button.bottomAnchor constant:-6.0],
  ]];
  [self configureToolbarButton:button
                          tool:@{
                            @"id" : toolIdentifier,
                            @"title" : title,
                            @"symbol" : systemImage ?: @""
                          }
                          mode:mode];

  [button addTarget:self
                action:@selector(handleToolbarToolButton:)
      forControlEvents:UIControlEventTouchUpInside];

  UILongPressGestureRecognizer *press_select_gesture = [[UILongPressGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleToolbarPressSelectGesture:)];
  press_select_gesture.minimumPressDuration = 0.22;
  press_select_gesture.allowableMovement = 10.0;
  press_select_gesture.cancelsTouchesInView = YES;
  press_select_gesture.delegate = self;
  [button addGestureRecognizer:press_select_gesture];
  [press_select_gesture release];

  [NSLayoutConstraint activateConstraints:@[
    [button.heightAnchor constraintEqualToConstant:52.0],
  ]];

  [_toolbar_buttons addObject:button];
  [button setNeedsUpdateConfiguration];
  return button;
}

- (UIImage *)toolbarImageForTool:(NSDictionary *)tool mode:(NSString *)mode
{
  return [self toolbarImageForTool:tool mode:mode selected:NO];
}

- (UIImage *)toolbarImageForTool:(NSDictionary *)tool mode:(NSString *)mode selected:(BOOL)selected
{
  NSString *icon = tool[@"icon"];
  if ([icon isKindOfClass:NSString.class]) {
    NSString *prefix = selected ? @"blender.tool.selected." : @"blender.tool.color.";
    UIImage *colored = [UIImage imageNamed:[prefix stringByAppendingString:icon]];
    if (colored) {
      return [colored imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
    }
  }
  UIImage *image = [icon isKindOfClass:NSString.class] ?
                       [UIImage imageNamed:[@"blender.tool." stringByAppendingString:icon]] :
                       nil;
  if (image) {
    return [image imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  }
  NSString *toolIdentifier = tool[@"id"];
  NSString *prefix = selected ? @"selected.tool." : @"tool.";
  image = blenderIOSIcon([NSString stringWithFormat:@"%@%@.%@", prefix, mode, toolIdentifier],
                         nil);
  if (image == nil) {
    NSString *symbol = tool[@"symbol"];
    image = blenderIOSIcon([prefix stringByAppendingString:toolIdentifier],
                           symbol.length ? symbol : nil);
  }
  return image ?: [UIImage systemImageNamed:@"questionmark.circle"];
}

- (void)configureToolbarButton:(UIButton *)button tool:(NSDictionary *)tool mode:(NSString *)mode
{
  NSString *title = tool[@"title"];
  button.accessibilityLabel = title;
  button.accessibilityIdentifier = tool[@"id"];
  UIImage *image = [self toolbarImageForTool:tool mode:mode];
  UIImage *selected_image = [self toolbarImageForTool:tool mode:mode selected:YES];
  NSArray *children = tool[@"children"];
  const BOOL grouped = children.count > 1;
  [button viewWithTag:9001].hidden = !grouped;
  button.accessibilityHint = grouped ? @"Touch and hold for related tools" : nil;
  button.menu = grouped ? [self toolbarMenuForTool:tool mode:mode] : nil;
  /* Assigning a menu re-enables its hold interaction. */
  [self updateToolbarMenuAvailabilityForButton:button];
  button.showsMenuAsPrimaryAction = NO;
  button.preferredMenuElementOrder = UIContextMenuConfigurationElementOrderFixed;

  button.configurationUpdateHandler = ^(UIButton *updated_button) {
    const BOOL shows_text = self->_toolbar_expanded;
    const BOOL is_emphasized = [self isToolbarListVisible] &&
                               (updated_button.selected ||
                                updated_button == self->_toolbar_hover_button);
    ((UIImageView *)[updated_button viewWithTag:9001]).tintColor = updated_button.selected ?
                                                                       UIColor.whiteColor :
                                                                       UIColor.secondaryLabelColor;
    updated_button.contentHorizontalAlignment = shows_text ?
                                                    UIControlContentHorizontalAlignmentLeading :
                                                    UIControlContentHorizontalAlignmentCenter;
    /* Glass only while hovered in the open list, the title beside the icon while the rail is
     * expanded. */
    updated_button.buttonStyle = is_emphasized && !updated_button.selected ? @"glass" : @"plain";
    updated_button.selectedTint = UIColor.clearColor;
    updated_button.foreground = UIColor.labelColor;
    updated_button.selectedForeground = UIColor.whiteColor;
    updated_button.buttonTitle = shows_text ? title : nil;
    updated_button.buttonImage = updated_button.selected ? selected_image : image;
    updated_button.imagePadding = shows_text ? 10.0 : 0.0;
    updated_button.buttonInsets = shows_text ? NSDirectionalEdgeInsetsMake(8.0, 12.0, 8.0, 14.0) :
                                               NSDirectionalEdgeInsetsMake(8.0, 8.0, 8.0, 8.0);
    updated_button.contentLeading = shows_text;
  };

  [button setNeedsUpdateConfiguration];
}

- (UIMenu *)toolbarMenuForTool:(NSDictionary *)tool mode:(NSString *)mode
{
  NSMutableArray *sections = [NSMutableArray array];
  NSMutableArray *actions = [NSMutableArray array];
  for (id child in tool[@"children"]) {
    if (![child isKindOfClass:NSDictionary.class]) {
      if (actions.count) {
        [sections addObject:[UIMenu menuWithTitle:@""
                                            image:nil
                                       identifier:nil
                                          options:UIMenuOptionsDisplayInline
                                         children:actions]];
        actions = [NSMutableArray array];
      }
      continue;
    }
    UIAction *action = [UIAction
        actionWithTitle:child[@"title"]
                  image:[self toolbarImageForTool:child mode:mode]
             identifier:child[@"id"]
                handler:^(UIAction *selected) {
                  UIButton *chosen_button = nil;
                  /* Resolve the current button: snapshots may have replaced the menu's original
                   * anchor. */
                  for (UIButton *button in self->_toolbar_buttons) {
                    if ([button.menu.identifier isEqualToString:tool[@"group"]]) {
                      NSMutableDictionary *updated = [NSMutableDictionary
                          dictionaryWithDictionary:tool];
                      [updated addEntriesFromDictionary:child];
                      [self configureToolbarButton:button tool:updated mode:mode];
                      chosen_button = button;
                      break;
                    }
                  }
                  [self setActiveToolbarToolIdentifier:selected.identifier];
                  WM_apple_ios_ui_tool_set_by_id(selected.identifier.UTF8String);
                  [self showToolbarLabelForButton:chosen_button transient:YES];
                }];
    action.state = [child[@"id"] isEqualToString:tool[@"id"]] ? UIMenuElementStateOn :
                                                                UIMenuElementStateOff;
    [actions addObject:action];
  }
  if (actions.count) {
    [sections addObject:[UIMenu menuWithTitle:@""
                                        image:nil
                                   identifier:nil
                                      options:UIMenuOptionsDisplayInline
                                     children:actions]];
  }
  return [UIMenu menuWithTitle:@""
                         image:nil
                    identifier:tool[@"group"]
                       options:0
                      children:sections];
}

- (void)updateToolbarMenuAvailabilityForButton:(UIButton *)button
{
  button.contextMenuInteractionEnabled = button.menu != nil && [self isToolbarListVisible] &&
                                         !_toolbar_press_select_active;
}

- (BOOL)isToolbarListVisible
{
  return _toolbar_expanded || _tool_rail_docked_left_center;
}

- (void)setActiveToolbarToolIdentifier:(NSString *)toolIdentifier
{
  /* JSON decoding creates new strings each poll. An unchanged tool must not force
   * overlay layout while UIKit is animating a menu back into its glass group. */
  if ([_active_tool_identifier isEqualToString:toolIdentifier]) {
    return;
  }
  [_active_tool_identifier release];
  _active_tool_identifier = [toolIdentifier copy];
#if defined(APPLE_TARGET_VISIONOS)
  [_vision_chrome setActiveTool:toolIdentifier ?: @""];
#endif

  [self updateToolbarVisibilityAnimated:NO];
  /* Resolve selection artwork in this event, before UIKit's glass transition. */
  [UIView performWithoutAnimation:^{
    for (UIButton *button in self->_toolbar_buttons) {
      [button updateConfiguration];
    }
  }];
}

#if defined(APPLE_TARGET_VISIONOS)
/* The tools as a visionOS ornament along the window's leading edge, in place of the rail. */
- (void)updateVisionToolPalette:(NSArray *)tools mode:(NSString *)mode
{
  if (!_vision_chrome) {
    return;
  }
  NSMutableDictionary *images = [NSMutableDictionary dictionary];
  NSMutableDictionary *selected_images = [NSMutableDictionary dictionary];
  NSMutableDictionary *menus = [NSMutableDictionary dictionary];
  for (NSDictionary *tool in tools) {
    if (![tool isKindOfClass:NSDictionary.class] || ![tool[@"id"] isKindOfClass:NSString.class]) {
      continue;
    }
    images[tool[@"id"]] = [self toolbarImageForTool:tool mode:mode];
    selected_images[tool[@"id"]] = [self toolbarImageForTool:tool mode:mode selected:YES];
    if ([tool[@"children"] count] > 1) {
      menus[tool[@"id"]] = [self toolbarMenuForTool:tool mode:mode];
    }
  }
  [_vision_chrome updateTools:tools images:images selectedImages:selected_images menus:menus];
  [_vision_chrome setActiveTool:_active_tool_identifier ?: @""];
}
#endif

- (CGFloat)expandedToolbarHeight
{
  UILayoutGuide *safe_area = _overlay_view.safeAreaLayoutGuide;
  CGFloat safe_height = safe_area.layoutFrame.size.height;
  if (safe_height <= 0.0) {
    safe_height = self.view.bounds.size.height;
  }
  return MIN(MAX(64.0, safe_height * 0.58), MAX(64.0, safe_height - 112.0));
}

- (void)updateToolbarHeightForCurrentState
{
  const BOOL list_visible = [self isToolbarListVisible];
  _tool_rail_width_constraint.constant = _toolbar_expanded ? [self expandedToolbarWidth] : 64.0;
  _tool_rail_height_constraint.constant = list_visible ? [self expandedToolbarHeight] : 64.0;
  _tool_drag_handle.hidden = !list_visible;
  _tool_drag_handle_height_constraint.constant = list_visible ? 44.0 : 0.0;
  _tool_scroll.scrollEnabled = list_visible;
  [self updateBrushRailGeometry];
}

- (CGFloat)expandedToolbarWidth
{
  /* Leave a usable shortcut lane beside the gizmo even in a narrow iPad window. */
  CGFloat safe_width = self.view.safeAreaLayoutGuide.layoutFrame.size.width;
  if (safe_width <= 0.0) {
    safe_width = self.view.bounds.size.width;
  }
  return MIN(240.0, MAX(64.0, safe_width - 284.0));
}

- (CGFloat)toolRailBottomDockCenterY
{
  CGRect safe_frame = _overlay_view.safeAreaLayoutGuide.layoutFrame;
  CGFloat rail_height = _tool_rail_height_constraint.constant > 0.0 ?
                            _tool_rail_height_constraint.constant :
                            _tool_rail.bounds.size.height;
  return CGRectGetMaxY(safe_frame) - 20.0 - rail_height * 0.5;
}

- (CGFloat)toolRailCenterDockCenterY
{
  return CGRectGetMidY(_overlay_view.safeAreaLayoutGuide.layoutFrame);
}

- (void)setToolRailDockedLeftCenter:(BOOL)dockedLeftCenter animated:(BOOL)animated
{
  if (_tool_rail_bottom_constraint == nil || _tool_rail_center_y_constraint == nil) {
    return;
  }

  _tool_rail_docked_left_center = dockedLeftCenter;
  _tool_rail_bottom_constraint.active = !dockedLeftCenter;
  _tool_rail_center_y_constraint.active = dockedLeftCenter;
  [self updateToolbarVisibilityAnimated:animated];

  void (^applyDock)(void) = ^{
    self->_tool_rail.transform = CGAffineTransformIdentity;
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

- (void)handleToolbarDockPan:(UIPanGestureRecognizer *)gesture
{
  if ([self isToolbarListVisible]) {
    return;
  }

  CGPoint translation = [gesture translationInView:_overlay_view];

  if (gesture.state == UIGestureRecognizerStateChanged) {
    _tool_rail.transform = CGAffineTransformMakeTranslation(0.0, translation.y);
    return;
  }

  if (gesture.state != UIGestureRecognizerStateEnded &&
      gesture.state != UIGestureRecognizerStateCancelled &&
      gesture.state != UIGestureRecognizerStateFailed)
  {
    return;
  }

  CGPoint velocity = [gesture velocityInView:_overlay_view];
  CGRect rail_frame = [_tool_rail.superview convertRect:_tool_rail.bounds fromView:_tool_rail];
  CGFloat final_center_y = CGRectGetMidY(rail_frame) + translation.y;
  CGFloat center_y = [self toolRailCenterDockCenterY];
  CGFloat bottom_y = [self toolRailBottomDockCenterY];
  BOOL dock_left_center = fabs(final_center_y - center_y) < fabs(final_center_y - bottom_y);

  if (velocity.y < -520.0) {
    dock_left_center = YES;
  }
  else if (velocity.y > 520.0) {
    dock_left_center = NO;
  }

  [self setToolRailDockedLeftCenter:dock_left_center animated:YES];
}

- (UIButton *)toolbarDragHandle
{
  UIButton *handle = [UIButton buttonWithType:UIButtonTypeCustom];
  handle.translatesAutoresizingMaskIntoConstraints = NO;
  handle.accessibilityLabel = @"Collapse Toolbar";
  handle.accessibilityIdentifier = @"blender.toolbar.handle";
  [handle addInteraction:[[[UIToolTipInteraction alloc] initWithDefaultToolTip:@"Collapse Toolbar"]
                             autorelease]];
  [handle addTarget:self
                action:@selector(collapseToolbarFromHandle:)
      forControlEvents:UIControlEventTouchUpInside];
  UIView *grip = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  grip.translatesAutoresizingMaskIntoConstraints = NO;
  grip.userInteractionEnabled = NO;
  grip.backgroundColor = UIColor.secondaryLabelColor;
  grip.layer.cornerRadius = 2.5;
  [handle addSubview:grip];
  [NSLayoutConstraint activateConstraints:@[
    [grip.centerXAnchor constraintEqualToAnchor:handle.centerXAnchor],
    [grip.centerYAnchor constraintEqualToAnchor:handle.centerYAnchor],
    [grip.widthAnchor constraintEqualToConstant:28.0],
    [grip.heightAnchor constraintEqualToConstant:5.0],
  ]];
  UIPanGestureRecognizer *pan = [[UIPanGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleToolbarHandlePan:)];
  pan.maximumNumberOfTouches = 1;
  pan.delegate = self;
  [handle addGestureRecognizer:pan];
  [pan release];
  return handle;
}

- (void)collapseToolbarFromHandle:(id)sender
{
  [self hideToolbarLabelAnimated:YES];
  UNUSED_VARS(sender);
  _toolbar_press_select_active = NO;
  _toolbar_expanded = NO;
  [self setToolbarHoverButton:nil];
  [self setToolRailDockedLeftCenter:NO animated:YES];
}

- (void)handleToolbarHandlePan:(UIPanGestureRecognizer *)gesture
{
  CGPoint translation = [gesture translationInView:_overlay_view];
  /* A temporarily opened rail sits at the bottom dock and follows the finger up toward the
   * higher dock; a docked rail only resists a downward pull. */
  const BOOL can_raise = !_tool_rail_docked_left_center;
  const CGFloat raise_distance = MAX(
      0.0, [self toolRailBottomDockCenterY] - [self toolRailCenterDockCenterY]);
  if (gesture.state == UIGestureRecognizerStateChanged) {
    const CGFloat offset = translation.y < 0.0 && can_raise ?
                               MAX(-raise_distance, translation.y) :
                               MIN(32.0, MAX(0.0, translation.y * 0.3));
    _tool_rail.transform = CGAffineTransformMakeTranslation(0.0, offset);
  }
  else if (gesture.state == UIGestureRecognizerStateEnded ||
           gesture.state == UIGestureRecognizerStateCancelled ||
           gesture.state == UIGestureRecognizerStateFailed)
  {
    CGPoint velocity = [gesture velocityInView:_overlay_view];
    if (gesture.state == UIGestureRecognizerStateEnded && can_raise && raise_distance > 0.0 &&
        (-translation.y > raise_distance * 0.5 || (translation.y < -8.0 && velocity.y < -420.0)))
    {
      /* Keep the list open, now as the docked rail rather than a temporary peek. */
      [self hideToolbarLabelAnimated:YES];
      _toolbar_press_select_active = NO;
      _toolbar_expanded = NO;
      [self setToolbarHoverButton:nil];
      [self setToolRailDockedLeftCenter:YES animated:YES];
    }
    else if (gesture.state == UIGestureRecognizerStateEnded &&
             (translation.y > 36.0 || (translation.y > 8.0 && velocity.y > 420.0)))
    {
      [self collapseToolbarFromHandle:gesture];
    }
    else {
      [UIView animateWithDuration:0.2
                       animations:^{
                         self->_tool_rail.transform = CGAffineTransformIdentity;
                       }];
    }
  }
}

- (void)setToolbarGeometryExpanded:(BOOL)expanded
{
  const BOOL list_visible = expanded || _tool_rail_docked_left_center;
  _tool_rail_width_constraint.constant = expanded ? [self expandedToolbarWidth] : 64.0;
  _tool_rail_height_constraint.constant = list_visible ? [self expandedToolbarHeight] : 64.0;
  _tool_rail.effect = _tool_rail_glass_effect;
}

- (void)updateToolbarVisibilityAnimated:(BOOL)animated
{
  const BOOL list_visible = [self isToolbarListVisible];
  for (UIButton *button in _toolbar_buttons) {
    [self updateToolbarMenuAvailabilityForButton:button];
  }
  void (^prepareExpanded)(void) = ^{
    for (UIButton *button in _toolbar_buttons) {
      button.hidden = NO;
      [button setNeedsUpdateConfiguration];
      if (list_visible && button.alpha < 1.0) {
        button.alpha = 0.0;
      }
    }
  };

  __block UIButton *fallback_button = nil;
  __block BOOL has_active_button = NO;
  for (UIButton *button in _toolbar_buttons) {
    if (fallback_button == nil) {
      fallback_button = button;
    }
    if ([button.accessibilityIdentifier isEqualToString:_active_tool_identifier]) {
      has_active_button = YES;
    }
  }

  void (^applyVisibility)(void) = ^{
    for (UIButton *button in _toolbar_buttons) {
      const BOOL is_active = has_active_button ? [button.accessibilityIdentifier
                                                     isEqualToString:_active_tool_identifier] :
                                                 button == fallback_button;
      button.selected = list_visible && is_active;
      [button setNeedsUpdateConfiguration];
      button.alpha = (list_visible || is_active) ? 1.0 : 0.0;
    }
    [_tool_stack setNeedsLayout];
  };

  void (^finishCompact)(void) = ^{
    if ([self isToolbarListVisible]) {
      return;
    }
    for (UIButton *button in _toolbar_buttons) {
      const BOOL is_active = has_active_button ? [button.accessibilityIdentifier
                                                     isEqualToString:_active_tool_identifier] :
                                                 button == fallback_button;
      button.selected = NO;
      button.hidden = !is_active;
      [button setNeedsUpdateConfiguration];
    }
    [_tool_scroll setContentOffset:CGPointZero animated:NO];
  };

  prepareExpanded();
  [self updateToolbarHeightForCurrentState];

  if (!animated) {
    applyVisibility();
    finishCompact();
    [_overlay_view layoutIfNeeded];
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
        [_overlay_view layoutIfNeeded];
      }
      completion:^(BOOL finished) {
        finishCompact();
      }];
}

- (void)setToolbarExpanded:(BOOL)expanded animated:(BOOL)animated
{
  if (_toolbar_expanded == expanded && animated) {
    return;
  }

  _toolbar_expanded = expanded;
  [self hideToolbarLabelAnimated:NO];
  [self updateToolbarVisibilityAnimated:animated];
}

- (NSDictionary *)toolbarModel
{
  char *json = WM_apple_ios_ui_toolbar_copy_json();
  if (json == nullptr) {
    return nil;
  }

  NSData *data = [NSData dataWithBytes:json length:strlen(json)];
  WM_apple_ios_ui_toolbar_free_json(json);
  if (data == nil) {
    return nil;
  }

  NSError *error = nil;
  id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
  if (error != nil || ![object isKindOfClass:[NSDictionary class]]) {
    return nil;
  }
  return (NSDictionary *)object;
}

- (NSString *)toolbarSignatureForTools:(NSArray *)tools mode:(NSString *)mode
{
  NSData *data = [NSJSONSerialization dataWithJSONObject:tools
                                                 options:NSJSONWritingSortedKeys
                                                   error:nil];
  NSString *contents = [[[NSString alloc] initWithData:data
                                              encoding:NSUTF8StringEncoding] autorelease];
  return [NSString stringWithFormat:@"%@:%@", mode ?: @"", contents ?: @""];
}

- (void)rebuildToolbarWithTools:(NSArray *)tools mode:(NSString *)mode
{
  NSString *label_tool = [[_toolbar_label_button.accessibilityIdentifier copy] autorelease];
  _toolbar_label_button = nil;
  NSArray *arranged_subviews = [[_tool_stack.arrangedSubviews copy] autorelease];
  for (UIView *view in arranged_subviews) {
    [_tool_stack removeArrangedSubview:view];
    [view removeFromSuperview];
  }
  [_toolbar_buttons removeAllObjects];
  _toolbar_hover_button = nil;

  for (NSDictionary *tool in tools) {
    if (![tool isKindOfClass:[NSDictionary class]]) {
      UIView *previous = _tool_stack.arrangedSubviews.lastObject;
      if (previous) {
        [_tool_stack setCustomSpacing:12.0 afterView:previous];
      }
      continue;
    }
    NSString *title = tool[@"title"];
    NSString *tool_id = tool[@"id"];
    NSString *symbol = tool[@"symbol"];
    if (![title isKindOfClass:[NSString class]] || ![tool_id isKindOfClass:[NSString class]] ||
        ![symbol isKindOfClass:[NSString class]] || title.length == 0 || tool_id.length == 0)
    {
      continue;
    }
    UIButton *button = [self toolbarButtonWithTitle:title
                                        systemImage:symbol
                                     toolIdentifier:tool_id
                                               mode:mode];
    [self configureToolbarButton:button tool:tool mode:mode];
    [_tool_stack addArrangedSubview:button];
    if ([button.accessibilityIdentifier isEqualToString:label_tool]) {
      _toolbar_label_button = button;
    }
  }
  [self updateToolbarVisibilityAnimated:NO];
#if defined(APPLE_TARGET_VISIONOS)
  [self updateVisionToolPalette:tools mode:mode];
#endif
  if (_toolbar_label_button) {
    [self layoutToolbarLabel];
  }
  else {
    [self hideToolbarLabelAnimated:NO];
  }
}

- (void)handleToolbarToolButton:(UIButton *)sender
{
  NSString *tool_id = sender.accessibilityIdentifier;
  if (![tool_id isKindOfClass:[NSString class]] || tool_id.length == 0) {
    return;
  }
  if (![self isToolbarListVisible]) {
    /* A tap on the collapsed rail opens it to pick a different tool. */
    [self setToolbarExpanded:YES animated:YES];
    return;
  }

  [self setActiveToolbarToolIdentifier:tool_id];
  WM_apple_ios_ui_tool_set_by_id(tool_id.UTF8String);
  if (_toolbar_expanded && !_tool_rail_docked_left_center) {
    /* Opened by a tap: picking closes it again. A docked list stays open. */
    [self setToolbarExpanded:NO animated:YES];
  }
  [self showToolbarLabelForButton:sender transient:YES];
}

- (void)layoutToolbarLabel
{
  UIButton *button = _toolbar_label_button;
  if (!button || !button.window || button.hidden || _toolbar_label_bubble.hidden) {
    return;
  }
  CGRect anchor = [button convertRect:button.bounds toView:_overlay_view];
  UIView *rail_view = [button isDescendantOfView:_brush_rail] ? _brush_rail : _tool_rail;
  CGRect rail = [rail_view convertRect:rail_view.bounds toView:_overlay_view];
  CGRect safe = CGRectInset(_overlay_view.safeAreaLayoutGuide.layoutFrame, 8, 8);
  CGFloat x = CGRectGetMaxX(rail) + 10;
  CGFloat max_width = MAX(44.0, CGRectGetMaxX(safe) - x);
  CGSize text_size = [_toolbar_label
      sizeThatFits:CGSizeMake(MAX(16.0, max_width - 28), CGFLOAT_MAX)];
  CGSize size = CGSizeMake(MIN(max_width, ceil(text_size.width) + 28),
                           ceil(text_size.height) + 16);
  CGFloat y = MIN(CGRectGetMaxY(safe) - size.height,
                  MAX(CGRectGetMinY(safe), CGRectGetMidY(anchor) - size.height / 2));
  _toolbar_label_bubble.bounds = CGRectMake(0, 0, size.width, size.height);
  _toolbar_label_bubble.center = CGPointMake(x + size.width / 2, y + size.height / 2);
  _toolbar_label_bubble.layer.cornerRadius = MIN(20.0, size.height / 2);
  _toolbar_label.frame = CGRectInset(_toolbar_label_bubble.bounds, 14, 8);
}

- (void)hideToolbarLabelAnimated:(BOOL)animated
{
  const NSUInteger generation = ++_toolbar_label_generation;
  _toolbar_label_button = nil;
  _toolbar_label_transient = NO;
  [UIView animateWithDuration:animated && !UIAccessibilityIsReduceMotionEnabled() ? 0.18 : 0
      delay:0
      options:UIViewAnimationOptionBeginFromCurrentState |
              UIViewAnimationOptionAllowUserInteraction
      animations:^{
        self->_toolbar_label_bubble.alpha = 0;
      }
      completion:^(BOOL finished) {
        if (self->_toolbar_label_generation == generation) {
          self->_toolbar_label_bubble.hidden = YES;
        }
      }];
}

- (void)showToolbarLabelForButton:(UIButton *)button transient:(BOOL)transient
{
  if (!button.accessibilityLabel.length) {
    return;
  }
  if (!_toolbar_label_bubble) {
    _toolbar_label_bubble = [[[UIVisualEffectView alloc] initWithEffect:WMIOSGlassEffect(NO, NO)]
        autorelease];
    _toolbar_label_bubble.userInteractionEnabled = NO;
    _toolbar_label_bubble.clipsToBounds = YES;
    _toolbar_label_bubble.accessibilityIdentifier = @"blender.toolbar.tool-name";
    _toolbar_label_bubble.alpha = 0;
    _toolbar_label = [[[UILabel alloc] initWithFrame:CGRectZero] autorelease];
    _toolbar_label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    _toolbar_label.adjustsFontForContentSizeCategory = YES;
    _toolbar_label.numberOfLines = 0;
    _toolbar_label.textColor = UIColor.labelColor;
    [_toolbar_label_bubble.contentView addSubview:_toolbar_label];
    [_overlay_view addSubview:_toolbar_label_bubble];
  }
  const BOOL unchanged_hover = !transient && !_toolbar_label_transient &&
                               _toolbar_label_button == button && !_toolbar_label_bubble.hidden;
  _toolbar_label_button = button;
  _toolbar_label_transient = transient;
  _toolbar_label.text = button.accessibilityLabel;
  _toolbar_label_bubble.hidden = NO;
  [self layoutToolbarLabel];
  if (unchanged_hover) {
    return;
  }
  const NSUInteger generation = ++_toolbar_label_generation;
  if (_toolbar_label_bubble.alpha == 0 && !UIAccessibilityIsReduceMotionEnabled()) {
    _toolbar_label_bubble.transform = CGAffineTransformMakeTranslation(-5, 0);
  }
  [_overlay_view bringSubviewToFront:_toolbar_label_bubble];
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.18
                        delay:0
                      options:UIViewAnimationOptionBeginFromCurrentState |
                              UIViewAnimationOptionAllowUserInteraction
                   animations:^{
                     self->_toolbar_label_bubble.alpha = 1;
                     self->_toolbar_label_bubble.transform = CGAffineTransformIdentity;
                   }
                   completion:nil];
  if (transient) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(0.9 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(),
                   ^{
                     if (self->_toolbar_label_generation == generation) {
                       [self hideToolbarLabelAnimated:YES];
                     }
                   });
  }
}

- (void)handleToolbarHover:(UIHoverGestureRecognizer *)gesture
{
  UIButton *hovered = nil;
  if (gesture.state == UIGestureRecognizerStateBegan ||
      gesture.state == UIGestureRecognizerStateChanged)
  {
    CGPoint location = [gesture locationInView:_tool_scroll];
    if (CGRectContainsPoint(_tool_scroll.bounds, location)) {
      UIView *hit = [_tool_scroll hitTest:location withEvent:nil];
      while (hit && hit != _tool_scroll) {
        if ([_toolbar_buttons containsObject:hit]) {
          hovered = (UIButton *)hit;
          break;
        }
        hit = hit.superview;
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

- (UIButton *)toolbarButtonAtGestureLocation:(UIGestureRecognizer *)gesture
{
  CGPoint location = [gesture locationInView:_overlay_view];
  for (UIButton *button in _toolbar_buttons) {
    if (button.hidden || button.alpha < 0.05) {
      continue;
    }

    CGPoint local_point = [button convertPoint:location fromView:_overlay_view];
    CGRect hit_bounds = CGRectInset(button.bounds, -10.0, -6.0);
    if (CGRectContainsPoint(hit_bounds, local_point)) {
      return button;
    }
  }
  return nil;
}

- (void)setToolbarHoverButton:(UIButton *)hoverButton
{
  if (_toolbar_hover_button == hoverButton) {
    return;
  }

  UIButton *previous_hover_button = _toolbar_hover_button;
  _toolbar_hover_button = hoverButton;

  previous_hover_button.highlighted = NO;
  [previous_hover_button setNeedsUpdateConfiguration];

  _toolbar_hover_button.highlighted = YES;
  [_toolbar_hover_button setNeedsUpdateConfiguration];
}

- (void)finishToolbarPressSelectWithButton:(UIButton *)button
{
  [self setToolbarHoverButton:nil];

  NSString *tool_id = button.accessibilityIdentifier;
  if ([tool_id isKindOfClass:[NSString class]] && tool_id.length > 0) {
    [self setActiveToolbarToolIdentifier:tool_id];
    WM_apple_ios_ui_tool_set_by_id(tool_id.UTF8String);
  }

  [self setToolbarExpanded:NO animated:YES];
  if (button) {
    [self showToolbarLabelForButton:button transient:YES];
  }
}

- (void)handleToolbarPressSelectGesture:(UILongPressGestureRecognizer *)gesture
{
  switch (gesture.state) {
    case UIGestureRecognizerStateBegan:
      if (_toolbar_expanded) {
        return;
      }
      _toolbar_press_select_active = YES;
      [self setToolbarExpanded:YES animated:YES];
      [self setToolbarHoverButton:[self toolbarButtonAtGestureLocation:gesture]];
      break;
    case UIGestureRecognizerStateChanged:
      if (!_toolbar_press_select_active) {
        return;
      }
      [self setToolbarHoverButton:[self toolbarButtonAtGestureLocation:gesture]];
      break;
    case UIGestureRecognizerStateEnded:
      if (!_toolbar_press_select_active) {
        return;
      }
      _toolbar_press_select_active = NO;
      [self finishToolbarPressSelectWithButton:[self toolbarButtonAtGestureLocation:gesture] ?:
                                                   _toolbar_hover_button];
      break;
    case UIGestureRecognizerStateCancelled:
    case UIGestureRecognizerStateFailed:
      if (!_toolbar_press_select_active) {
        return;
      }
      _toolbar_press_select_active = NO;
      [self setToolbarHoverButton:nil];
      [self setToolbarExpanded:NO animated:YES];
      break;
    default:
      break;
  }
}

@end
