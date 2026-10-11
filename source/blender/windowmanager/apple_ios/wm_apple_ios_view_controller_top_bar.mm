/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * The top bar: documents, mode, undo, shading, tool settings, selection mode and menus.
 */

#import "wm_apple_ios_view_controller.hh"

@implementation WMIOSViewController (TopBar)

- (UIBarButtonItem *)documentsButton
{
  UIBarButtonItem *button = [[[UIBarButtonItem alloc]
      initWithImage:[UIImage systemImageNamed:@"chevron.backward"]
              style:UIBarButtonItemStylePlain
             target:self
             action:@selector(showDocuments:)] autorelease];
  button.title = @"Documents";
  button.accessibilityLabel = @"Documents";
  button.accessibilityIdentifier = @"blender.documents";
  button.identifier = @"blender.documents";
  return button;
}

- (void)refreshNativeMenuBar
{
  if (_native_menubar_request) {
    NSArray *items = blenderIOSMenuResult(_native_menubar_request);
    if (!items) {
      return;
    }
    _native_menubar_request = 0;
    if (![_native_menubar_items isEqualToArray:items]) {
      [_native_menubar_items release];
      _native_menubar_items = [items copy];
      [UIMenuSystem.mainSystem setNeedsRebuild];
    }
  }
  /* Projecting all of Blender's menus is costly and they change with the mode, rarely
   * otherwise: ask again when the mode changes, and every few seconds for the rest. */
  static CFAbsoluteTime last_request = 0;
  static NSString *last_mode = nil;
  const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
  if ([last_mode isEqual:_object_mode] && now - last_request < 3.0) {
    return;
  }
  last_request = now;
  [last_mode release];
  last_mode = [_object_mode copy];
  _native_menubar_request = WM_apple_ios_ui_menu_request("{\"root\":\"menubar\",\"path\":[]}",
                                                         false);
}

- (void)setFingerInputMode:(BOOL)useTool
{
  _finger_tool_enabled = useTool;
  [(UIWindow<GHOST_IOSInputWindow> *)self.view.window setFingerToolEnabled:useTool];
  _finger_input_button.image = useTool ?
                                   blenderIOSIcon(@"control.finger_tool", @"cursorarrow") :
                                   blenderIOSIcon(@"operator.VIEW3D_OT_rotate", @"rotate.3d");
  _finger_input_button.accessibilityValue = useTool ? @"Use Tool" : @"Orbit View";
  NSMutableArray *actions = [NSMutableArray array];
  for (UIAction *item in _finger_input_button.menu.children) {
    UIAction *action = [[item copy] autorelease];
    action.state = [action.identifier isEqualToString:(useTool ? @"tool" : @"orbit")] ?
                       UIMenuElementStateOn :
                       UIMenuElementStateOff;
    [actions addObject:action];
  }
  _finger_input_button.menu = [_finger_input_button.menu menuByReplacingChildren:actions];
  _finger_input_button.menuRepresentation = _finger_input_button.menu;
}

- (UIBarButtonItem *)fingerInputButton
{
  UIAction *tool = [UIAction actionWithTitle:@"Use Tool"
                                       image:blenderIOSIcon(@"control.finger_tool", @"cursorarrow")
                                  identifier:@"tool"
                                     handler:^(UIAction *action) {
                                       [self setFingerInputMode:YES];
                                     }];
  UIAction *orbit = [UIAction
      actionWithTitle:@"Orbit View"
                image:blenderIOSIcon(@"operator.VIEW3D_OT_rotate", @"rotate.3d")
           identifier:@"orbit"
              handler:^(UIAction *action) {
                [self setFingerInputMode:NO];
              }];
  UIBarButtonItem *button = [[[UIBarButtonItem alloc]
      initWithImage:tool.image
               menu:[UIMenu menuWithTitle:@"One Finger" children:@[ tool, orbit ]]] autorelease];
  button.title = @"One Finger";
  button.accessibilityLabel = button.title;
  button.identifier = @"blender.finger-input";
  return button;
}

- (UIAction *)modeActionWithTitle:(NSString *)title command:(NSInteger)command
{
  return [UIAction actionWithTitle:title
                             image:blenderIOSCommandIcon(command, nil)
                        identifier:nil
                           handler:^(__kindof UIAction *action) {
                             self->_mode_button.title = title;
                             self->_mode_button.image = action.image;
                             self->_mode_button.accessibilityValue = title;
                             WM_apple_ios_ui_command_enqueue((int)command);
                           }];
}

/* Lists the same context-dependent modes as Blender's header (OBJECT_OT_mode_set), so
 * Grease Pencil, curves, armatures and other object types get their own modes. */
- (void)setModePickerModes:(NSArray *)modes current:(NSString *)current
{
  if (_mode_button == nil) {
    return;
  }
  if (![current isKindOfClass:NSString.class]) {
    current = @"OBJECT";
  }
  if ([_mode_items isEqualToArray:modes] && [_object_mode isEqualToString:current]) {
    return;
  }
  [_mode_items release];
  _mode_items = [modes copy];
  [_object_mode release];
  _object_mode = [current copy];

  NSMutableArray *actions = [NSMutableArray array];
  UIAction *selected = nil;
  for (NSDictionary *item in modes) {
    if (![item isKindOfClass:NSDictionary.class] || ![item[@"id"] isKindOfClass:NSString.class]) {
      continue;
    }
    NSString *identifier = item[@"id"];
    NSString *icon = [item[@"icon"] isKindOfClass:NSString.class] ? item[@"icon"] : @"";
    UIImage *image = WMIOSBlenderIcon(icon, NO);
    NSString *title = [item[@"label"] length] ? item[@"label"] : identifier;
    UIAction *action = [UIAction
        actionWithTitle:title
                  image:image
             identifier:[@"blender.mode." stringByAppendingString:identifier]
                handler:^(__kindof UIAction *sender) {
                  self->_mode_button.title = sender.title;
                  self->_mode_button.image = sender.image;
                  self->_mode_button.accessibilityValue = sender.title;
                  WM_apple_ios_ui_setting_enqueue("object_mode", identifier.UTF8String);
                }];
    if ([identifier isEqualToString:current]) {
      action.state = UIMenuElementStateOn;
      selected = action;
    }
    [actions addObject:action];
  }
  if (!actions.count) {
    return;
  }
  UIMenu *menu = [UIMenu menuWithTitle:@"Mode" children:actions];
  _mode_button.menu = menu;
  _mode_button.menuRepresentation = menu;
  UIAction *shown = selected ?: actions.firstObject;
  _mode_button.title = shown.title;
  _mode_button.image = shown.image;
  _mode_button.accessibilityValue = shown.title;
}

