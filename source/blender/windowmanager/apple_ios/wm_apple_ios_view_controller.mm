/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * The view controller of Blender's window: the Metal view, its life cycle, layout and the
 * snapshots Blender publishes.
 */

#import "wm_apple_ios_view_controller.hh"

@implementation WMIOSViewController

- (instancetype)initWithMetalKitView:(MTKView *)mtkView window:(GHOST_WindowIOS *)window
{
  self = [super init];
  if (self) {
    _ghost_window = window;
    _view = mtkView;
    self.edgesForExtendedLayout = UIRectEdgeAll;
    self.extendedLayoutIncludesOpaqueBars = YES;
  }
  return self;
}

- (void)loadView
{
  self.view = _view;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  _view = (MTKView *)self.view;
  [self updateMTKViewDrawableScale];

  [self installIPadOverlay];
  [self installViewportLoadingIndicator];
  [self installDocumentControls];
  if ([NSUserDefaults.standardUserDefaults boolForKey:@"BlenderDesktopInterface"]) {
    /* Start in the interface the user left; Blender follows from its inbox. */
    WM_apple_ios_ui_setting_enqueue("interface", "desktop");
    [self setDesktopInterface:YES];
  }
  /* Snapshots arrive by notification; only whether Blender is held up (compiling shaders) is
   * checked on a timer, since a held-up Blender publishes nothing. */
  _toolbar_refresh_timer = [NSTimer
      scheduledTimerWithTimeInterval:0.25
                              target:self
                            selector:@selector(updateViewportLoadingIndicator)
                            userInfo:nil
                             repeats:YES];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(blenderSnapshotsPublished:)
                                             name:@"BlenderSnapshotsPublished"
                                           object:nil];
  [self refreshToolbarFromBlender];
  [self refreshScenePanelsFromBlender];
  _actions_edit_menu_interaction = [[UIEditMenuInteraction alloc] initWithDelegate:self];
  [_view addInteraction:_actions_edit_menu_interaction];

  UILongPressGestureRecognizer *actions_menu_press = [[UILongPressGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleActionsMenuPress:)];
  actions_menu_press.minimumPressDuration = 0.42;
  actions_menu_press.allowableMovement = 12.0;
  actions_menu_press.cancelsTouchesInView = YES;
  actions_menu_press.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  actions_menu_press.delegate = self;
  actions_menu_press.enabled = UIDevice.currentDevice.userInterfaceIdiom !=
                               UIUserInterfaceIdiomPhone;
  [_view addGestureRecognizer:actions_menu_press];
  [actions_menu_press release];
  [self setNeedsUpdateOfHomeIndicatorAutoHidden];
}

/* Documents. The launcher owns them, see `wm_apple_ios_launcher.mm`; this window shows the open
 * one.
 */

/** Blender's main window, shown over the launcher while a document is open. */
static WMIOSViewController *document_controller = nil;

namespace blender {

void WM_apple_ios_document_window_changed()
{
  [document_controller updateDocumentControls];
}

void *WM_apple_ios_window_controller(void *metal_view, GHOST_WindowIOS *window, void **r_root)
{
  WMIOSViewController *controller = [[WMIOSViewController alloc]
      initWithMetalKitView:(MTKView *)metal_view
                    window:window];
#if defined(APPLE_TARGET_VISIONOS)
  /* A visionOS window's own title bar and toolbar ornament. */
  const bool use_navigation = true;
#else
  const bool use_navigation = UIDevice.currentDevice.userInterfaceIdiom ==
                              UIUserInterfaceIdiomPhone;
#endif
  if (use_navigation) {
    WMIOSNavigationController *navigation = [[[WMIOSNavigationController
        alloc] initWithNavigationBarClass:WMIOSNavigationBar.class toolbarClass:nil] autorelease];
    [navigation setViewControllers:@[ controller ] animated:NO];
    *r_root = navigation;
  }
  else {
    *r_root = controller;
  }
  return controller;
}

}  // namespace blender

- (void)installDocumentControls
{
  document_controller = self;
  [self updateDocumentControls];
}

- (void)updateDocumentControls
{
  /* The system's navigation item has the back action. Blender's own bar shows it as a button,
   * without the document's name, which would take room from the tools. */
  _documents_button.hidden = !wm_apple_ios_launcher_can_show_documents();
}

- (void)showDocuments:(id)sender
{
  wm_apple_ios_launcher_show_documents(sender);
}

- (void)updateMTKViewDrawableScale
{
  CGFloat scale = WMIOSDisplayScale(_view);
  if (scale <= 0.0) {
    scale = 1.0;
  }

  _view.contentScaleFactor = scale;
  _view.layer.contentsScale = scale;

  CGSize bounds_size = _view.bounds.size;
  CGSize drawable_size = CGSizeMake(MAX(1.0, bounds_size.width * scale),
                                    MAX(1.0, bounds_size.height * scale));
  if (!CGSizeEqualToSize(_view.drawableSize, drawable_size)) {
    _view.drawableSize = drawable_size;
  }
}

