/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Test-only dylib, embedded in a temporary signed device build, never shipped. */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <cstring>
#include <dlfcn.h>

@interface NSObject (BlenderResponsivenessTest)
- (void)setToolbarExpanded:(BOOL)expanded animated:(BOOL)animated;
- (void)handleViewGizmoHover:(UIHoverGestureRecognizer *)gesture;
- (void)handleViewGizmoHold:(UILongPressGestureRecognizer *)gesture;
- (void)cancelViewGizmoInteraction;
- (void)setToolRailDockedLeftCenter:(BOOL)docked animated:(BOOL)animated;
- (void)handleToolbarHover:(UIHoverGestureRecognizer *)gesture;
@end

@interface BlenderToolbarProbeHover : UIHoverGestureRecognizer
@property(nonatomic) UIGestureRecognizerState probeState;
@property(nonatomic) CGPoint probePoint;
@end
@implementation BlenderToolbarProbeHover
- (UIGestureRecognizerState)state { return self.probeState; }
- (CGPoint)locationInView:(UIView *)view { return self.probePoint; }
@end

@interface BlenderGizmoProbeHold : UILongPressGestureRecognizer
@property(nonatomic) UIGestureRecognizerState probeState;
@property(nonatomic) CGPoint probePoint;
@end
@implementation BlenderGizmoProbeHold
- (UIGestureRecognizerState)state { return self.probeState; }
- (CGPoint)locationInView:(UIView *)view { return self.probePoint; }
@end

@interface BlenderGizmoProbeHover : UIHoverGestureRecognizer
@property(nonatomic) UIGestureRecognizerState probeState;
@end
@implementation BlenderGizmoProbeHover
- (UIGestureRecognizerState)state { return self.probeState; }
@end

@interface BlenderDeviceProbe : NSObject
@property(nonatomic, strong) CADisplayLink *link;
@property(nonatomic, strong) UIViewController *controller;
@property(nonatomic, strong) UIScrollView *scroll;
@property(nonatomic) CFTimeInterval start;
@property(nonatomic) CFTimeInterval last;
@property(nonatomic) double maximumGap;
@property(nonatomic) NSInteger ticks;
@property(nonatomic) NSInteger commands;
@property(nonatomic) NSInteger snapshots;
@property(nonatomic) CGFloat scrollDistance;
@property(nonatomic) BOOL expanded;
@property(nonatomic) BOOL scrolled;
@property(nonatomic) BOOL engineOnMain;
@end

@implementation BlenderDeviceProbe
- (void)tick:(CADisplayLink *)link
{
  NSAssert(NSThread.isMainThread, @"UI probe must run on main");
  double elapsed = link.timestamp - self.start;
  if (self.last) {
    self.maximumGap = MAX(self.maximumGap, link.timestamp - self.last);
  }
  self.last = link.timestamp;
  self.ticks++;
  auto snapshot = (char *(*)())dlsym(RTLD_DEFAULT, "WM_apple_ios_ui_toolbar_copy_json");
  char *json = snapshot();
  if (json && strlen(json) > 2) {
    self.snapshots++;
  }
  free(json);
  if (elapsed > 0.5 && !self.expanded) {
    [self.controller setToolbarExpanded:YES animated:YES];
    self.expanded = YES;
  }
  if (elapsed > 1.5 && !self.scrolled) {
    CGFloat bottom = MAX(0, self.scroll.contentSize.height - self.scroll.bounds.size.height);
    [self.scroll setContentOffset:CGPointMake(0, bottom) animated:YES];
    self.scrolled = YES;
  }
  self.scrollDistance = MAX(self.scrollDistance, self.scroll.contentOffset.y);
  if (self.commands < 20 && elapsed > 0.2 * (self.commands + 1)) {
    auto enqueue = (void (*)(const char *))dlsym(RTLD_DEFAULT, "WM_apple_ios_ui_operator_enqueue");
    enqueue("WM_OT_responsive_device_probe");
    self.commands++;
  }
}
@end

static BlenderDeviceProbe *probe;

static UIViewController *blenderController()
{
  for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
    if (![scene isKindOfClass:UIWindowScene.class]) {
      continue;
    }
    for (UIWindow *window in ((UIWindowScene *)scene).windows) {
      UIViewController *controller = window.rootViewController;
      if ([controller isKindOfClass:UINavigationController.class]) {
        controller = ((UINavigationController *)controller).topViewController;
      }
      if ([controller respondsToSelector:@selector(setToolbarExpanded:animated:)]) {
        return controller;
      }
    }
  }
  return nil;
}