- (NSString *)modeTitleForIdentifier:(NSString *)mode
{
  if ([mode isEqualToString:@"objectmode"]) {
    return @"Object Mode";
  }
  if ([mode hasSuffix:@"_edit"] || [mode hasPrefix:@"edit"]) {
    return @"Edit Mode";
  }
  if ([mode isEqualToString:@"sculpt_mode"] || [mode isEqualToString:@"sculpt"]) {
    return @"Sculpt Mode";
  }
  if ([mode isEqualToString:@"vertexpaint"]) {
    return @"Vertex Paint";
  }
  if ([mode isEqualToString:@"weightpaint"]) {
    return @"Weight Paint";
  }
  if ([mode isEqualToString:@"imagepaint"] || [mode isEqualToString:@"texturepaint"]) {
    return @"Texture Paint";
  }
  if ([mode isEqualToString:@"posemode"]) {
    return @"Pose Mode";
  }
  return @"Object Mode";
}

- (void)setModePickerTitleForMode:(NSString *)mode
{
  if (_mode_button == nil) {
    return;
  }

  NSString *title = [self modeTitleForIdentifier:mode];
  UIImage *image = nil;
  for (UIAction *action in _mode_button.menu.children) {
    if ([action.title isEqualToString:title]) {
      image = action.image;
      break;
    }
  }
  if (image == nil) {
    image = blenderIOSIcon([@"mode." stringByAppendingString:mode ?: @"objectmode"], nil);
  }
  if (![_mode_button.title isEqualToString:title]) {
    _mode_button.title = title;
  }
  if (_mode_button.image != image) {
    _mode_button.image = image;
  }
  _mode_button.accessibilityValue = title;
}

- (UIAction *)viewportShadingActionWithTitle:(NSString *)title
                                 systemImage:(NSString *)systemImage
                                     command:(NSInteger)command
{
  UIImage *image = blenderIOSCommandIcon(command, systemImage);
  return [UIAction actionWithTitle:title
                             image:image
                        identifier:[NSString stringWithFormat:@"%ld", (long)command]
                           handler:^(__kindof UIAction *action) {
                             WM_apple_ios_ui_command_enqueue((int)command);
                           }];
}

- (void)setViewportShadingValue:(NSString *)value
{
  if (![value isKindOfClass:NSString.class]) {
    return;
  }
  NSDictionary *commands = @{
    @"wireframe" : @(WM_IOS_UI_COMMAND_SET_VIEWPORT_WIREFRAME),
    @"solid" : @(WM_IOS_UI_COMMAND_SET_VIEWPORT_SOLID),
    @"material" : @(WM_IOS_UI_COMMAND_SET_VIEWPORT_MATERIAL),
    @"rendered" : @(WM_IOS_UI_COMMAND_SET_VIEWPORT_RENDERED),
  };
  NSNumber *command = commands[value];
  if (command == nil || _viewport_shading_button == nil ||
      _viewport_shading_button.tag == command.integerValue)
  {
    return;
  }

  NSMutableArray *actions = [NSMutableArray array];
  UIMenu *menu = (UIMenu *)_viewport_shading_button.menuRepresentation;
  for (UIAction *item in menu.children) {
    UIAction *action = [[item copy] autorelease];
    const BOOL selected = action.identifier.integerValue == command.integerValue;
    action.state = selected ? UIMenuElementStateOn : UIMenuElementStateOff;
    if (selected) {
      _viewport_shading_button.image = action.image;
      _viewport_shading_button.accessibilityValue = action.title;
    }
    [actions addObject:action];
  }
  _viewport_shading_button.menuRepresentation = [menu menuByReplacingChildren:actions];
  _viewport_shading_button.tag = command.integerValue;
}

- (UIAction *)settingsActionWithTitle:(NSString *)title
                          systemImage:(NSString *)systemImage
                              setting:(NSString *)setting
                                value:(NSString *)value
{
  UIImage *image = blenderIOSIcon([NSString stringWithFormat:@"setting.%@.%@", setting, value],
                                  systemImage);
  return [UIAction actionWithTitle:title
                             image:image
                        identifier:[NSString stringWithFormat:@"%@.%@", setting, value]
                           handler:^(__kindof UIAction *action) {
                             WM_apple_ios_ui_setting_enqueue(setting.UTF8String, value.UTF8String);
                           }];
}

- (UIBarButtonItem *)settingsMenuButtonWithAccessibilityLabel:(NSString *)accessibilityLabel
                                                  systemImage:(NSString *)systemImage
                                                        title:(NSString *)title
                                                     children:(NSArray *)children
{
  UIImage *image = blenderIOSIcon([@"control." stringByAppendingString:accessibilityLabel],
                                  systemImage);
  UIBarButtonItem *button = [[[UIBarButtonItem alloc]
      initWithImage:image
               menu:[UIMenu menuWithTitle:title children:children]] autorelease];
  button.title = title;
  button.accessibilityLabel = accessibilityLabel;
  button.identifier = [@"blender.setting." stringByAppendingString:accessibilityLabel];
  button.menuRepresentation = button.menu;
  return button;
}