- (void)installViewportLoadingIndicator
{
  _viewport_loading_indicator = [[[UIActivityIndicatorView alloc]
      initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleLarge] autorelease];
  _viewport_loading_indicator.translatesAutoresizingMaskIntoConstraints = NO;
  _viewport_loading_indicator.color = UIColor.secondaryLabelColor;
  _viewport_loading_indicator.hidesWhenStopped = YES;
  _viewport_loading_indicator.userInteractionEnabled = NO;
  _viewport_loading_indicator.isAccessibilityElement = YES;
  _viewport_loading_indicator.accessibilityLabel = @"Loading Viewport";
  _viewport_loading_indicator.accessibilityIdentifier = @"blender.viewport.loading";
  [self.view addSubview:_viewport_loading_indicator];
  [NSLayoutConstraint activateConstraints:@[
    [_viewport_loading_indicator.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
    [_viewport_loading_indicator.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
  ]];
  [_viewport_loading_indicator startAnimating];
}

- (void)updateViewportLoadingIndicator
{
  /* Read only the presentation mailbox; never wait for the Blender thread. */
  if (_viewport_loading_indicator.isAnimating && _ghost_window &&
      GHOST_ios_window_presented_frame(_ghost_window))
  {
    [_viewport_loading_indicator stopAnimating];
    _viewport_loading_indicator.accessibilityElementsHidden = YES;
  }
}

- (void)detachBlender
{
  [_viewport_loading_indicator stopAnimating];
  [self hideToolbarLabelAnimated:NO];
  [self cancelViewGizmoInteraction];
  [_toolbar_refresh_timer invalidate];
  _toolbar_refresh_timer = nil;
  [_panels_refresh_timer invalidate];
  _panels_refresh_timer = nil;
  _pencil_menu.delegate = nil;
  _ghost_window = nullptr;
}

- (void)dealloc
{
  if (document_controller == self) {
    document_controller = nil;
  }
  [_documents_button release];
  [_scene_panels release];
  [_redo_strip release];
  [_mode_items release];
  [_object_mode release];
  [_timeline release];
  [_assets release];
  [_file_drop release];
  [_compile_indicator stop];
  [_compile_indicator release];
  [_render release];
#if defined(APPLE_TARGET_VISIONOS)
  [_vision_chrome release];
#endif
  [_settings release];
  [_quick_actions release];
  [_viewport_state release];
  [_show_interface_button release];
  [NSNotificationCenter.defaultCenter removeObserver:self];
  [_view_gizmo_geometry release];
#if !defined(APPLE_TARGET_VISIONOS)
  [_view_gizmo_selection_feedback release];
  [_view_gizmo_open_feedback release];
#endif
  _pencil_menu.delegate = nil;
  [_pencil_menu release];
  [_toolbar_refresh_timer invalidate];
  [_panels_refresh_timer invalidate];
  [_toolbar_signature release];
  [_action_signature release];
  [_top_settings_values release];
  [_top_settings_group release];
  [_native_menubar_items release];
  [_active_tool_identifier release];
  [_actions_edit_menu_interaction release];
  [_top_bar release];
  [_top_bar_item release];
  [_mesh_select_buttons release];
  [_toolbar_buttons release];
  [_brush_buttons release];
  [_brush_signature release];
  [_active_brush_identifier release];
  [_brush_scroll release];
  [_brush_stack release];
  [_brush_rail_bottom_constraint release];
  [_action_buttons release];
  [_tool_rail_glass_effect release];
  [_tool_rail_bottom_constraint release];
  [_tool_rail_center_y_constraint release];
  [_tool_scroll release];
  [_tool_stack release];
  [_action_rail_glass_effect release];
  [_action_scroll release];
  [_action_stack release];
  [_action_expanded_width_constraint release];
  [_action_collapsed_width_constraint release];
  [_action_toggle_width_constraint release];
  [_action_toggle_trailing_constraint release];
  [_action_group_leading_constraint release];
  [_action_group_center_constraint release];
  [_action_scroll_trailing_constraint release];
  [_context_menu_button release];
  [_context_menu_width_constraint release];
  [_context_menu_spacing_constraint release];
  [_redo_pill_width_constraint release];
  [_redo_pill_min_width_constraint release];
  [_view_pie_buttons release];
  [super dealloc];
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer
       shouldReceiveTouch:(UITouch *)touch
{
  if (gestureRecognizer.view == _view &&
      [gestureRecognizer isKindOfClass:UILongPressGestureRecognizer.class])
  {
    /* Ancestor recognizers also see touches on native controls. Only a press
     * starting on the viewport itself belongs to the viewport context menu. */
    return touch.type == UITouchTypeDirect && touch.view == _view && !_pencil_menu.active;
  }
  return YES;
}

- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)gestureRecognizer
{
  if ([gestureRecognizer isKindOfClass:UILongPressGestureRecognizer.class] &&
      [_toolbar_buttons containsObject:gestureRecognizer.view])
  {
    /* Group flyouts use UIButton's native hold menu; scrolling owns a moving touch. */
    /* Holding a closed rail always opens it for press-drag-release selection; group menus
     * only take over the hold once the rail is already showing its list. */
    return !_toolbar_expanded &&
           !((UIButton *)gestureRecognizer.view).contextMenuInteractionEnabled;
  }
  if ([gestureRecognizer isKindOfClass:UILongPressGestureRecognizer.class] &&
      [_brush_buttons containsObject:gestureRecognizer.view])
  {
    return !_brush_rail_expanded;
  }
  if (gestureRecognizer.view == _brush_drag_handle) {
    CGPoint velocity = [(UIPanGestureRecognizer *)gestureRecognizer velocityInView:_overlay_view];
    return [self isBrushListVisible] && velocity.y > fabs(velocity.x);
  }
  if (gestureRecognizer.view == _brush_rail &&
      [gestureRecognizer isKindOfClass:[UIPanGestureRecognizer class]])
  {
    if ([self isBrushListVisible]) {
      return NO;
    }
    CGPoint velocity = [(UIPanGestureRecognizer *)gestureRecognizer velocityInView:_overlay_view];
    return fabs(velocity.y) > fabs(velocity.x) * 0.4;
  }
  if (gestureRecognizer.view == _tool_drag_handle) {
    CGPoint velocity = [(UIPanGestureRecognizer *)gestureRecognizer velocityInView:_overlay_view];
    /* Down collapses. Up docks a temporarily opened rail at the higher position. */
    return [self isToolbarListVisible] &&
           (velocity.y > fabs(velocity.x) ||
            (!_tool_rail_docked_left_center && -velocity.y > fabs(velocity.x)));
  }
  if (gestureRecognizer.view == _tool_rail &&
      [gestureRecognizer isKindOfClass:[UIPanGestureRecognizer class]])
  {
    if ([self isToolbarListVisible]) {
      return NO;
    }
    CGPoint velocity = [(UIPanGestureRecognizer *)gestureRecognizer velocityInView:_overlay_view];
    return fabs(velocity.y) > fabs(velocity.x) * 0.4;
  }
  return YES;
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer
    shouldRecognizeSimultaneouslyWithGestureRecognizer:
        (UIGestureRecognizer *)otherGestureRecognizer
{
  UNUSED_VARS(gestureRecognizer, otherGestureRecognizer);
  return NO;
}

- (void)refreshScenePanelsFromBlender
{
  /* The mailboxes return immediately when unchanged, including while Blender is busy. */
  if (char *timeline = WM_apple_ios_ui_timeline_take_json()) {
    NSData *data = [NSData dataWithBytes:timeline length:strlen(timeline)];
    WM_apple_ios_ui_toolbar_free_json(timeline);
    [_timeline updateSnapshot:[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]];
  }
  if (char *assets = WM_apple_ios_ui_assets_take_json()) {
    NSData *data = [NSData dataWithBytes:assets length:strlen(assets)];
    WM_apple_ios_ui_toolbar_free_json(assets);
    [_assets updateSnapshot:[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]];
  }
  char *json = WM_apple_ios_ui_panels_take_json();
  if (!json) {
    return;
  }
  NSData *data = [NSData dataWithBytes:json length:strlen(json)];
  WM_apple_ios_ui_toolbar_free_json(json);
  NSDictionary *snapshot = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  [_scene_panels updateSnapshot:snapshot];
}

- (void)applyTemplatePresentation:(NSNotification *)notification
{
  [_scene_panels dismiss];
  const BOOL timeline = [notification.userInfo[@"timeline"] boolValue];
  if (_timeline.visible != timeline) {
    [_timeline toggle:nil];
  }
  [self.view setNeedsLayout];
}

- (void)newDocument:(id)sender
{
  wm_apple_ios_launcher_new_document(self);
}

- (void)buildMenuWithBuilder:(id<UIMenuBuilder>)builder
{
  [super buildMenuWithBuilder:builder];
  if (builder.system != UIMenuSystem.mainSystem || !_native_menubar_items.count) {
    return;
  }
  NSDictionary *systemIdentifiers = @{
    @"TOPBAR_MT_blender" : UIMenuApplication,
    @"TOPBAR_MT_file" : UIMenuFile,
    @"TOPBAR_MT_edit" : UIMenuEdit,
    @"TOPBAR_MT_window" : UIMenuWindow,
    @"TOPBAR_MT_help" : UIMenuHelp,
    @"VIEW3D_MT_view" : UIMenuView,
  };
  NSMutableArray *commands = [NSMutableArray array];
  for (NSDictionary *item in _native_menubar_items) {
    if ([item[@"separator"] boolValue] || ![item[@"title"] length]) {
      continue;
    }
    if (![item[@"submenu"] boolValue]) {
      [commands addObject:item];
      continue;
    }
    NSString *menuName = item[@"menu"];
    NSString *systemIdentifier = systemIdentifiers[menuName];
    NSString *identifier = systemIdentifier ?:
                                              [@"org.blender.menu."
                                   stringByAppendingString:menuName ?: item[@"title"]];
    UIMenu *menu = blenderIOSNativeSubmenu(item, identifier);
    if (systemIdentifier && [builder menuForIdentifier:systemIdentifier]) {
      if ([systemIdentifier isEqualToString:UIMenuApplication] ||
          [systemIdentifier isEqualToString:UIMenuWindow])
      {
        /* Keep iPadOS window-management and application commands available. */
        UIMenu *group = [UIMenu menuWithTitle:@""
                                        image:nil
                                   identifier:nil
                                      options:UIMenuOptionsDisplayInline
                                     children:menu.children];
        [builder insertChildMenu:group atEndOfMenuForIdentifier:systemIdentifier];
      }
      else {
        [builder replaceMenuForIdentifier:systemIdentifier withMenu:menu];
      }
    }
    else if ([builder menuForIdentifier:UIMenuWindow]) {
      [builder insertSiblingMenu:menu beforeMenuForIdentifier:UIMenuWindow];
    }
    else {
      [builder insertChildMenu:menu atEndOfMenuForIdentifier:UIMenuRoot];
    }
  }
  if (commands.count) {
    [builder insertChildMenu:[UIMenu menuWithTitle:@"Commands"
                                          children:blenderIOSNativeMenuElements(commands)]
        atEndOfMenuForIdentifier:UIMenuRoot];
  }
}

- (void)setOverlayShown:(BOOL)shown
{
  [self.navigationController setNavigationBarHidden:!shown animated:YES];
  UIView *overlay = _overlay_view;
  if (shown) {
    overlay.hidden = NO;
  }
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.25
      animations:^{
        overlay.alpha = shown ? 1 : 0;
      }
      completion:^(BOOL finished) {
        if (finished && overlay.alpha == 0) {
          overlay.hidden = YES;
        }
      }];
}

- (void)setInterfaceHidden:(BOOL)hidden
{
  if (_interface_hidden == hidden) {
    return;
  }
  _interface_hidden = hidden;
  [_pencil_menu cancel];
  [GHOST_ios_window_native(_ghost_window) setPencilContextMenu:hidden ? nil : _pencil_menu];
  [self setOverlayShown:!hidden];
  if (hidden && !_show_interface_button) {
    /* Faint until a Pencil or pointer comes near, so it does not compete with the work. */
    UIButtonConfiguration *config = WMIOSGlassButtonConfiguration(NO);
    config.image = [UIImage systemImageNamed:@"arrow.down.right.and.arrow.up.left"];
    config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    __block WMIOSViewController *controller = self;
    _show_interface_button = [[UIButton
        buttonWithConfiguration:config
                  primaryAction:[UIAction actionWithHandler:^(__kindof UIAction *) {
                    [controller setInterfaceHidden:NO];
                  }]] retain];
    _show_interface_button.accessibilityLabel = @"Show Interface";
    _show_interface_button.accessibilityIdentifier = @"blender.interface.show";
    _show_interface_button.translatesAutoresizingMaskIntoConstraints = NO;
    UIHoverGestureRecognizer *hover = [[[UIHoverGestureRecognizer alloc]
        initWithTarget:self
                action:@selector(hoverShowInterface:)] autorelease];
    [_show_interface_button addGestureRecognizer:hover];
    [self.view addSubview:_show_interface_button];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
      [_show_interface_button.topAnchor constraintEqualToAnchor:safe.topAnchor constant:12],
      [_show_interface_button.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor
                                                            constant:-12],
      [_show_interface_button.widthAnchor constraintEqualToConstant:44],
      [_show_interface_button.heightAnchor constraintEqualToConstant:44],
    ]];
    _show_interface_button.alpha = 0;
  }
  [self.view bringSubviewToFront:_show_interface_button];
  UIButton *button = _show_interface_button;
  button.hidden = NO;
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.25
      animations:^{
        button.alpha = hidden ? 0.35 : 0;
      }
      completion:^(BOOL) {
        button.hidden = !self->_interface_hidden;
      }];
