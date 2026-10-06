/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#import <UIKit/UIKit.h>
#import "GHOST_IOSRedoStrip.hh"
@interface GHOSTIOSRedoStrip (Testing)
- (void)commitField:(UITextField *)field;
- (void)dismiss:(id)sender;
- (void)expand:(id)sender;
@end

static NSMutableArray *requests;
extern "C" void WM_apple_ios_ui_setting_enqueue(const char *setting, const char *value)
{
  NSCAssert(strcmp(setting, "native_redo_edit") == 0, @"Dedicated operator channel");
  NSData *data = [[NSString stringWithUTF8String:value] dataUsingEncoding:NSUTF8StringEncoding];
  [requests addObject:[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]];
}

int main()
{
  @autoreleasepool {
    [UIApplication sharedApplication];
    [UIView setAnimationsEnabled:NO];
    UIWindow *window = [[[UIWindow alloc] initWithFrame:CGRectMake(0, 0, 1194, 834)] autorelease];
    UIViewController *controller = [[[UIViewController alloc] init] autorelease];
    window.rootViewController = controller;
    [window makeKeyAndVisible];
    requests = [NSMutableArray array];
    GHOSTIOSRedoStrip *strip = [[[GHOSTIOSRedoStrip alloc] initWithOverlay:controller.view
        icon:^UIImage *(NSString *name) { return [UIImage systemImageNamed:@"cube"]; }] autorelease];
    NSDictionary *number = @{@"key": @"size", @"property": @"size", @"type": @"FLOAT", @"label": @"Size",
        @"value": @2, @"min": @0, @"max": @100, @"editable": @YES, @"dragStep": @0.01};
    NSDictionary *toggle = @{@"key": @"uv", @"type": @"BOOLEAN", @"label": @"Generate UVs", @"value": @YES, @"editable": @YES};
    NSDictionary *menu = @{@"key": @"mode", @"type": @"ENUM", @"label": @"Fill", @"value": @"A", @"editable": @YES,
        @"options": @[@{@"value": @"A", @"label": @"Nothing", @"checked": @YES, @"icon": @"MESH_CUBE"},
                      @{@"value": @"B", @"label": @"Triangle Fan", @"checked": @NO, @"icon": @"MESH_CIRCLE"}]};
    NSDictionary *slider = @{@"key": @"factor", @"type": @"FLOAT", @"label": @"Factor", @"value": @0.5,
        @"editable": @YES, @"slider": @{@"min": @0, @"max": @1}};
    NSMutableDictionary *model = [NSMutableDictionary dictionaryWithDictionary:
        @{@"id": @"1", @"title": @"Add Cube", @"ack": @0, @"properties": @[number, toggle, menu, slider]}];
    [strip updateSnapshot:nil];
    NSCAssert(strip.view.hidden, @"No strip before an adjustable operation");
    [strip updateSnapshot:model];
    NSCAssert(!strip.view.hidden, @"Operation opens strip");
    NSDictionary *controls = [strip valueForKey:@"_controls"];
    UITextField *field = controls[@"size"];
    NSCAssert([field isKindOfClass:UITextField.class] && [controls[@"uv"] isKindOfClass:UISwitch.class] &&
              [controls[@"factor"] isKindOfClass:UISlider.class], @"Real native controls");
    UIButton *enumButton = controls[@"mode"];
    NSCAssert(enumButton.menu.children.count == 2 && enumButton.configuration.image, @"Native enum with Blender icons");
    for (NSNumber *width in @[@1194, @744, @393]) {
      CGRect rect = CGRectMake(86, 80, width.doubleValue - 100, 480);
      [strip layoutInRect:rect];
      [strip.view layoutIfNeeded];
      UIScrollView *scroll = [strip valueForKey:@"_scroll"];
      UIButton *done = [strip valueForKey:@"_done"];
      NSCAssert(CGRectContainsRect(rect, strip.view.frame) && strip.view.frame.size.height == 60,
                @"Thin strip fits iPad and compact phone bounds");
      NSCAssert(CGRectGetMaxX(scroll.frame) <= CGRectGetMinX(done.frame) && CGRectGetMaxX(done.frame) <= strip.view.bounds.size.width,
                @"Fixed checkmark never overlaps scrolling controls");
    }
    field.text = [NSNumberFormatter localizedStringFromNumber:@3.5 numberStyle:NSNumberFormatterDecimalStyle];
    NSCAssert([[field actionsForTarget:strip forControlEvent:UIControlEventEditingDidEnd] containsObject:@"commitField:"], @"Field commits when editing ends");
    [strip commitField:field];
    NSCAssert([requests.lastObject[@"value"] doubleValue] == 3.5 && [requests.lastObject[@"id"] isEqual:@"1"], @"Locale-aware edit carries operation identity: %@ / %@", requests, field.text);
    NSString *pendingText = [[field.text copy] autorelease];
    [strip updateSnapshot:model];
    NSCAssert([field.text isEqual:pendingText] && controls[@"size"] == field, @"Stale snapshots preserve pending edits and control identity");
    UISwitch *uv = controls[@"uv"];
    uv.on = NO;
    [uv sendActionsForControlEvents:UIControlEventValueChanged];
    NSCAssert([requests.lastObject[@"key"] isEqual:@"uv"] && ![requests.lastObject[@"value"] boolValue], @"Native switch queues real property value");
    UISlider *factor = controls[@"factor"];
    factor.value = 0.75;
    [factor sendActionsForControlEvents:UIControlEventValueChanged];
    NSCAssert([requests.lastObject[@"key"] isEqual:@"factor"] && [requests.lastObject[@"value"] doubleValue] == 0.75, @"Native slider queues adjustment");
    NSUInteger count = requests.count;
    field.text = @"nan";
    [strip commitField:field];
    NSCAssert(requests.count == count, @"Invalid numbers never reach Blender");
    UIButton *done = [strip valueForKey:@"_done"];
    NSCAssert([[done actionsForTarget:strip forControlEvent:UIControlEventTouchUpInside] containsObject:@"dismiss:"], @"Checkmark routes to dismissal");
    [strip dismiss:done];
    NSCAssert(strip.view.hidden && !strip.collapsedButton.hidden && requests.count == count,
              @"Checkmark collapses locally without dismissing the Blender operation");
    NSCAssert([strip.collapsedButton.configuration.title isEqual:@"Add Cube"] &&
              strip.collapsedWidth >= 72 && strip.collapsedWidth <= 136, @"Compact pill names the operator");
    [strip updateSnapshot:model];
    NSCAssert(strip.view.hidden && !strip.collapsedButton.hidden, @"Snapshots preserve collapsed state");
    [strip updateSnapshot:nil];
    NSCAssert(strip.collapsedButton.hidden && strip.collapsedWidth == 0, @"Unavailable operation hides its pill");
    [strip updateSnapshot:model];
    NSCAssert(strip.view.hidden && !strip.collapsedButton.hidden, @"Temporary unavailability preserves collapse preference");
    [strip expand:strip.collapsedButton];
    NSCAssert(!strip.view.hidden && strip.collapsedButton.hidden && requests.count == count,
              @"Pill restores controls without re-executing the operator");
    [strip dismiss:done];
    [field retain];
    model[@"id"] = @"2";
    model[@"operator"] = @"MESH_OT_extrude_region_move";
    model[@"title"] = @"Extrude Region and Move";
    [strip updateSnapshot:model];
    NSCAssert(!strip.view.hidden, @"A new operation reopens the strip");
    NSCAssert(strip.collapsedButton.hidden && [strip.collapsedButton.configuration.title isEqual:@"Extrude"],
              @"Long macro name uses a concise pill title");
    count = requests.count;
    field.text = @"12";
    [strip commitField:field];
    NSCAssert(requests.count == count, @"Retired controls cannot edit or dismiss a new operation");
    [field release];
    [strip updateSnapshot:nil];
    NSCAssert(strip.view.hidden, @"Unavailable operation hides strip");
    NSLog(@"PASS: native redo strip controls, asynchronous edits, dismissal, stale identity, and compact layout");
  }
}