- (UIBarButtonItemGroup *)topSettingsControlGroup
{
  UIBarButtonItem *orientation_button = [self
      settingsMenuButtonWithAccessibilityLabel:@"Transform Orientation"
                                   systemImage:@"rotate.3d"
                                         title:@"Transform Orientation"
                                      children:@[
                                        [self settingsActionWithTitle:@"Global"
                                                          systemImage:@"globe"
                                                              setting:@"orientation"
                                                                value:@"global"],
                                        [self settingsActionWithTitle:@"Local"
                                                          systemImage:@"cube"
                                                              setting:@"orientation"
                                                                value:@"local"],
                                        [self settingsActionWithTitle:@"Normal"
                                                          systemImage:@"arrow.up.forward"
                                                              setting:@"orientation"
                                                                value:@"normal"],
                                        [self settingsActionWithTitle:@"Gimbal"
                                                          systemImage:@"gyroscope"
                                                              setting:@"orientation"
                                                                value:@"gimbal"],
                                        [self settingsActionWithTitle:@"View"
                                                          systemImage:@"eye"
                                                              setting:@"orientation"
                                                                value:@"view"],
                                        [self settingsActionWithTitle:@"Cursor"
                                                          systemImage:@"scope"
                                                              setting:@"orientation"
                                                                value:@"cursor"],
                                        [self settingsActionWithTitle:@"Parent"
                                                          systemImage:@"arrow.triangle.branch"
                                                              setting:@"orientation"
                                                                value:@"parent"],
                                      ]];
  UIBarButtonItem *pivot_button = [self
      settingsMenuButtonWithAccessibilityLabel:@"Pivot Point"
                                   systemImage:@"viewfinder"
                                         title:@"Pivot Point"
                                      children:@[
                                        [self settingsActionWithTitle:@"Median Point"
                                                          systemImage:@"circle.grid.cross"
                                                              setting:@"pivot"
                                                                value:@"median"],
                                        [self settingsActionWithTitle:@"Bounding Box Center"
                                                          systemImage:@"square.dashed"
                                                              setting:@"pivot"
                                                                value:@"bounds"],
                                        [self settingsActionWithTitle:@"3D Cursor"
                                                          systemImage:@"scope"
                                                              setting:@"pivot"
                                                                value:@"cursor"],
                                        [self settingsActionWithTitle:@"Individual Origins"
                                                          systemImage:@"circle.grid.2x2"
                                                              setting:@"pivot"
                                                                value:@"individual"],
                                        [self settingsActionWithTitle:@"Active Element"
                                                          systemImage:@"record.circle"
                                                              setting:@"pivot"
                                                                value:@"active"],
                                      ]];
  UIBarButtonItem *snap_button = [self
      settingsMenuButtonWithAccessibilityLabel:@"Snapping"
                                   systemImage:@"magnet"
                                         title:@"Snapping"
                                      children:@[
                                        [self settingsActionWithTitle:@"Off"
                                                          systemImage:@"magnet"
                                                              setting:@"snap"
                                                                value:@"off"],
                                        [self settingsActionWithTitle:@"Increment"
                                                          systemImage:@"grid"
                                                              setting:@"snap"
                                                                value:@"increment"],
                                        [self settingsActionWithTitle:@"Grid"
                                                          systemImage:@"square.grid.3x3"
                                                              setting:@"snap"
                                                                value:@"grid"],
                                        [self
                                            settingsActionWithTitle:@"Vertex"
                                                        systemImage:@"point.topleft.down.curvedto."
                                                                    @"point.bottomright.up"
                                                            setting:@"snap"
                                                              value:@"vertex"],
                                        [self settingsActionWithTitle:@"Edge"
                                                          systemImage:@"line.diagonal"
                                                              setting:@"snap"
                                                                value:@"edge"],
                                        [self settingsActionWithTitle:@"Face"
                                                          systemImage:@"square"
                                                              setting:@"snap"
                                                                value:@"face"],
                                        [self settingsActionWithTitle:@"Volume"
                                                          systemImage:@"cube.transparent"
                                                              setting:@"snap"
                                                                value:@"volume"],
                                      ]];
  UIBarButtonItem *proportional_button = [self
      settingsMenuButtonWithAccessibilityLabel:@"Proportional Editing"
                                   systemImage:@"circle.circle"
                                         title:@"Proportional Editing"
                                      children:@[
                                        [self settingsActionWithTitle:@"Off"
                                                          systemImage:@"circle"
                                                              setting:@"proportional"
                                                                value:@"off"],
                                        [self settingsActionWithTitle:@"On"
                                                          systemImage:@"largecircle.fill.circle"
                                                              setting:@"proportional"
                                                                value:@"on"],
                                        [self settingsActionWithTitle:@"Connected Only"
                                                          systemImage:@"link.circle"
                                                              setting:@"proportional"
                                                                value:@"connected"],
                                        [self settingsActionWithTitle:@"Projected"
                                                          systemImage:@"square.3.layers.3d"
                                                              setting:@"proportional"
                                                                value:@"projected"],
                                      ]];

  UIMenu *menu = [UIMenu menuWithTitle:@"Transform Settings"
                              children:@[
                                orientation_button.menu,
                                pivot_button.menu,
                                snap_button.menu,
                                proportional_button.menu
                              ]];
  UIBarButtonItem *compact = [[[UIBarButtonItem alloc] initWithImage:orientation_button.image
                                                                menu:menu] autorelease];
  compact.title = @"Transform Settings";
  compact.accessibilityLabel = compact.title;
  _top_settings_group = [[UIBarButtonItemGroup fixedGroupWithRepresentativeItem:compact
                                                                          items:@[
                                                                            orientation_button,
                                                                            pivot_button,
                                                                            snap_button,
                                                                            proportional_button
                                                                          ]] retain];
  _top_settings_group.menuRepresentation = menu;
  _top_settings_group.hidden = YES;
  return _top_settings_group;
}