#if !defined(APPLE_TARGET_VISIONOS)
  [self setNeedsStatusBarAppearanceUpdate];
#endif
  [self.view setNeedsLayout];
}

- (void)hoverShowInterface:(UIHoverGestureRecognizer *)hover
{
  const BOOL near = hover.state == UIGestureRecognizerStateBegan ||
                    hover.state == UIGestureRecognizerStateChanged;
  [UIView animateWithDuration:0.15
                   animations:^{
                     hover.view.alpha = near ? 1 : 0.35;
                   }];
}

- (BOOL)prefersStatusBarHidden
{
  return _interface_hidden;
}

- (void)setDesktopInterface:(BOOL)desktop
{
  if (desktop) {
    [self setInterfaceHidden:NO];
  }
  _desktop_interface = desktop;
  [NSUserDefaults.standardUserDefaults setBool:desktop forKey:@"BlenderDesktopInterface"];
  [_pencil_menu cancel];
  /* No native controls, radial menu or bottom tray over Blender's own interface. */
  [GHOST_ios_window_native(_ghost_window) setPencilContextMenu:desktop ? nil : _pencil_menu];
  if (desktop) {
    WM_apple_ios_ui_bottom_tray_set(0.0f);
  }
  [self setOverlayShown:!desktop];
  [self.view setNeedsLayout];
}