static NSArray *rectValues(CGRect rect)
{
  return @[ @(rect.origin.x), @(rect.origin.y), @(rect.size.width), @(rect.size.height) ];
}

static UIViewController *blenderPresentedController()
{
  UIViewController *presented = blenderController().view.window.rootViewController.presentedViewController;
  while (presented.presentedViewController) {
    presented = presented.presentedViewController;
  }
  return presented;
}

extern "C" char *blender_device_probe_scene_panels(const char *request_json)
{
  NSString *json = request_json ? [NSString stringWithUTF8String:request_json] : @"{}";
  NSDictionary *request = [NSJSONSerialization JSONObjectWithData:[json dataUsingEncoding:NSUTF8StringEncoding]
                                                         options:0 error:nil];
  __block char *result = nullptr;
  dispatch_sync(dispatch_get_main_queue(), ^{
    id panels = [blenderController() valueForKey:@"_scene_panels"];
    UIBarButtonItem *inspector = [panels valueForKey:@"inspectorButton"];
    UIBarButtonItem *hierarchy = [panels valueForKey:@"hierarchyButton"];
    NSString *action = request[@"action"];
    if ([action isEqual:@"inspector"] || [action isEqual:@"hierarchy"]) {
      UIBarButtonItem *button = [action isEqual:@"inspector"] ? inspector : hierarchy;
      [button.target performSelector:button.action withObject:button];
    }
    if ([action isEqual:@"tab"]) {
      UIStackView *tabs = [panels valueForKey:@"_tabsStack"];
      for (UIButton *button in tabs.arrangedSubviews) {
        if ([button.accessibilityIdentifier isEqual:request[@"id"]]) {
          [button sendActionsForControlEvents:UIControlEventTouchUpInside];
        }
      }
    }
    if ([action isEqual:@"section"]) {
      UITableView *form = [panels valueForKey:@"_form"];
      NSArray *identifiers = [panels valueForKey:@"_sectionIdentifiers"];
      NSUInteger index = [identifiers indexOfObject:request[@"id"]];
      if (index != NSNotFound) {
        UIView *view = [form.delegate tableView:form viewForHeaderInSection:index];
        UIButton *header = [view isKindOfClass:UIButton.class] ? (UIButton *)view :
                            (UIButton *)((UIStackView *)view).arrangedSubviews.firstObject;
        [header sendActionsForControlEvents:UIControlEventTouchUpInside];
      }
    }
    NSDictionary *controls = [panels valueForKey:@"_controls"];
    if ([action isEqual:@"color_open"]) {
      UIControl *button = controls[request[@"key"]];
      UITableView *form = [panels valueForKey:@"_form"];
      NSArray *sections = [panels valueForKey:@"_formSections"];
      for (NSUInteger section = 0; section < sections.count; section++) {
        NSArray *rows = sections[section];
        for (NSUInteger row = 0; row < rows.count; row++) {
          if ([button isDescendantOfView:rows[row]]) {
            [form scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:row inSection:section]
                       atScrollPosition:UITableViewScrollPositionMiddle animated:NO];
            [form layoutIfNeeded];
          }
        }
      }
      [button sendActionsForControlEvents:UIControlEventTouchUpInside];
    }
    if ([action isEqual:@"color_select"]) {
      UIColorPickerViewController *picker = [panels valueForKey:@"_colorPicker"];
      NSArray *value = request[@"value"];
      UIColor *color = [UIColor colorWithRed:[value[0] doubleValue] green:[value[1] doubleValue]
                                       blue:[value[2] doubleValue] alpha:value.count == 4 ? [value[3] doubleValue] : 1];
      [picker.delegate colorPickerViewController:picker didSelectColor:color
                                   continuously:[request[@"continuous"] boolValue]];
    }
    if ([action isEqual:@"color_close"]) {
      UIColorPickerViewController *picker = [panels valueForKey:@"_colorPicker"];
      [picker.delegate colorPickerViewControllerDidFinish:picker];
    }
    if ([action isEqual:@"toggle"]) {
      UISwitch *toggle = controls[request[@"key"]];
      if ([toggle isKindOfClass:UISwitch.class]) {
        toggle.on = [request[@"value"] boolValue];
        [toggle sendActionsForControlEvents:UIControlEventValueChanged];
      }
    }
    NSArray *rows = [panels valueForKey:@"_rows"];
    UITableView *tree = [panels valueForKey:@"_tree"];
    if ([action isEqual:@"select"]) {
      NSUInteger index = [rows indexOfObjectPassingTest:^BOOL(NSDictionary *row, NSUInteger index, BOOL *stop) {
        return [row[@"id"] isEqual:request[@"id"]];
      }];
      if (index != NSNotFound) {
        [tree.delegate tableView:tree didSelectRowAtIndexPath:[NSIndexPath indexPathForRow:index inSection:0]];
      }
    }
    if ([action isEqual:@"property"]) {
      for (id control in controls.allValues) {
        if (![control isKindOfClass:UITextField.class]) {
          continue;
        }
        NSDictionary *descriptor = [control valueForKey:@"descriptor"];
        NSString *key = [NSString stringWithFormat:@"%@.%@.%@", descriptor[@"owner"], descriptor[@"property"], descriptor[@"index"] ?: @""];
        if ([key isEqual:request[@"key"]]) {
          UITextField *field = control;
          field.text = request[@"value"];
          [field sendActionsForControlEvents:UIControlEventEditingDidEnd];
          break;
        }
      }
    }
    NSMutableDictionary *values = [NSMutableDictionary dictionary];
    for (NSString *key in controls) {
      id control = controls[key];
      if ([control isKindOfClass:UITextField.class]) {
        NSDictionary *descriptor = [control valueForKey:@"descriptor"];
        NSString *semantic = [NSString stringWithFormat:@"%@.%@.%@", descriptor[@"owner"], descriptor[@"property"], descriptor[@"index"] ?: @""];
        values[semantic] = ((UITextField *)control).text ?: @"";
      }
    }
    UIView *left = [panels valueForKey:@"_inspector"];
    UIView *right = [panels valueForKey:@"_hierarchy"];
    NSDictionary *snapshot = @{@"inspector": @(inspector.selected), @"hierarchy": @(hierarchy.selected),
      @"left": rectValues(left.frame), @"right": rectValues(right.frame), @"fields": values,
      @"rows": rows ?: @[], @"error": [[panels valueForKey:@"_status"] text] ?: @"",
      @"tabs": [panels valueForKey:@"_tabs"] ?: @[],
      @"properties": [panels valueForKey:@"_snapshot"][@"inspector"] ?: @{},
      @"colorPicker": @([[panels valueForKey:@"_colorPicker"] isKindOfClass:UIColorPickerViewController.class]),
      @"colorPresented": @(((UIColorPickerViewController *)[panels valueForKey:@"_colorPicker"]).viewIfLoaded.window != nil),
      @"colorWindowLevel": @(((UIColorPickerViewController *)[panels valueForKey:@"_colorPicker"]).viewIfLoaded.window.windowLevel),
      @"colorFrame": rectValues(((UIColorPickerViewController *)[panels valueForKey:@"_colorPicker"]).viewIfLoaded.frame),
      @"rootWindowLevel": @(left.window.windowLevel),
      @"colorAlpha": @([[panels valueForKey:@"_colorPicker"] supportsAlpha]),
      @"tab": [panels valueForKey:@"_selectedTab"] ?: @"",
      @"sections": [panels valueForKey:@"_sectionIdentifiers"] ?: @[],
      @"expanded": [[panels valueForKey:@"_expandedPanels"][[panels valueForKey:@"_selectedTab"] ?: @""] allObjects] ?: @[]};
    NSData *data = [NSJSONSerialization dataWithJSONObject:snapshot options:0 error:nil];
    result = strdup([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String);
  });
  return result;
}