- (void)setTopSettingsValues:(NSDictionary *)values
{
  if (![values isKindOfClass:NSDictionary.class] ||
      [_top_settings_values isEqualToDictionary:values] ||
      _top_settings_group.barButtonItems.count != 4)
  {
    return;
  }
  NSArray *keys = @[ @"orientation", @"pivot", @"snap", @"proportional" ];
  NSMutableDictionary *applied = _top_settings_values ? [_top_settings_values mutableCopy] :
                                                        [[NSMutableDictionary alloc] init];
  BOOL changed = NO;
  for (NSUInteger index = 0; index < keys.count; index++) {
    NSString *key = keys[index];
    NSString *value = values[key];
    if (![value isKindOfClass:NSString.class] || [value isEqual:applied[key]]) {
      continue;
    }
    UIBarButtonItem *button = _top_settings_group.barButtonItems[index];
    NSString *identifier = [NSString stringWithFormat:@"%@.%@", key, value];
    UIAction *selected = nil;
    for (UIAction *action in button.menu.children) {
      if ([action.identifier isEqual:identifier]) {
        selected = action;
        break;
      }
    }
    const BOOL custom = [key isEqual:@"orientation"] && [value isEqual:@"custom"];
    const BOOL mixed = [key isEqual:@"snap"] && [value isEqual:@"mixed"];
    if (!selected && !custom && !mixed) {
      continue;
    }
    NSMutableArray *actions = [NSMutableArray array];
    for (UIAction *action in button.menu.children) {
      UIAction *updated = [[action copy] autorelease];
      updated.state = [action.identifier isEqual:identifier] ? UIMenuElementStateOn :
                                                               UIMenuElementStateOff;
      [actions addObject:updated];
    }
    button.image = selected ? selected.image :
                   mixed    ? blenderIOSIcon(@"control.Snapping", @"magnet") :
                              [UIImage systemImageNamed:@"rotate.3d"];
    button.accessibilityValue = selected ? selected.title :
                                custom   ? @"Custom Orientation" :
                                           @"Multiple or Other Elements";
    button.menu = [button.menu menuByReplacingChildren:actions];
    button.menuRepresentation = button.menu;
    applied[key] = value;
    changed = YES;
  }
  if (changed) {
    NSMutableArray *menus = [NSMutableArray array];
    for (UIBarButtonItem *button in _top_settings_group.barButtonItems) {
      [menus addObject:button.menu];
    }
    UIMenu *menu = [_top_settings_group.representativeItem.menu menuByReplacingChildren:menus];
    _top_settings_group.representativeItem.image =
        _top_settings_group.barButtonItems.firstObject.image;
    _top_settings_group.representativeItem.menu = menu;
    _top_settings_group.menuRepresentation = menu;
    [_top_settings_values release];
    _top_settings_values = [applied copy];
  }
  [applied release];
}

- (BOOL)isTransformToolIdentifier:(NSString *)toolIdentifier
{
  if (![toolIdentifier isKindOfClass:[NSString class]]) {
    return NO;
  }

  return [toolIdentifier isEqualToString:@"builtin.move"] ||
         [toolIdentifier isEqualToString:@"builtin.rotate"] ||
         [toolIdentifier isEqualToString:@"builtin.scale"] ||
         [toolIdentifier isEqualToString:@"builtin.scale_cage"] ||
         [toolIdentifier isEqualToString:@"builtin.transform"] ||
         [toolIdentifier isEqualToString:@"builtin.shear"];
}

- (void)setTopSettingsVisible:(BOOL)visible animated:(BOOL)animated
{
  UNUSED_VARS(animated);
  if (_top_settings_group.hidden == visible) {
    _top_settings_group.hidden = !visible;
  }
}

- (UIBarButtonItem *)meshSelectModeButtonWithTitle:(NSString *)title
                                       systemImage:(NSString *)systemImage
                                             value:(NSString *)value
{
  UIImage *image = blenderIOSIcon([@"mesh." stringByAppendingString:value], systemImage);
  UIAction *action = [UIAction
      actionWithTitle:title
                image:image
           identifier:value
              handler:^(__kindof UIAction *selected) {
                [self setMeshSelectModeValue:value];
                WM_apple_ios_ui_setting_enqueue("mesh_select_mode", value.UTF8String);
              }];
  UIBarButtonItem *button = [[[UIBarButtonItem alloc] initWithPrimaryAction:action] autorelease];
  button.image = image;
  button.accessibilityLabel = title;
  button.accessibilityIdentifier = value;
  button.identifier = [@"blender.mesh." stringByAppendingString:value];
  [_mesh_select_buttons addObject:button];
  return button;
}

- (UIBarButtonItemGroup *)meshSelectModeControlGroup
{
  NSArray *items = @[
    [self meshSelectModeButtonWithTitle:@"Vertex Select" systemImage:@"circle.fill" value:@"VERT"],
    [self meshSelectModeButtonWithTitle:@"Edge Select" systemImage:@"line.diagonal" value:@"EDGE"],
    [self meshSelectModeButtonWithTitle:@"Face Select" systemImage:@"square.fill" value:@"FACE"]
  ];
  UIBarButtonItem *compact = [[[UIBarButtonItem alloc]
      initWithImage:blenderIOSIcon(@"mesh.VERT", @"circle.fill")
               menu:nil] autorelease];
  compact.title = @"Selection Mode";
  compact.accessibilityLabel = compact.title;
  NSMutableArray *actions = [NSMutableArray array];
  for (UIBarButtonItem *button in items) {
    [actions addObject:button.primaryAction];
  }
  _mesh_select_control = [[[UISegmentedControl alloc] initWithFrame:CGRectMake(0, 0, 144, 44)
                                                            actions:actions] autorelease];
  _mesh_select_control.accessibilityLabel = @"Mesh Selection Mode";
  _mesh_select_control.accessibilityIdentifier = @"blender.mesh-selection";
  _mesh_select_control.momentary = NO;
  [NSLayoutConstraint activateConstraints:@[
    [_mesh_select_control.widthAnchor constraintEqualToConstant:144],
    [_mesh_select_control.heightAnchor constraintEqualToConstant:44],
  ]];
  UIBarButtonItem *control = [[[UIBarButtonItem alloc] initWithCustomView:_mesh_select_control]
      autorelease];
  control.identifier = @"blender.mesh-selection";
#if !defined(APPLE_TARGET_VISIONOS)
  control.hidesSharedBackground = YES;
  if (@available(iOS 27.0, *)) {
    control.visibilityPriority = UIBarButtonItemVisibilityPriorityHigh;
    compact.visibilityPriority = UIBarButtonItemVisibilityPriorityHigh;
  }
#endif
  _mesh_select_group = [UIBarButtonItemGroup fixedGroupWithRepresentativeItem:compact
                                                                        items:@[ control ]];
  _mesh_select_group.hidden = YES;
  [self setMeshSelectModeValue:@"VERT"];
  return _mesh_select_group;
}