/* Blender tells UIKit when it published snapshots, see `GHOST_ios_mailbox_published`: update
 * from those that changed, without polling. */
- (void)blenderSnapshotsPublished:(NSNotification *)notification
{
  [self refreshScenePanelsFromBlender];
  if (char *json = WM_apple_ios_ui_toolbar_take_json()) {
    NSData *data = [NSData dataWithBytes:json length:strlen(json)];
    WM_apple_ios_ui_toolbar_free_json(json);
    NSDictionary *model = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if ([model isKindOfClass:NSDictionary.class]) {
      [self applyToolbarModel:model];
    }
  }
  [self refreshNativeMenuBar];
}

- (void)refreshToolbarFromBlender
{
  [self updateViewportLoadingIndicator];
  [self applyToolbarModel:[self toolbarModel]];
  [self refreshNativeMenuBar];
}

- (void)applyToolbarModel:(NSDictionary *)model
{
  if ([model[@"desktop"] isKindOfClass:NSNumber.class] &&
      [model[@"desktop"] boolValue] != _desktop_interface)
  {
    /* Blender's own Window menu can switch the interface too. */
    [self setDesktopInterface:[model[@"desktop"] boolValue]];
  }
  [_redo_strip updateSnapshot:model[@"redo"]];
  NSArray *navigation_geometry = model[@"navigationGizmo"];
  if ([navigation_geometry isKindOfClass:NSArray.class] && navigation_geometry.count == 3 &&
      ![_view_gizmo_geometry isEqual:navigation_geometry])
  {
    [_view_gizmo_geometry release];
    _view_gizmo_geometry = [navigation_geometry copy];
    [self layoutViewPie];
  }
  NSArray *tools = model[@"tools"];
  NSArray *actions = model[@"actions"];
  NSString *mode = model[@"mode"];
  NSString *mesh_select_mode = model[@"meshSelectMode"];
  NSString *active_tool = model[@"active"];
  if (![tools isKindOfClass:[NSArray class]]) {
    return;
  }
  if (![actions isKindOfClass:[NSArray class]]) {
    actions = @[];
  }
  if (![mode isKindOfClass:[NSString class]]) {
    mode = @"";
  }
  if (![active_tool isKindOfClass:[NSString class]]) {
    active_tool = @"";
  }
  if (![mesh_select_mode isKindOfClass:[NSString class]]) {
    mesh_select_mode = @"";
  }

  if ([model[@"modes"] isKindOfClass:NSArray.class] && [model[@"modes"] count]) {
    [self setModePickerModes:model[@"modes"] current:model[@"objectMode"]];
  }
  else {
    [self setModePickerTitleForMode:mode];
  }
  [self setViewportShadingValue:model[@"shading"]];
  if ([model[@"viewport"] isKindOfClass:NSDictionary.class] &&
      ![_viewport_state isEqual:model[@"viewport"]])
  {
    [_viewport_state release];
    _viewport_state = [model[@"viewport"] copy];
  }
  [_render updateStatus:model[@"render"]];
  [_settings updatePreferences:model[@"preferences"]];
  [self setUndoAvailable:model[@"canUndo"]];
  [self setMeshSelectModeValue:mesh_select_mode];
  [self setTopSettingsValues:model[@"settings"]];
  [self setMeshSelectModeVisible:[mode isEqualToString:@"mesh_edit"] animated:YES];

  NSString *signature = [self toolbarSignatureForTools:tools mode:mode];
  if (_toolbar_signature == nil || ![_toolbar_signature isEqualToString:signature]) {
    [_toolbar_signature release];
    _toolbar_signature = [signature copy];
    [self rebuildToolbarWithTools:tools mode:mode];
  }

  [self setActiveToolbarToolIdentifier:active_tool];
  [self updateBrushRailWithModel:model[@"brushes"]];
  BOOL transform = [self isTransformToolIdentifier:active_tool];
  UIBarButtonItemGroup *tool_group = [_scene_panels updateToolSettings:model[@"toolSettings"]
                                                       acknowledgement:model[@"propertiesAck"]];
  [self setTopSettingsVisible:transform animated:YES];
  UIBarButtonItemGroup *settings_group = transform || !tool_group ? _top_settings_group :
                                                                    tool_group;
  if (_top_bar_item.centerItemGroups.firstObject != settings_group) {
    [_scene_panels hideToolSettings];
    _top_bar_item.centerItemGroups = @[ settings_group ];
  }
  NSString *action_signature = [self actionSignatureForActions:actions];
  if (_action_signature == nil || ![_action_signature isEqualToString:action_signature]) {
    [_action_signature release];
    _action_signature = [action_signature copy];
    [_quick_actions release];
    _quick_actions = [actions copy];
    /* Build immediately: a delayed completion could restore a finished transform's buttons. */
    [self rebuildActionStripWithActions:actions];
  }
  [self updateAxisConstraintsWithActions:actions];
  [self updateActionsButtonMenu];
}