extern "C" char *blender_device_probe_view_gizmo(int action, int item)
{
  __block char *result = nullptr;
  dispatch_sync(dispatch_get_main_queue(), ^{
    UIViewController *controller = blenderController();
    UIView *hotspot = [controller valueForKey:@"_view_gizmo_hotspot"];
    UIView *feedback = [controller valueForKey:@"_view_gizmo_feedback"];
    NSArray<UIButton *> *buttons = [controller valueForKey:@"_view_pie_buttons"];
    UIView *overlay = [controller valueForKey:@"_overlay_view"];
    if (action == 0 || action == 2) {
      BlenderGizmoProbeHover *hover = [BlenderGizmoProbeHover new];
      hover.probeState = action == 0 ? UIGestureRecognizerStateBegan : UIGestureRecognizerStateEnded;
      [controller handleViewGizmoHover:hover];
    }
    else if (action == 1 || action == 3 || action == 5) {
      BlenderGizmoProbeHold *hold = [BlenderGizmoProbeHold new];
      hold.probeState = action == 1 ? UIGestureRecognizerStateBegan :
                        action == 3 ? UIGestureRecognizerStateEnded : UIGestureRecognizerStateChanged;
      hold.probePoint = item >= 0 ? buttons[item].center :
          [hotspot convertPoint:CGPointMake(56, 56) toView:overlay];
      [controller handleViewGizmoHold:hold];
    }
    else if (action == 4) {
      [controller cancelViewGizmoInteraction];
    }
    NSMutableArray *frames = [NSMutableArray array];
    for (UIButton *button in buttons) {
      [frames addObject:@{@"frame": rectValues(button.frame), @"hidden": @(button.hidden),
                         @"selected": @(button.selected), @"title": button.accessibilityLabel}];
    }
    NSInteger holdCount = 0;
    for (UIGestureRecognizer *gesture in hotspot.gestureRecognizers) {
      if ([gesture isKindOfClass:UILongPressGestureRecognizer.class]) {
        UILongPressGestureRecognizer *hold = (id)gesture;
        if (hold.minimumPressDuration == 0.32 &&
            [hold.allowedTouchTypes containsObject:@(UITouchTypeDirect)] &&
            [hold.allowedTouchTypes containsObject:@(UITouchTypePencil)]) holdCount++;
      }
    }
    NSDictionary *state = @{
      @"visible": [controller valueForKey:@"_view_pie_visible"],
      @"feedbackAlpha": @(feedback.alpha), @"feedbackScale": @(feedback.transform.a),
      @"feedbackFrame": rectValues([feedback convertRect:feedback.bounds toView:overlay]),
      @"hotspot": rectValues(hotspot.frame), @"enabled": @(hotspot.userInteractionEnabled),
      @"holds": @(holdCount), @"buttons": frames,
      @"safeArea": rectValues(overlay.safeAreaLayoutGuide.layoutFrame),
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
    result = strndup((const char *)json.bytes, json.length);
  });
  return result;
}

extern "C" char *blender_device_probe_geometry()
{
  __block char *result = nullptr;
  dispatch_sync(dispatch_get_main_queue(), ^{
    UIViewController *controller = blenderController();
    if (!controller) {
      result = strdup("{}");
      return;
    }
    UIWindow *window = controller.view.window;
    NSMutableDictionary *geometry = [@{
      @"screen" : rectValues(window.windowScene.screen.bounds),
      @"window" : rectValues(window.bounds),
      @"viewport" : rectValues([controller.view convertRect:controller.view.bounds toView:window]),
      @"safeArea" : rectValues([controller.view
          convertRect:controller.view.safeAreaLayoutGuide.layoutFrame toView:window]),
      @"viewportClass" : NSStringFromClass(controller.view.class),
      @"sdk" : NSBundle.mainBundle.infoDictionary[@"DTSDKName"] ?: @"",
      @"sceneManifest" : @(NSBundle.mainBundle.infoDictionary[@"UIApplicationSceneManifest"] != nil),
      @"idiom" : @(UIDevice.currentDevice.userInterfaceIdiom),
    } mutableCopy];
    for (NSString *key in @[ @"_top_bar", @"_tool_rail", @"_action_rail", @"_view_gizmo_hotspot" ]) {
      UIView *view = [controller valueForKey:key];
      if (view) {
        geometry[key] = @{
          @"frame" : rectValues([view convertRect:view.bounds toView:window]),
          @"hidden" : @(view.hidden),
        };
      }
    }
    UINavigationItem *item = [controller valueForKey:@"_top_bar_item"];
    NSMutableArray *leading = [NSMutableArray array];
    for (UIBarButtonItemGroup *group in item.leadingItemGroups) {
      if (group.hidden) {
        continue;
      }
      for (UIBarButtonItem *button in group.barButtonItems) {
        [leading addObject:button.identifier ?: @""];
      }
    }
    geometry[@"leadingItems"] = leading;
    geometry[@"pinnedTrailingItem"] = item.pinnedTrailingGroup.barButtonItems.firstObject.identifier ?: @"";
    NSData *data = [NSJSONSerialization dataWithJSONObject:geometry options:0 error:nil];
    result = strdup([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String);
  });
  return result;
}

extern "C" char *blender_device_probe_toolbar_label(int action, int index)
{
  __block char *result = nullptr;
  dispatch_sync(dispatch_get_main_queue(), ^{
    UIViewController *controller = blenderController();
    NSArray<UIButton *> *buttons = [controller valueForKey:@"_toolbar_buttons"];
    UIButton *button = index >= 0 && index < buttons.count ? buttons[index] : nil;
    UIScrollView *scroll = [controller valueForKey:@"_tool_scroll"];
    if (action == 0 || action == 4) {
      [controller setToolRailDockedLeftCenter:YES animated:NO];
      [controller setToolbarExpanded:action == 4 animated:NO];
      [controller.view layoutIfNeeded];
    }
    else if (action == 1) {
      [button sendActionsForControlEvents:UIControlEventTouchUpInside];
    }
    else if (action == 2 || action == 3) {
      BlenderToolbarProbeHover *hover = [BlenderToolbarProbeHover new];
      hover.probeState = action == 2 ? UIGestureRecognizerStateChanged : UIGestureRecognizerStateEnded;
      hover.probePoint = [button convertPoint:CGPointMake(CGRectGetMidX(button.bounds),
                                                        CGRectGetMidY(button.bounds)) toView:scroll];
      [controller handleToolbarHover:hover];
    }
    UILabel *label = [controller valueForKey:@"_toolbar_label"];
    UIView *bubble = [controller valueForKey:@"_toolbar_label_bubble"];
    UIView *rail = [controller valueForKey:@"_tool_rail"];
    NSMutableArray *tools = [NSMutableArray array];
    for (UIButton *b in buttons) {
      [tools addObject:@{@"id": b.accessibilityIdentifier ?: @"", @"name": b.accessibilityLabel ?: @""}];
    }
    NSDictionary *state = @{
      @"tools": tools, @"active": [controller valueForKey:@"_active_tool_identifier"] ?: @"",
      @"expanded": [controller valueForKey:@"_toolbar_expanded"],
      @"docked": [controller valueForKey:@"_tool_rail_docked_left_center"],
      @"transient": [controller valueForKey:@"_toolbar_label_transient"],
      @"label": label.text ?: @"", @"alpha": @(bubble.alpha),
      @"rail": rectValues([rail convertRect:rail.bounds toView:controller.view]),
      @"bubble": rectValues(bubble.frame), @"viewport": rectValues(controller.view.bounds),
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
    result = strndup((const char *)json.bytes, json.length);
  });
  return result;
}

extern "C" char *blender_device_probe_quick_actions(int taps)
{
  __block char *result = nullptr;
  dispatch_sync(dispatch_get_main_queue(), ^{
    UIViewController *controller = blenderController();
    UIButton *toggle = [controller valueForKey:@"_action_toggle"];
    UIScrollView *scroll = [controller valueForKey:@"_action_scroll"];
    UIView *rail = [controller valueForKey:@"_action_rail"];
    for (int i = 0; i < taps; i++) {
      [toggle sendActionsForControlEvents:UIControlEventTouchUpInside];
    }
    NSDictionary *state = @{
      @"collapsed": [controller valueForKey:@"_actions_collapsed"],
      @"title": toggle.configuration.title ?: @"",
      @"icon": @(toggle.configuration.image != nil),
      @"hidden": @(scroll.hidden), @"alpha": @(scroll.alpha),
      @"interactive": @(scroll.userInteractionEnabled),
      @"accessibilityHidden": @(scroll.accessibilityElementsHidden),
      @"count": @([[controller valueForKey:@"_action_buttons"] count]),
      @"toggle": rectValues([toggle convertRect:toggle.bounds toView:controller.view]),
      @"lane": rectValues([rail convertRect:rail.bounds toView:controller.view]),
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
    result = strndup((const char *)json.bytes, json.length);
  });
  return result;
}

extern "C" char *blender_device_probe_mesh_select(int index)
{
  __block char *result = nullptr;
  dispatch_sync(dispatch_get_main_queue(), ^{
    UIViewController *controller = blenderController();
    UISegmentedControl *segments = [controller valueForKey:@"_mesh_select_control"];
    UIBarButtonItemGroup *group = [controller valueForKey:@"_mesh_select_group"];
    UINavigationItem *item = [controller valueForKey:@"_top_bar_item"];
    if (index >= 0 && index < segments.numberOfSegments) {
      UIButton *driver = [UIButton buttonWithType:UIButtonTypeSystem];
      [driver addAction:[segments actionForSegmentAtIndex:index] forControlEvents:UIControlEventTouchUpInside];
      [driver sendActionsForControlEvents:UIControlEventTouchUpInside];
    }
    NSMutableArray *names = [NSMutableArray array];
    for (NSInteger i = 0; i < segments.numberOfSegments; i++) {
      [names addObject:[segments actionForSegmentAtIndex:i].title ?: @""];
    }
    NSDictionary *state = @{
      @"selected": @(segments.selectedSegmentIndex), @"hidden": @(group.hidden),
      @"segments": @(segments.numberOfSegments), @"momentary": @(segments.momentary),
      @"names": names, @"frame": rectValues([segments convertRect:segments.bounds toView:controller.view]),
      @"nextToMode": @([item.leadingItemGroups indexOfObject:group] > 0 &&
          [item.leadingItemGroups indexOfObject:group] != NSNotFound &&
          item.leadingItemGroups[[item.leadingItemGroups indexOfObject:group] - 1].barButtonItems.firstObject ==
          [controller valueForKey:@"_mode_button"]),
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
    result = strndup((const char *)json.bytes, json.length);
  });
  return result;
}

extern "C" char *blender_device_probe_snapshot()
{
  __block char *result;
  dispatch_sync(dispatch_get_main_queue(), ^{
    auto copy = (char *(*)())dlsym(RTLD_DEFAULT, "WM_apple_ios_ui_toolbar_copy_json");
    char *json = copy();
    NSData *data = [NSData dataWithBytes:json length:strlen(json)];
    free(json);
    NSMutableDictionary *snapshot = [NSJSONSerialization
        JSONObjectWithData:data
                   options:NSJSONReadingMutableContainers
                     error:nil];
    UIViewController *controller = blenderController();
    NSMutableArray *axes = [NSMutableArray array];
    for (UIButton *button in [controller valueForKey:@"_action_buttons"]) {
      if ([@[ @"X", @"Y", @"Z" ] containsObject:button.accessibilityHint ?: @""]) {
        [axes addObject:@{@"key" : button.accessibilityHint, @"selected" : @(button.selected)}];
      }
    }
    snapshot[@"nativeAxes"] = axes;
    NSMutableArray *menus = [NSMutableArray array];
    UIBarButtonItem *button = [controller valueForKey:@"_actions_button"];
    for (UIMenu *section in button.menu.children) {
      for (UIMenuElement *menu in section.children) {
        [menus addObject:menu.title];
      }
    }
    snapshot[@"nativeMenus"] = menus;
    NSMutableArray *tools = [NSMutableArray array];
    for (UIButton *tool in [controller valueForKey:@"_toolbar_buttons"]) {
      [tools addObject:@{@"id": tool.accessibilityIdentifier ?: @"",
                         @"group": tool.menu.identifier ?: @"",
                         @"title": tool.accessibilityLabel ?: @""}];
    }
    snapshot[@"nativeTools"] = tools;
    UIViewController *presented = blenderPresentedController();
    snapshot[@"presented"] = presented ? NSStringFromClass(presented.class) : @"";
    data = [NSJSONSerialization dataWithJSONObject:snapshot options:0 error:nil];
    result = strdup([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String);
  });
  return result;
}

static UIAction *toolbarAction(UIMenu *menu, NSString *identifier)
{
  for (UIMenuElement *element in menu.children) {
    if ([element isKindOfClass:UIAction.class] &&
        [((UIAction *)element).identifier isEqualToString:identifier])
    {
      return (UIAction *)element;
    }
    if ([element isKindOfClass:UIMenu.class]) {
      if (UIAction *action = toolbarAction((UIMenu *)element, identifier)) {
        return action;
      }
    }
  }
  return nil;
}

extern "C" bool blender_device_probe_tool_pick(const char *identifier)
{
  NSString *name = [NSString stringWithUTF8String:identifier];
  __block BOOL picked = NO;
  dispatch_sync(dispatch_get_main_queue(), ^{
    UIViewController *controller = blenderController();
    [controller setToolbarExpanded:YES animated:NO];
    for (UIButton *button in [controller valueForKey:@"_toolbar_buttons"]) {
      UIAction *action = toolbarAction(button.menu, name);
      if (action) {
        UIButton *driver = [UIButton buttonWithType:UIButtonTypeSystem];
        [driver addAction:action forControlEvents:UIControlEventTouchUpInside];
        [driver sendActionsForControlEvents:UIControlEventTouchUpInside];
        picked = YES;
        break;
      }
    }
  });
  return picked;
}

extern "C" void blender_device_probe_tool_menu(const char *identifier)
{
  NSString *name = [NSString stringWithUTF8String:identifier];
  dispatch_async(dispatch_get_main_queue(), ^{
    UIViewController *controller = blenderController();
    [controller setToolbarExpanded:YES animated:NO];
    [controller.view layoutIfNeeded];
    for (UIButton *button in [controller valueForKey:@"_toolbar_buttons"]) {
      if (toolbarAction(button.menu, name)) {
        UIScrollView *scroll = [controller valueForKey:@"_tool_scroll"];
        [scroll scrollRectToVisible:[button convertRect:button.bounds toView:scroll] animated:NO];
        /* Programmatic presentation of the same native menu for screenshot verification. */
        button.showsMenuAsPrimaryAction = YES;
        [button performPrimaryAction];
        button.showsMenuAsPrimaryAction = NO;
        break;
      }
    }
  });
}

static UIMenu *nativeSubmenu(UIMenu *menu, NSString *title)
{
  if ([menu.title isEqualToString:title]) {
    return menu;
  }
  for (UIMenuElement *element in menu.children) {
    if ([element isKindOfClass:UIMenu.class]) {
      if (UIMenu *found = nativeSubmenu((UIMenu *)element, title)) {
        return found;
      }
    }
  }
  return nil;
}

extern "C" void blender_device_probe_add_menu(int appearance)
{
  dispatch_async(dispatch_get_main_queue(), ^{
    UIViewController *controller = blenderController();
    controller.view.window.overrideUserInterfaceStyle = UIUserInterfaceStyle(appearance);
    UIBarButtonItem *actions = [controller valueForKey:@"_actions_button"];
    UIMenu *menu = nativeSubmenu(actions.menu, @"Add");
    if (menu) {
      UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
      button.frame = CGRectMake(200, 100, 100, 44);
      [button setTitle:@"Add" forState:UIControlStateNormal];
      button.menu = menu;
      button.showsMenuAsPrimaryAction = YES;
      [controller.view addSubview:button];
      [button performPrimaryAction];
    }
  });
}

extern "C" bool blender_device_probe_axis(const char *axis)
{
  NSString *name = [NSString stringWithUTF8String:axis];
  __block BOOL found = NO;
  dispatch_sync(dispatch_get_main_queue(), ^{
    for (UIButton *button in [blenderController() valueForKey:@"_action_buttons"]) {
      if ([button.accessibilityHint isEqual:name]) {
        [button sendActionsForControlEvents:UIControlEventTouchUpInside];
        found = YES;
        break;
      }
    }
  });
  return found;
}

extern "C" bool blender_device_probe_cancel_picker()
{
  __block BOOL cancelled = NO;
  dispatch_sync(dispatch_get_main_queue(), ^{
    UIViewController *presented = blenderPresentedController();
    if ([presented isKindOfClass:UIDocumentPickerViewController.class]) {
      UIDocumentPickerViewController *picker = (UIDocumentPickerViewController *)presented;
      [picker.delegate documentPickerWasCancelled:picker];
      [picker dismissViewControllerAnimated:NO completion:nil];
      cancelled = YES;
    }
  });
  return cancelled;
}

extern "C" bool blender_device_probe_begin()
{
  BOOL engineOnMain = NSThread.isMainThread;
  if (engineOnMain) {
    return false;
  }
  __block BOOL ready = NO;
  dispatch_sync(dispatch_get_main_queue(), ^{
    probe = [BlenderDeviceProbe new];
    probe.engineOnMain = engineOnMain;
    probe.controller = blenderController();
    if (!probe.controller) {
      return;
    }
    probe.scroll = [probe.controller valueForKey:@"_tool_scroll"];
    probe.start = CACurrentMediaTime();
    probe.link = [CADisplayLink displayLinkWithTarget:probe selector:@selector(tick:)];
    [probe.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    ready = YES;
  });
  return ready;
}

extern "C" char *blender_device_probe_end()
{
  __block char *result;
  dispatch_sync(dispatch_get_main_queue(), ^{
    [probe.link invalidate];
    NSDictionary *metrics = @{
      @"ticks" : @(probe.ticks),
      @"maximum_gap_ms" : @(probe.maximumGap * 1000),
      @"queued_commands" : @(probe.commands),
      @"snapshot_reads" : @(probe.snapshots),
      @"toolbar_expanded" : @(probe.expanded),
      @"scroll_distance" : @(probe.scrollDistance),
      @"engine_on_main" : @(probe.engineOnMain),
    };
    NSData *data = [NSJSONSerialization dataWithJSONObject:metrics options:0 error:nil];
    result = strdup([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String);
    [probe.controller setToolbarExpanded:NO animated:YES];
    probe = nil;
  });
  return result;
}

extern "C" char *blender_device_probe_menu_result(uint64_t identifier)
{
  auto take = (char *(*)(uint64_t))dlsym(RTLD_DEFAULT, "WM_apple_ios_ui_menu_take_result");
  __block char *result;
  dispatch_sync(dispatch_get_main_queue(), ^{
    result = take(identifier);
  });
  return result;
}

static char dialog_handler_key;
@interface UIAlertAction (BlenderDialogTest)
+ (instancetype)probeActionWithTitle:(NSString *)title
                              style:(UIAlertActionStyle)style
                            handler:(void (^)(UIAlertAction *))handler;
@end
@implementation UIAlertAction (BlenderDialogTest)
+ (instancetype)probeActionWithTitle:(NSString *)title
                              style:(UIAlertActionStyle)style
                            handler:(void (^)(UIAlertAction *))handler
{
  UIAlertAction *action = [self probeActionWithTitle:title style:style handler:handler];
  objc_setAssociatedObject(action, &dialog_handler_key, handler, OBJC_ASSOCIATION_COPY_NONATOMIC);
  return action;
}
@end

extern "C" void blender_device_probe_dialog_start()
{
  dispatch_sync(dispatch_get_main_queue(), ^{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      method_exchangeImplementations(
          class_getClassMethod(UIAlertAction.class, @selector(actionWithTitle:style:handler:)),
          class_getClassMethod(UIAlertAction.class, @selector(probeActionWithTitle:style:handler:)));
    });
  });
}

static void dialogViews(UIView *view, NSMutableArray<UIView *> *views)
{
  if (!view) {
    return;
  }
  [views addObject:view];
  for (UIView *child in view.subviews) {
    dialogViews(child, views);
  }
}

extern "C" char *blender_device_probe_dialog_snapshot()
{
  __block char *result;
  dispatch_sync(dispatch_get_main_queue(), ^{
    UIViewController *controller = blenderPresentedController();
    NSMutableDictionary *state = [NSMutableDictionary dictionary];
    state[@"class"] = controller ? NSStringFromClass(controller.class) : @"";
    if ([controller isKindOfClass:UIAlertController.class]) {
      UIAlertController *alert = (UIAlertController *)controller;
      state[@"title"] = alert.title ?: @"";
      state[@"message"] = alert.message ?: @"";
      NSMutableArray *actions = [NSMutableArray array];
      for (UIAlertAction *action in alert.actions) {
        [actions addObject:@{@"title": action.title ?: @"", @"style": @(action.style)}];
      }
      state[@"actions"] = actions;
    }
    NSMutableArray *views = [NSMutableArray array];
    dialogViews(controller.view, views);
    NSMutableArray *toggles = [NSMutableArray array];
    for (UIView *view in views) {
      if ([view isKindOfClass:UISwitch.class]) {
        [toggles addObject:@{@"title": view.accessibilityLabel ?: @"",
                            @"selected": @(((UISwitch *)view).on)}];
      }
    }
    state[@"toggles"] = toggles;
    NSData *data = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
    result = strdup([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String);
  });
  return result;
}

extern "C" bool blender_device_probe_dialog_choose(const char *title)
{
  NSString *name = [NSString stringWithUTF8String:title];
  __block BOOL found = NO;
  dispatch_sync(dispatch_get_main_queue(), ^{
    UIViewController *controller = blenderPresentedController();
    if ([controller isKindOfClass:UIAlertController.class]) {
      for (UIAlertAction *action in ((UIAlertController *)controller).actions) {
        if ([action.title isEqual:name] && action.enabled) {
          void (^handler)(UIAlertAction *) = objc_getAssociatedObject(action, &dialog_handler_key);
          if (handler) {
            handler(action);
            found = YES;
          }
          break;
        }
      }
    }
    else {
      NSMutableArray *views = [NSMutableArray array];
      dialogViews(controller.view, views);
      for (UIView *view in views) {
        if ([view isKindOfClass:UIButton.class] &&
            [((UIButton *)view).configuration.title isEqual:name]) {
          [(UIButton *)view sendActionsForControlEvents:UIControlEventTouchUpInside];
          found = YES;
          break;
        }
      }
    }
  });
  return found;
}