- (void)setMeshSelectModeVisible:(BOOL)visible animated:(BOOL)animated
{
  UNUSED_VARS(animated);
  if (_mesh_select_group.hidden == visible) {
    _mesh_select_group.hidden = !visible;
  }
}

- (void)setMeshSelectModeValue:(NSString *)value
{
  NSUInteger index = [@[ @"VERT", @"EDGE", @"FACE" ] indexOfObject:value ?: @""];
  if (index == NSNotFound) {
    return;
  }
  _mesh_select_control.selectedSegmentIndex = index;
  if ([_mesh_select_group.representativeItem.accessibilityValue isEqualToString:value]) {
    return;
  }
  NSMutableArray *actions = [NSMutableArray array];
  for (UIBarButtonItem *button in _mesh_select_buttons) {
    button.selected = [button.accessibilityIdentifier isEqualToString:value];
    UIAction *action = [[button.primaryAction copy] autorelease];
    action.image = button.image;
    action.state = button.selected ? UIMenuElementStateOn : UIMenuElementStateOff;
    button.menuRepresentation = action;
    [actions addObject:action];
    if (button.selected) {
      _mesh_select_group.representativeItem.image = button.image;
    }
  }
  UIMenu *menu = [UIMenu menuWithTitle:@"Selection Mode" children:actions];
  _mesh_select_group.representativeItem.menu = menu;
  _mesh_select_group.representativeItem.accessibilityValue = value;
  _mesh_select_group.menuRepresentation = menu;
  _mesh_select_group.barButtonItems.firstObject.menuRepresentation = menu;
}

- (void)showViewportShading:(UIBarButtonItem *)button
{
  [_scene_panels presentShadingFrom:button controller:self];
}

- (UIBarButtonItem *)viewportShadingButton
{
  UIBarButtonItem *button = [[[UIBarButtonItem alloc]
      initWithImage:blenderIOSIcon(@"control.shading", @"circle.lefthalf.filled")
               menu:nil] autorelease];
  button.title = @"Viewport Shading";
  button.accessibilityLabel = @"Viewport Shading";
  button.identifier = @"blender.shading";

  button.menu = [UIMenu
      menuWithTitle:@"Viewport Shading"
           children:@[
             [self viewportShadingActionWithTitle:@"Wireframe"
                                      systemImage:@"circle.dashed"
                                          command:WM_IOS_UI_COMMAND_SET_VIEWPORT_WIREFRAME],
             [self viewportShadingActionWithTitle:@"Solid"
                                      systemImage:@"cube"
                                          command:WM_IOS_UI_COMMAND_SET_VIEWPORT_SOLID],
             [self viewportShadingActionWithTitle:@"Material Preview"
                                      systemImage:@"circle.lefthalf.filled"
                                          command:WM_IOS_UI_COMMAND_SET_VIEWPORT_MATERIAL],
             [self viewportShadingActionWithTitle:@"Rendered"
                                      systemImage:@"camera.aperture"
                                          command:WM_IOS_UI_COMMAND_SET_VIEWPORT_RENDERED],
           ]];
  button.menuRepresentation = button.menu;
  button.menu = nil;
  button.target = self;
  button.action = @selector(showViewportShading:);
  return button;
}

- (void)setUndoAvailable:(id)value
{
  const BOOL enabled = [value isKindOfClass:NSNumber.class] && [value boolValue];
  if (_undo_button.enabled != enabled) {
    _undo_button.enabled = enabled;
  }
}

- (void)handleUndoButton:(UIBarButtonItem *)button
{
  if (button.enabled) {
    WM_apple_ios_ui_operator_enqueue("ED_OT_undo");
  }
}

- (UIBarButtonItem *)undoButton
{
  UIBarButtonItem *button = [[[UIBarButtonItem alloc]
      initWithImage:blenderIOSIcon(@"operator.ED_OT_undo", @"arrow.uturn.backward")
              style:UIBarButtonItemStylePlain
             target:self
             action:@selector(handleUndoButton:)] autorelease];
  button.title = @"Undo";
  button.accessibilityLabel = @"Undo";
  button.accessibilityIdentifier = @"blender.undo";
  button.identifier = @"blender.undo";
  button.enabled = NO;
  return button;
}

- (UIBarButtonItem *)modePickerButton
{
  UIBarButtonItem *button = [[[UIBarButtonItem alloc] initWithTitle:@"Object Mode"
                                                               menu:nil] autorelease];
  button.accessibilityLabel = @"Mode";
  button.identifier = @"blender.mode";

  UIMenu *mode_menu = [UIMenu
      menuWithTitle:@"Mode"
           children:@[
             [self modeActionWithTitle:@"Object Mode" command:WM_IOS_UI_COMMAND_SET_MODE_OBJECT],
             [self modeActionWithTitle:@"Edit Mode" command:WM_IOS_UI_COMMAND_SET_MODE_EDIT],
             [self modeActionWithTitle:@"Sculpt Mode" command:WM_IOS_UI_COMMAND_SET_MODE_SCULPT],
             [self modeActionWithTitle:@"Vertex Paint"
                               command:WM_IOS_UI_COMMAND_SET_MODE_VERTEX_PAINT],
             [self modeActionWithTitle:@"Weight Paint"
                               command:WM_IOS_UI_COMMAND_SET_MODE_WEIGHT_PAINT],
             [self modeActionWithTitle:@"Texture Paint"
                               command:WM_IOS_UI_COMMAND_SET_MODE_TEXTURE_PAINT],
           ]];
  button.menu = mode_menu;
  button.menuRepresentation = mode_menu;
  button.image = ((UIAction *)mode_menu.children.firstObject).image;
  button.accessibilityValue = button.title;
  return button;
}