- (void)viewDidLayoutSubviews
{
  [super viewDidLayoutSubviews];
  if (!_touch_settings_applied && self.view.window && _settings &&
      UIDevice.currentDevice.userInterfaceIdiom != UIUserInterfaceIdiomPhone)
  {
    /* The phone has its own one-finger button and defaults to using the tool. */
    _touch_settings_applied = YES;
    [self setFingerInputMode:[_settings.class fingerUsesTool]];
  }
  CGRect panel_rect = _overlay_view.safeAreaLayoutGuide.layoutFrame;
  /* The bottom tray: the open asset pane along the edge and the timeline just above it. The quick
   * actions, the Adjust pill, the collapsed rails and the navigation gizmo rest on top, so the
   * tray never leaves a gap and follows the timeline as it opens or expands. */
  const BOOL tray_shown = !_desktop_interface && !_interface_hidden;
  const CGFloat assets_tray = _assets.visible && tray_shown ? _assets.height + 8 : 0;
#if defined(APPLE_TARGET_VISIONOS)
  const CGFloat timeline_tray = 0; /* A window of its own. */
#else
  const CGFloat timeline_tray = _timeline.visible && tray_shown ? _timeline.height + 8 : 0;
#endif
  const CGFloat tray = assets_tray + timeline_tray;
  if (fabs(tray - _bottom_tray) > 0.5) {
    _bottom_tray = tray;
    _action_bottom_constraint.constant = -6.0 - tray;
    [self updateBrushRailGeometry];
    /* Lay out again now, inside any running animation, so the controls move with the tray. The
     * nested pass sees the new tray and completes the layout. */
    [self.view setNeedsLayout];
    [self.view layoutIfNeeded];
    return;
  }
  const CGFloat window_height = _overlay_view.bounds.size.height;
  if (window_height > 0 && !_desktop_interface) {
    const CGFloat tray_top = CGRectGetMaxY(panel_rect) - 6 - (tray > 0 ? tray - 8 : 0);
    WM_apple_ios_ui_bottom_tray_set(tray > 0 ? float((window_height - tray_top) / window_height) :
                                               0.0f);
  }
  const CGRect bar_rect = [_top_bar convertRect:_top_bar.bounds toView:_overlay_view];
  const CGFloat panel_top = MAX(CGRectGetMinY(panel_rect), CGRectGetMaxY(bar_rect));
  CGFloat panel_bottom = MIN(CGRectGetMaxY(panel_rect) - 64 - _bottom_tray,
                             CGRectGetMinY(_overlay_view.keyboardLayoutGuide.layoutFrame));
  [self layoutViewPie];
  [self updateRedoPillLayout:NO];
  CGRect redo_rect = _overlay_view.safeAreaLayoutGuide.layoutFrame;
  const CGFloat rails_right = MAX(CGRectGetMaxX(_tool_rail.frame),
                                  _brush_rail.hidden ? 0.0 : CGRectGetMaxX(_brush_rail.frame));
  const CGFloat redo_left = MAX(CGRectGetMinX(redo_rect) + 12, rails_right + 12);
  CGFloat redo_right = CGRectGetMaxX(redo_rect) - 12;
  CGFloat redo_bottom = MIN(CGRectGetMinY(_action_rail.frame) - 8,
                            CGRectGetMinY(_overlay_view.keyboardLayoutGuide.layoutFrame) - 8);
  if (redo_rect.size.width < 700) {
    redo_bottom = MIN(redo_bottom, CGRectGetMinY(_view_gizmo_hotspot.frame) - 8);
  }
  else {
    redo_right = MIN(redo_right, CGRectGetMinX(_view_gizmo_hotspot.frame) - 12);
  }
  /* The asset pane spans the full width along the bottom edge, and the timeline sits on the edge
   * or on the asset pane; everything else sits above them. The tray keeps the same distance from
   * the sides of the window as from its bottom. */
  const CGRect bounds = _overlay_view.bounds;
  const CGFloat tray_bottom = CGRectGetMaxY(redo_rect) - 6;
  const CGFloat edge = CGRectGetMaxY(bounds) - tray_bottom;
  const CGFloat tray_left = MAX(CGRectGetMinX(redo_rect), CGRectGetMinX(bounds) + edge);
  const CGFloat tray_width = MAX(
      0, MIN(CGRectGetMaxX(redo_rect), CGRectGetMaxX(bounds) - edge) - tray_left);
  [_assets
      layoutInRect:CGRectMake(tray_left, panel_top, tray_width, MAX(0, tray_bottom - panel_top))];
  [_timeline layoutInRect:CGRectMake(tray_left,
                                     panel_top,
                                     tray_width,
                                     MAX(0, tray_bottom - assets_tray - panel_top))];
  panel_rect.origin.y = panel_top;
  panel_rect.size.height = MAX(0, panel_bottom - panel_top);
  [_scene_panels layoutInRect:panel_rect];
  _redo_lane_rect = CGRectMake(redo_left,
                               panel_top + 8,
                               MAX(0, redo_right - redo_left),
                               MAX(60, redo_bottom - panel_top - 8));
  _redo_lane_wide = redo_rect.size.width >= 700;
  [self layoutRedoStrip];
  [self layoutToolbarLabel];
  [self updateMTKViewDrawableScale];
  [self updateToolbarHeightForCurrentState];
  [self updateBrushRailGeometry];
  if (_top_bar_height_constraint) {
    const CGFloat height =
        [_top_bar sizeThatFits:CGSizeMake(_top_bar.bounds.size.width, CGFLOAT_MAX)].height;
    _top_bar_height_constraint.constant = MAX(44.0, height);
  }
  if (_action_phone_bottom_constraint) {
    [self updatePhoneActionStripLayout];
  }
}