- (void)updateActionsButtonMenu
{
  if (_actions_button == nil || _actions_button.menu != nil) {
    return;
  }
  __block __typeof__(self) controller = self;
  UIMenu * (^section)(NSArray *) = ^UIMenu *(NSArray *children) {
    return [UIMenu menuWithTitle:@""
                           image:nil
                      identifier:nil
                         options:UIMenuOptionsDisplayInline
                        children:children];
  };

  /* Share: what leaves the document. Documents save themselves, so
   * there is no Save; going back to Documents keeps the work. */
  UIAction *share = [UIAction actionWithTitle:@"Share…"
                                        image:[UIImage systemImageNamed:@"square.and.arrow.up"]
                                   identifier:@"blender.document.share"
                                      handler:^(__kindof UIAction *) {
                                        [controller shareDocument];
                                      }];
  UIAction *saveAs = [UIAction actionWithTitle:@"Save As…"
                                         image:[UIImage systemImageNamed:@"doc.on.doc"]
                                    identifier:@"blender.document.save-as"
                                       handler:^(__kindof UIAction *) {
                                         [controller saveDocumentAs];
                                       }];
  UIAction *renderImage = [UIAction actionWithTitle:@"Render Image"
                                              image:[UIImage systemImageNamed:@"photo"]
                                         identifier:@"blender.render.image"
                                            handler:^(__kindof UIAction *) {
                                              [controller->_render renderAnimation:NO];
                                            }];
  UIAction *renderAnimation = [UIAction actionWithTitle:@"Render Animation"
                                                  image:[UIImage systemImageNamed:@"film"]
                                             identifier:@"blender.render.animation"
                                                handler:^(__kindof UIAction *) {
                                                  [controller->_render renderAnimation:YES];
                                                }];
  /* Every earlier render, in the render sheet's film strip. */
  UIAction *renders = [UIAction actionWithTitle:@"Renders"
                                          image:[UIImage systemImageNamed:@"photo.stack"]
                                     identifier:@"blender.render.history"
                                        handler:^(__kindof UIAction *) {
                                          [controller->_render showRenders];
                                        }];
  NSArray *render_actions = @[ renderImage, renderAnimation, renders ];
  UIAction *saveTemplate = [UIAction actionWithTitle:@"Save as Template…"
                                               image:[UIImage systemImageNamed:@"square.on.square"]
                                          identifier:@"blender.template.save"
                                             handler:^(__kindof UIAction *) {
                                               [controller saveAsTemplate];
                                             }];
  /* Blender's own Import and Export menus, so every format it and its add-ons support is listed.
   * Their file browsers open the system document picker. */
  UIMenu *import_menu = blenderIOSNativeSubmenu(@{
    @"title" : @"Import",
    @"icon" : @"IMPORT",
    @"route" : @{@"root" : @"TOPBAR_MT_file_import", @"path" : @[]}
  },
                                                @"blender.file.import");
  UIMenu *export_menu = blenderIOSNativeSubmenu(@{
    @"title" : @"Export",
    @"icon" : @"EXPORT",
    @"route" : @{@"root" : @"TOPBAR_MT_file_export", @"path" : @[]}
  },
                                                @"blender.file.export");
  UIMenu *share_menu = [UIMenu menuWithTitle:@""
                                    children:@[
                                      section(@[ share, saveAs ]),
                                      section(@[ import_menu, export_menu ]),
                                      section(render_actions),
                                      section(@[ saveTemplate ]),
                                    ]];
  _share_button.menu = share_menu;
  _share_button.menuRepresentation = share_menu;

  /* More: smaller things, with viewport options that show their current state when opened. */
  UIAction *create = [UIAction actionWithTitle:@"New…"
                                         image:[UIImage systemImageNamed:@"doc.badge.plus"]
                                    identifier:@"blender.document.new"
                                       handler:^(__kindof UIAction *) {
                                         [controller newDocument:nil];
                                       }];
  UIDeferredMenuElement *viewport = [UIDeferredMenuElement
      elementWithUncachedProvider:^(void (^completion)(NSArray<UIMenuElement *> *)) {
        NSDictionary *state = controller->_viewport_state;
        NSMutableArray *toggles = [NSMutableArray array];
        for (NSArray *spec in @[
               @[ @"overlays", @"Overlays", @"circle.dashed.inset.filled" ],
               @[ @"xray", @"X-Ray", @"square.on.square.intersection.dashed" ],
               @[ @"gizmos", @"Gizmos", @"move.3d" ],
               @[ @"statistics", @"Statistics", @"chart.bar" ],
               @[ @"wireframe", @"Wireframe", @"cube" ]
             ])
        {
          NSString *name = spec[0];
          UIAction *toggle = [UIAction
              actionWithTitle:spec[1]
                        image:[UIImage systemImageNamed:spec[2]]
                   identifier:[@"blender.viewport." stringByAppendingString:name]
                      handler:^(__kindof UIAction *) {
                        WM_apple_ios_ui_setting_enqueue("viewport_toggle", name.UTF8String);
                      }];
          toggle.state = [state[name] boolValue] ? UIMenuElementStateOn : UIMenuElementStateOff;
          toggle.attributes = state ? 0 : UIMenuElementAttributesDisabled;
          [toggles addObject:toggle];
        }
        completion(@[ section(toggles) ]);
      }];
  UIAction *desktop = [UIAction actionWithTitle:@"Switch to Desktop Interface"
                                          image:[UIImage systemImageNamed:@"macwindow"]
                                     identifier:@"blender.interface.desktop"
                                        handler:^(__kindof UIAction *) {
                                          WM_apple_ios_ui_setting_enqueue("interface", "desktop");
                                        }];
  UIAction *hide = [UIAction
      actionWithTitle:@"Hide Interface"
                image:[UIImage systemImageNamed:@"arrow.up.left.and.arrow.down.right"]
           identifier:@"blender.interface.hide"
              handler:^(__kindof UIAction *) {
                [controller setInterfaceHidden:YES];
              }];
  UIAction *settings = [UIAction actionWithTitle:@"Settings"
                                           image:[UIImage systemImageNamed:@"gearshape"]
                                      identifier:@"blender.settings"
                                         handler:^(__kindof UIAction *) {
                                           [controller->_settings present];
                                         }];
  if (!_settings) {
    settings.attributes = UIMenuElementAttributesHidden;
  }
  UIMenu *more = [UIMenu menuWithTitle:@""
                              children:@[
                                section(@[ create ]),
                                viewport,
                                section(@[ hide, desktop ]),
                                section(@[ settings ])
                              ]];
  _actions_button.menu = more;
  _actions_button.menuRepresentation = more;
  _actions_button.enabled = YES;
}

- (void)saveDocumentAs
{
  UIDocument *document = wm_apple_ios_launcher_document();
  if (document == nil) {
    return;
  }
  /* Save a copy wherever the user picks, as Files apps do for Save As. */
  __block WMIOSViewController *controller = self;
  [document saveToURL:document.fileURL
       forSaveOperation:UIDocumentSaveForOverwriting
      completionHandler:^(BOOL) {
        UIDocumentPickerViewController *picker = [[[UIDocumentPickerViewController alloc]
            initForExportingURLs:@[ document.fileURL ]
                          asCopy:YES] autorelease];
        [controller presentViewController:picker animated:YES completion:nil];
      }];
}

- (void)shareDocument
{
  UIDocument *document = wm_apple_ios_launcher_document();
  if (document == nil) {
    return;
  }
  /* Share the file as it is now. */
  __block WMIOSViewController *controller = self;
  [document saveToURL:document.fileURL
       forSaveOperation:UIDocumentSaveForOverwriting
      completionHandler:^(BOOL) {
        UIActivityViewController *activity = [[[UIActivityViewController alloc]
            initWithActivityItems:@[ document.fileURL ]
            applicationActivities:nil] autorelease];
        activity.popoverPresentationController.sourceItem = controller->_share_button;
        [controller presentViewController:activity animated:YES completion:nil];
      }];
}