- (void)installIPadOverlay
{
  if (_overlay_view != nil) {
    return;
  }

  _overlay_view = [[WMIOSPassThroughOverlayView alloc] initWithFrame:self.view.bounds];
  _overlay_view.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                                   UIViewAutoresizingFlexibleHeight;
  _overlay_view.backgroundColor = UIColor.clearColor;
  [self.view addSubview:_overlay_view];

  _mesh_select_buttons = [[NSMutableArray alloc] init];
  _redo_strip = [[blenderIOSSwiftClass(@"WMIOSSwiftRedoStrip") alloc]
      initWithOverlay:_overlay_view
                 icon:^UIImage *(NSString *name) {
                   return WMIOSBlenderIcon(name, NO);
                 }];
  _scene_panels = [[blenderIOSSwiftClass(@"WMIOSSwiftScenePanels") alloc]
      initWithOverlay:_overlay_view
                 icon:^UIImage *(NSString *name) {
                   return WMIOSBlenderIcon(name, NO);
                 }];
  _scene_panels.menuProvider = ^UIMenu *(NSDictionary *property) {
    NSMutableDictionary *route = [NSMutableDictionary
        dictionaryWithDictionary:@{@"root" : property[@"menu"], @"path" : @[]}];
    if (property[@"tab"]) {
      route[@"propertiesTab"] = property[@"tab"];
    }
    return blenderIOSNativeSubmenu(
        @{@"title" : property[@"label"], @"icon" : property[@"icon"] ?: @"", @"route" : route},
        property[@"menu"]);
  };
  _timeline = [[blenderIOSSwiftClass(@"WMIOSSwiftTimeline") alloc] initWithOverlay:_overlay_view];
  __block UIView *timeline_host = self.view;
  /* The bottom tray grows and shrinks; everything resting on it moves with it. */
  void (^relayout_tray)(void) = ^{
    [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.3
                          delay:0
         usingSpringWithDamping:0.9
          initialSpringVelocity:0
                        options:UIViewAnimationOptionBeginFromCurrentState |
                                UIViewAnimationOptionAllowUserInteraction
                     animations:^{
                       [timeline_host setNeedsLayout];
                       [timeline_host layoutIfNeeded];
                     }
                     completion:nil];
  };
  _timeline.visibilityDidChange = relayout_tray;
  _assets = [[blenderIOSSwiftClass(@"WMIOSSwiftAssets") alloc] initWithOverlay:_overlay_view];
  _assets.visibilityDidChange = relayout_tray;
  /* Assets dragged out of the pane drop onto the viewport, which Blender draws in this view. */
  __block WMIOSViewController *assets_host = self;
  _assets.clientPoint = ^CGPoint(CGPoint point) {
    GHOST_WindowIOS *ghost_window = assets_host->_ghost_window;
    return ghost_window ? GHOST_ios_window_pixels(ghost_window, point) : point;
  };
  [self.view addInteraction:_assets.dropInteraction];
  /* Files dragged in from other apps drop where they land, as on a Mac. */
  _file_drop = [[WMIOSFileDrop alloc] init];
  _file_drop.dropFiles = ^(NSArray<NSString *> *paths, CGPoint point) {
    if (GHOST_WindowIOS *ghost_window = assets_host->_ghost_window) {
      GHOST_ios_window_drop_files(ghost_window, paths, point);
    }
  };
  [self.view addInteraction:_file_drop.interaction];
#if defined(APPLE_TARGET_VISIONOS)
  /* Ornaments beside the window, for the tools and the quick actions. */
  if (Class chrome_class = NSClassFromString(@"WMIOSVisionChrome")) {
    __block WMIOSViewController *chrome_owner = self;
    _vision_chrome = [[chrome_class alloc]
        initWithController:self
                    choose:^(NSString *tool_id) {
                      [chrome_owner setActiveToolbarToolIdentifier:tool_id];
                      WM_apple_ios_ui_tool_set_by_id(tool_id.UTF8String);
                    }];
  }
#endif
  [self installNativeTopBar];
  /* A navigation controller's bar is outside the overlay; the safe area already clears it. */
  _compile_indicator = [[WMIOSCompileIndicator alloc]
      initWithOverlay:_overlay_view
                below:self.navigationController ? nil : _top_bar];
#if !defined(APPLE_TARGET_VISIONOS)
  /* Apple Pencil's palette; visionOS has no Pencil to bring it up. */
  _pencil_menu = [[blenderIOSSwiftClass(@"WMIOSSwiftPencilMenu") alloc]
      initWithFrame:_overlay_view.bounds];
  _pencil_menu.delegate = self;
#endif
  const BOOL phone = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone;
  [GHOST_ios_window_native(_ghost_window) setPencilContextMenu:_pencil_menu];
  _toolbar_buttons = [[NSMutableArray alloc] init];
  _action_buttons = [[NSMutableArray alloc] init];
  _view_pie_buttons = [[NSMutableArray alloc] init];
  _toolbar_expanded = NO;
  _view_pie_visible = NO;

  _tool_rail_glass_effect = [WMIOSGlassEffect(YES, YES) retain];
  _tool_rail = [[[WMIOSPassThroughVisualEffectView alloc] initWithEffect:_tool_rail_glass_effect]
      autorelease];
  _tool_rail.translatesAutoresizingMaskIntoConstraints = NO;
  _tool_rail.clipsToBounds = YES;
  _tool_rail.layer.cornerRadius = 32.0;
  _tool_rail.layer.cornerCurve = kCACornerCurveContinuous;
  [_overlay_view addSubview:_tool_rail];

  UIPanGestureRecognizer *tool_rail_pan = [[UIPanGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleToolbarDockPan:)];
  tool_rail_pan.maximumNumberOfTouches = 1;
  tool_rail_pan.cancelsTouchesInView = YES;
  tool_rail_pan.delaysTouchesBegan = NO;
  tool_rail_pan.delegate = self;
  [_tool_rail addGestureRecognizer:tool_rail_pan];
  [tool_rail_pan release];
#if defined(APPLE_TARGET_VISIONOS)
  /* Tools float beside the window as an ornament instead. */
  _tool_rail.hidden = YES;
#endif

  _tool_drag_handle = [self toolbarDragHandle];
  [_tool_rail.contentView addSubview:_tool_drag_handle];
  _tool_drag_handle_height_constraint = [_tool_drag_handle.heightAnchor
      constraintEqualToConstant:0.0];

  _tool_scroll = [[WMIOSButtonScrollView alloc] initWithFrame:CGRectZero];
  UIHoverGestureRecognizer *tool_hover = [[UIHoverGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleToolbarHover:)];
  [_tool_scroll addGestureRecognizer:tool_hover];
  [tool_hover release];
  _tool_scroll.translatesAutoresizingMaskIntoConstraints = NO;
  _tool_scroll.backgroundColor = UIColor.clearColor;
  _tool_scroll.clipsToBounds = YES;
  _tool_scroll.showsVerticalScrollIndicator = NO;
  _tool_scroll.alwaysBounceVertical = YES;
  _tool_scroll.canCancelContentTouches = YES;
  _tool_scroll.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
  [_tool_rail.contentView addSubview:_tool_scroll];

  _tool_stack = [[WMIOSToolStackView alloc] initWithFrame:CGRectZero];
  _tool_stack.axis = UILayoutConstraintAxisVertical;
  _tool_stack.alignment = UIStackViewAlignmentFill;
  _tool_stack.distribution = UIStackViewDistributionEqualSpacing;
  _tool_stack.spacing = 2.0;
  _tool_stack.translatesAutoresizingMaskIntoConstraints = NO;
  [_tool_scroll addSubview:_tool_stack];
  [self installBrushRail];

  _action_rail = [[[WMIOSPassThroughVisualEffectView alloc] initWithEffect:nil] autorelease];
  _action_rail.translatesAutoresizingMaskIntoConstraints = NO;
  _action_rail.backgroundColor = UIColor.clearColor;
  _action_rail.clipsToBounds = YES;
  _action_rail.layer.cornerRadius = 0.0;
  _action_rail.layer.cornerCurve = kCACornerCurveContinuous;
  _action_rail.hidden = YES;
  _action_rail.alpha = 0.0;
  [_overlay_view addSubview:_action_rail];

  _action_scroll = [[WMIOSFadingButtonScrollView alloc] initWithFrame:CGRectZero];
  _action_scroll.translatesAutoresizingMaskIntoConstraints = NO;
  _action_scroll.backgroundColor = UIColor.clearColor;
  _action_scroll.clipsToBounds = YES;
  _action_scroll.showsHorizontalScrollIndicator = NO;
  _action_scroll.alwaysBounceHorizontal = NO;
  _action_scroll.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
  [_action_rail.contentView addSubview:_action_scroll];

  _action_stack = [[WMIOSPassThroughStackView alloc] initWithFrame:CGRectZero];
  _action_stack.axis = UILayoutConstraintAxisHorizontal;
  _action_stack.alignment = UIStackViewAlignmentCenter;
  _action_stack.distribution = UIStackViewDistributionFill;
  _action_stack.spacing = 4.0;
  _action_stack.translatesAutoresizingMaskIntoConstraints = NO;
  [_action_scroll addSubview:_action_stack];
  [self installActionStripToggle];

  NSArray *view_pie_specs = @[
    @{
      @"title" : @"Frame All",
      @"symbol" : @"rectangle.expand.vertical",
      @"command" : @(WM_IOS_UI_COMMAND_VIEW_ALL)
    },
    @{
      @"title" : @"Frame Selected",
      @"symbol" : @"viewfinder",
      @"command" : @(WM_IOS_UI_COMMAND_FRAME_SELECTED)
    },
    @{@"title" : @"Camera", @"symbol" : @"camera", @"command" : @(WM_IOS_UI_COMMAND_VIEW_CAMERA)},
    @{
      @"title" : @"Perspective",
      @"symbol" : @"cube.transparent",
      @"command" : @(WM_IOS_UI_COMMAND_VIEW_PERSP_ORTHO)
    },
    @{
      @"title" : @"Local View",
      @"symbol" : @"scope",
      @"command" : @(WM_IOS_UI_COMMAND_VIEW_LOCAL)
    },
    @{
      @"title" : @"Front",
      @"symbol" : @"arrow.up.square",
      @"command" : @(WM_IOS_UI_COMMAND_VIEW_FRONT)
    },
    @{
      @"title" : @"Right",
      @"symbol" : @"arrow.right.square",
      @"command" : @(WM_IOS_UI_COMMAND_VIEW_RIGHT)
    },
    @{
      @"title" : @"Top",
      @"symbol" : @"arrow.down.to.line.square",
      @"command" : @(WM_IOS_UI_COMMAND_VIEW_TOP)
    },
  ];

  for (NSDictionary *spec in view_pie_specs) {
    UIButton *button = [self viewPieButtonWithTitle:spec[@"title"]
                                        systemImage:spec[@"symbol"]
                                            command:[spec[@"command"] integerValue]];
    [_overlay_view addSubview:button];
  }
  if (phone) {
    NSMutableArray *actions = [NSMutableArray array];
    for (NSDictionary *spec in view_pie_specs) {
      [actions addObject:[self viewportShadingActionWithTitle:spec[@"title"]
                                                  systemImage:spec[@"symbol"]
                                                      command:[spec[@"command"] integerValue]]];
    }
    UIBarButtonItem *view = [[[UIBarButtonItem alloc]
        initWithImage:[UIImage systemImageNamed:@"eye"]
                 menu:[UIMenu menuWithTitle:@"View" children:actions]] autorelease];
    view.title = @"View";
    view.accessibilityLabel = @"View";
    view.menuRepresentation = view.menu;
    _top_bar_item.trailingItemGroups = [_top_bar_item.trailingItemGroups
        arrayByAddingObject:view.creatingFixedGroup];
  }

  UIButton *gizmo_control = [UIButton buttonWithType:UIButtonTypeCustom];
  _view_gizmo_hotspot = gizmo_control;
  _view_gizmo_hotspot.translatesAutoresizingMaskIntoConstraints = YES;
  _view_gizmo_hotspot.backgroundColor = UIColor.clearColor;
  _view_gizmo_hotspot.accessibilityIdentifier = @"blender.viewport.navigation";
  _view_gizmo_hotspot.accessibilityLabel = @"View navigation";
  [_overlay_view addSubview:_view_gizmo_hotspot];
#if !defined(APPLE_TARGET_VISIONOS)
  _view_gizmo_selection_feedback = [[UISelectionFeedbackGenerator
      feedbackGeneratorForView:_view_gizmo_hotspot] retain];
  _view_gizmo_open_feedback = [[UIImpactFeedbackGenerator
      feedbackGeneratorWithStyle:UIImpactFeedbackStyleSoft
                         forView:_view_gizmo_hotspot] retain];
#endif
  _view_gizmo_feedback = [[[UIView alloc] initWithFrame:CGRectMake(14, 14, 84, 84)] autorelease];
  _view_gizmo_feedback.userInteractionEnabled = NO;
  _view_gizmo_feedback.layer.cornerRadius = 42;
  _view_gizmo_feedback.layer.borderWidth = 1;
  _view_gizmo_feedback.alpha = 0;
  [_view_gizmo_hotspot addSubview:_view_gizmo_feedback];
  [gizmo_control addTarget:self
                    action:@selector(handleViewGizmoPress:)
          forControlEvents:UIControlEventTouchDown];
  [gizmo_control addTarget:self
                    action:@selector(handleViewGizmoRelease:)
          forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside |
                           UIControlEventTouchCancel];

  UIHoverGestureRecognizer *view_gizmo_hover = [[UIHoverGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleViewGizmoHover:)];
  [_view_gizmo_hotspot addGestureRecognizer:view_gizmo_hover];
  [view_gizmo_hover release];

  UILongPressGestureRecognizer *view_gizmo_hold = [[UILongPressGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleViewGizmoHold:)];
  view_gizmo_hold.minimumPressDuration = 0.32;
  view_gizmo_hold.allowableMovement = 10.0;
  view_gizmo_hold.allowedTouchTypes = @[ @(UITouchTypeDirect), @(UITouchTypePencil) ];
  [_view_gizmo_hotspot addGestureRecognizer:view_gizmo_hold];

  UITapGestureRecognizer *view_gizmo_tap = [[UITapGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleViewGizmoTap:)];
  view_gizmo_tap.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  [view_gizmo_tap requireGestureRecognizerToFail:view_gizmo_hold];
  [_view_gizmo_hotspot addGestureRecognizer:view_gizmo_tap];
  [view_gizmo_tap release];

  UIPanGestureRecognizer *view_gizmo_drag = [[UIPanGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleViewGizmoDrag:)];
  view_gizmo_drag.maximumNumberOfTouches = 1;
  view_gizmo_drag.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  view_gizmo_drag.cancelsTouchesInView = YES;
  view_gizmo_drag.delegate = self;
  [view_gizmo_drag requireGestureRecognizerToFail:view_gizmo_hold];
  [_view_gizmo_hotspot addGestureRecognizer:view_gizmo_drag];
  [view_gizmo_drag release];
  [view_gizmo_hold release];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(cancelViewGizmoInteraction)
                                             name:UIApplicationWillResignActiveNotification
                                           object:nil];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(applyTemplatePresentation:)
                                             name:@"BlenderTemplatePresentation"
                                           object:nil];

  UILayoutGuide *safe_area = _overlay_view.safeAreaLayoutGuide;
  /* The radial view is frontmost and passes through when the palette is closed. */
  if (_pencil_menu) {
    [_overlay_view addSubview:_pencil_menu];
  }
  _tool_rail_width_constraint = [_tool_rail.widthAnchor constraintEqualToConstant:64.0];
  _tool_rail_height_constraint = [_tool_rail.heightAnchor constraintEqualToConstant:64.0];
  _tool_rail_bottom_constraint = [[_tool_rail.bottomAnchor
      constraintEqualToAnchor:safe_area.bottomAnchor
                     constant:-12.0] retain];
  _tool_rail_center_y_constraint = [[_tool_rail.centerYAnchor
      constraintEqualToAnchor:safe_area.centerYAnchor] retain];
  /* Center a compact row when it fits; grow the scrollable content, not the
   * buttons or reserved viewport, when it overflows or the tool rail expands. */
  NSLayoutConstraint *action_content_width = [_action_scroll.contentLayoutGuide.widthAnchor
      constraintEqualToAnchor:_action_scroll.frameLayoutGuide.widthAnchor];
  action_content_width.priority = UILayoutPriorityDefaultHigh;
  [NSLayoutConstraint activateConstraints:@[
    [_tool_rail.leadingAnchor constraintEqualToAnchor:safe_area.leadingAnchor constant:14.0],
    _tool_rail_bottom_constraint,
    _tool_rail_width_constraint,
    [_tool_rail.topAnchor constraintGreaterThanOrEqualToAnchor:safe_area.topAnchor constant:16.0],
    _tool_rail_height_constraint,

    [_tool_scroll.leadingAnchor constraintEqualToAnchor:_tool_rail.contentView.leadingAnchor],
    [_tool_scroll.trailingAnchor constraintEqualToAnchor:_tool_rail.contentView.trailingAnchor],
    [_tool_drag_handle.leadingAnchor constraintEqualToAnchor:_tool_rail.contentView.leadingAnchor],
    [_tool_drag_handle.trailingAnchor
        constraintEqualToAnchor:_tool_rail.contentView.trailingAnchor],
    [_tool_drag_handle.topAnchor constraintEqualToAnchor:_tool_rail.contentView.topAnchor],
    _tool_drag_handle_height_constraint,
    [_tool_scroll.topAnchor constraintEqualToAnchor:_tool_drag_handle.bottomAnchor],
    [_tool_scroll.bottomAnchor constraintEqualToAnchor:_tool_rail.contentView.bottomAnchor],

    [_tool_stack.leadingAnchor
        constraintEqualToAnchor:_tool_scroll.contentLayoutGuide.leadingAnchor
                       constant:6.0],
    [_tool_stack.trailingAnchor
        constraintEqualToAnchor:_tool_scroll.contentLayoutGuide.trailingAnchor
                       constant:-6.0],
    [_tool_stack.topAnchor constraintEqualToAnchor:_tool_scroll.contentLayoutGuide.topAnchor
                                          constant:6.0],
    [_tool_stack.bottomAnchor constraintEqualToAnchor:_tool_scroll.contentLayoutGuide.bottomAnchor
                                             constant:-6.0],
    [_tool_stack.widthAnchor constraintEqualToAnchor:_tool_scroll.frameLayoutGuide.widthAnchor
                                            constant:-12.0],

    [_action_scroll.leadingAnchor constraintEqualToAnchor:_action_group.leadingAnchor
                                                 constant:48.0],
    [_action_scroll.topAnchor constraintEqualToAnchor:_action_rail.contentView.topAnchor
                                             constant:0.0],
    [_action_scroll.bottomAnchor constraintEqualToAnchor:_action_rail.contentView.bottomAnchor
                                                constant:0.0],

    action_content_width,
    [_action_scroll.contentLayoutGuide.widthAnchor
        constraintGreaterThanOrEqualToAnchor:_action_scroll.frameLayoutGuide.widthAnchor],
    [_action_stack.centerXAnchor
        constraintEqualToAnchor:_action_scroll.contentLayoutGuide.centerXAnchor],
    [_action_stack.leadingAnchor
        constraintGreaterThanOrEqualToAnchor:_action_scroll.contentLayoutGuide.leadingAnchor
                                    constant:6.0],
    [_action_stack.trailingAnchor
        constraintLessThanOrEqualToAnchor:_action_scroll.contentLayoutGuide.trailingAnchor
                                 constant:-6.0],
    [_action_stack.topAnchor constraintEqualToAnchor:_action_scroll.contentLayoutGuide.topAnchor
                                            constant:6.0],
    [_action_stack.bottomAnchor
        constraintEqualToAnchor:_action_scroll.contentLayoutGuide.bottomAnchor
                       constant:-6.0],
    [_action_stack.heightAnchor
        constraintEqualToAnchor:_action_scroll.frameLayoutGuide.heightAnchor
                       constant:-12.0],

  ]];
  [self installBrushRailLayout];
  [self installActionStripLayout];

  [self layoutViewPie];
}

- (void)handleGesture:(UIGestureRecognizer *)gestureRecognizer
{
}

- (BOOL)prefersHomeIndicatorAutoHidden
{
  /* Make the Home Indicator (the bottom-center white navigation bar) auto-hide when possible. */
  return YES;
}

@end