- (void)saveAsTemplate
{
  UIAlertController *alert = [UIAlertController
      alertControllerWithTitle:@"Save as Template"
                       message:nil
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
    field.placeholder = @"Template Name";
    field.text = @"My Template";
  }];
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  [alert
      addAction:
          [UIAlertAction
              actionWithTitle:@"Save"
                        style:UIAlertActionStyleDefault
                      handler:^(UIAlertAction *) {
                        NSString *name = [alert.textFields.firstObject.text
                            stringByTrimmingCharactersInSet:NSCharacterSet
                                                                .whitespaceAndNewlineCharacterSet];
                        if (!name.length) {
                          return;
                        }
                        [alert
                            dismissViewControllerAnimated:YES
                                               completion:^{
                                                 UIViewController *progress =
                                                     [[[UIViewController alloc] init] autorelease];
                                                 progress.modalPresentationStyle =
                                                     UIModalPresentationFormSheet;
                                                 progress.modalInPresentation = YES;
                                                 progress.preferredContentSize = CGSizeMake(320,
                                                                                            180);
                                                 progress.view.backgroundColor =
                                                     UIColor.systemBackgroundColor;
                                                 UIContentUnavailableConfiguration *loading =
                                                     [UIContentUnavailableConfiguration
                                                         loadingConfiguration];
                                                 loading.text = @"Saving Template";
                                                 progress.contentUnavailableConfiguration =
                                                     loading;
                                                 [self
                                                     presentViewController:progress
                                                                  animated:YES
                                                                completion:^{
                                                                  wm_apple_ios_template_save(
                                                                      name,
                                                                      @{
                                                                        @"timeline" :
                                                                            @(self->_timeline
                                                                                  .visible)
                                                                      },
                                                                      ^(NSString *error) {
                                                                        [progress
                                                                            dismissViewControllerAnimated:
                                                                                YES
                                                                                               completion:^{
                                                                                                 UIAlertController *result = [UIAlertController
                                                                                                     alertControllerWithTitle:
                                                                                                         error ?
                                                                                                             @"Could Not Save Template" :
                                                                                                             @"Template Saved"
                                                                                                                      message:
                                                                                                                          error ?:
                                                                                                                              @"Available in My Templates when creating a new document."
                                                                                                               preferredStyle:
                                                                                                                   UIAlertControllerStyleAlert];
                                                                                                 [result
                                                                                                     addAction:
                                                                                                         [UIAlertAction
                                                                                                             actionWithTitle:
                                                                                                                 @"OK"
                                                                                                                       style:
                                                                                                                           UIAlertActionStyleDefault
                                                                                                                     handler:
                                                                                                                         nil]];
                                                                                                 [self
                                                                                                     presentViewController:
                                                                                                         result
                                                                                                                  animated:
                                                                                                                      YES
                                                                                                                completion:
                                                                                                                    nil];
                                                                                               }];
                                                                      });
                                                                }];
                                               }];
                      }]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (UIBarButtonItem *)actionsButton
{
  UIBarButtonItem *button = [[[UIBarButtonItem alloc]
      initWithImage:[UIImage systemImageNamed:@"ellipsis"]
               menu:nil] autorelease];
  button.title = @"More";
  button.accessibilityLabel = @"More";
  button.identifier = @"blender.actions";
  return button;
}

- (void)installNativeTopBar
{
  if (self.navigationController) {
    _top_bar = (WMIOSNavigationBar *)[self.navigationController.navigationBar retain];
    _top_bar_item = [self.navigationItem retain];
  }
  else {
    _top_bar = [[WMIOSNavigationBar alloc] initWithFrame:CGRectZero];
    _top_bar.translatesAutoresizingMaskIntoConstraints = NO;
    _top_bar_item = [[UINavigationItem alloc] initWithTitle:@""];
  }
  _top_bar.tintColor = UIColor.systemBlueColor;
  _top_bar.accessibilityIdentifier = @"blender.top-toolbar";
  _top_bar_item.style = UINavigationItemStyleEditor;

  _mode_button = [self modePickerButton];
  _actions_button = [self actionsButton];
  _share_button = [[[UIBarButtonItem alloc]
      initWithImage:[UIImage systemImageNamed:@"square.and.arrow.up"]
               menu:nil] autorelease];
  _share_button.title = @"Share";
  _share_button.accessibilityLabel = @"Share";
  _share_button.identifier = @"blender.share";
  _render = [[blenderIOSSwiftClass(@"WMIOSSwiftRender") alloc] initWithPresenter:self];
  if (Class settings_class = NSClassFromString(@"WMIOSSwiftSettings")) {
    _settings = [[settings_class alloc] initWithPresenter:self];
    __block WMIOSViewController *settings_host = self;
    _settings.fingerUsesToolChanged = ^(BOOL useTool) {
      [settings_host setFingerInputMode:useTool];
    };
    _settings.showQuickActionsChanged = ^(BOOL) {
      [settings_host rebuildActionStripWithActions:settings_host->_quick_actions];
      [settings_host.view setNeedsLayout];
    };
    /* Industry Compatible is the iPad's default keymap; switch once, then respect the choice. */
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    if (![defaults boolForKey:@"BlenderKeymapDefaulted"]) {
      [defaults setBool:YES forKey:@"BlenderKeymapDefaulted"];
      WM_apple_ios_ui_setting_enqueue("keymap", "Industry_Compatible");
    }
  }
  _documents_button = [[self documentsButton] retain];
  _viewport_shading_button = [self viewportShadingButton];
  _undo_button = [self undoButton];
  [self updateActionsButtonMenu];
  [self setViewportShadingValue:@"solid"];

#if defined(APPLE_TARGET_VISIONOS)
  /* The toolbar ornament below the window: Blender's full context menu, beside the quick
   * actions. */
  [_vision_chrome setContextMenu:[self nativeBlenderContextMenu]];
#endif
  /* Real bar items provide UIKit with actions and menu representations for overflow.
   * Group scene actions beside Mode; keep viewport shading on the trailing side. */
  _top_bar_item.leadingItemGroups = @[
    _documents_button.creatingFixedGroup,
    _mode_button.creatingFixedGroup,
    _assets.button.creatingFixedGroup,
    [self meshSelectModeControlGroup],
    _undo_button.creatingFixedGroup,
  ];
  if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
    _finger_input_button = [self fingerInputButton];
    [self setFingerInputMode:YES];
    _top_bar_item.leadingItemGroups = [_top_bar_item.leadingItemGroups
        arrayByAddingObject:_finger_input_button.creatingFixedGroup];
  }
  _top_bar_item.centerItemGroups = @[ [self topSettingsControlGroup] ];
  /* Panels, then shading, Share and More at the far right. */
  _top_bar_item.trailingItemGroups = @[
    [UIBarButtonItemGroup fixedGroupWithRepresentativeItem:nil
                                                     items:@[
                                                       _scene_panels.hierarchyButton,
                                                       _timeline.button,
                                                       _scene_panels.inspectorButton
                                                     ]],
    _viewport_shading_button.creatingFixedGroup,
    _share_button.creatingFixedGroup,
  ];
  _top_bar_item.pinnedTrailingGroup = _actions_button.creatingFixedGroup;
#if !defined(APPLE_TARGET_VISIONOS)
  _viewport_shading_button.sharesBackground = NO;
  /* Back to Documents is navigation, not a tool: give it its own glass at the top left, like the
   * system's back button. */
  _documents_button.sharesBackground = NO;
  if (@available(iOS 27.0, *)) {
    _mode_button.visibilityPriority = UIBarButtonItemVisibilityPriorityHigh;
    _undo_button.visibilityPriority = UIBarButtonItemVisibilityPriorityHigh;
    _viewport_shading_button.visibilityPriority = UIBarButtonItemVisibilityPriorityHigh;
    _share_button.visibilityPriority = UIBarButtonItemVisibilityPriorityHigh;
  }
#  if __IPHONE_OS_VERSION_MAX_ALLOWED >= 270100
  if (@available(iOS 27.1, *)) {
    _top_bar_item.verticalBarCompressionBehavior = UIVerticalBarCompressionBehaviorPrefersBarItems;
  }
#  endif
#endif
  if (self.navigationController) {
    return;
  }
  [_top_bar setItems:@[ _top_bar_item ] animated:NO];
  [_overlay_view addSubview:_top_bar];
  UILayoutGuide *safe = _overlay_view.safeAreaLayoutGuide;
  _top_bar_height_constraint = [_top_bar.heightAnchor constraintEqualToConstant:56.0];
  [NSLayoutConstraint activateConstraints:@[
    [_top_bar.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
    [_top_bar.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
    /* A windowed iPad can have no top safe-area inset. Keep the floating controls
     * clear of the window edge as well as any system-reserved area. */
    [_top_bar.topAnchor constraintEqualToAnchor:safe.topAnchor constant:12.0],
    _top_bar_height_constraint,
  ]];
}

@end
