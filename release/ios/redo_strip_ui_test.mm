/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#import <UIKit/UIKit.h>
#import "GHOST_IOSRedoStrip.hh"
@interface GHOSTIOSRedoStrip (Testing)
- (void)commitField:(UITextField *)field;
- (void)dismiss:(id)sender;
- (void)expand:(id)sender;
- (void)scrubNumber:(UIGestureRecognizer *)gesture;
- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)gesture;
- (void)beginSlider:(UISlider *)slider;
- (void)changeSlider:(UISlider *)slider;
- (void)finishSlider:(UISlider *)slider;
- (void)cancelSlider:(UISlider *)slider;
@end

@interface RedoTestPress : UIPanGestureRecognizer
@property(nonatomic) UIGestureRecognizerState testState;
@property(nonatomic) CGPoint testPoint;
@property(nonatomic) CGPoint testVelocity;
@end
@implementation RedoTestPress
- (UIGestureRecognizerState)state { return _testState; }
- (CGPoint)locationInView:(UIView *)view { return _testPoint; }
- (CGPoint)velocityInView:(UIView *)view { return _testVelocity; }
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
    [controller.view addSubview:strip.collapsedButton];
    strip.collapsedButton.frame = CGRectMake(520, 760, 136, 44);
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
    NSCAssert(strip.view.hidden && !strip.collapsedButton.hidden && strip.collapsedWidth > 0,
              @"New operations start as a collapsed pill");
    NSCAssert([[strip.collapsedButton actionsForTarget:strip forControlEvent:UIControlEventTouchUpInside]
                  containsObject:@"expand:"], @"Pill is wired to expansion");
    [strip expand:strip.collapsedButton];
    NSCAssert(!strip.view.hidden && strip.collapsedButton.hidden, @"Tapping the pill opens the controls");
    [strip updateSnapshot:model];
    NSCAssert(!strip.view.hidden, @"Same-operation updates preserve explicit expansion");
    [strip updateSnapshot:nil];
    [strip updateSnapshot:model];
    NSCAssert(!strip.view.hidden, @"Temporary unavailability preserves explicit expansion");
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
      NSCAssert(CGRectGetMinX(done.frame) >= 0 && CGRectGetMaxX(done.frame) <= CGRectGetMinX(scroll.frame) &&
                CGRectGetMaxX(scroll.frame) <= strip.view.bounds.size.width,
                @"Leading close control never overlaps scrolling controls");
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
    [strip changeSlider:factor];
    NSCAssert([requests.lastObject[@"key"] isEqual:@"factor"] && [requests.lastObject[@"value"] doubleValue] == 0.75, @"Native slider queues adjustment");
    [strip beginSlider:factor];
    factor.value = 0.6;
    [strip changeSlider:factor];
    NSCAssert([requests.lastObject[@"phase"] isEqual:@"update"], @"Slider previews while held");
    [strip cancelSlider:factor];
    NSCAssert([requests.lastObject[@"phase"] isEqual:@"cancel"] && [requests.lastObject[@"value"] doubleValue] == 0.75,
              @"Slider cancellation restores the original operator value");
    UIPanGestureRecognizer *installed = nil;
    for (UIGestureRecognizer *gesture in field.gestureRecognizers) {
      if ([gesture.name isEqual:@"blender.number.scrub"]) { installed = (UIPanGestureRecognizer *)gesture; }
    }
    NSCAssert([installed isKindOfClass:UIPanGestureRecognizer.class] && installed.delegate == (id)strip,
              @"Redo numbers scrub from an immediate drag, not a long press");
    RedoTestPress *press = [[[RedoTestPress alloc] init] autorelease];
    press.name = @"blender.number.scrub";
    [field addGestureRecognizer:press];
    press.testVelocity = CGPointMake(300, 40);
    NSCAssert([strip gestureRecognizerShouldBegin:press], @"Sideways drags on a number scrub it");
    press.testVelocity = CGPointMake(40, 300);
    NSCAssert(![strip gestureRecognizerShouldBegin:press], @"Other drags fall through to the strip");
    press.testState = UIGestureRecognizerStateBegan;
    [strip scrubNumber:press];
    press.testPoint = CGPointMake(50, 0);
    press.testState = UIGestureRecognizerStateChanged;
    [strip scrubNumber:press];
    NSCAssert([requests.lastObject[@"phase"] isEqual:@"update"] && [requests.lastObject[@"value"] doubleValue] == 4,
              @"Number scrub updates the operator before release");
    press.testPoint = CGPointMake(60, 0);
    press.testState = UIGestureRecognizerStateEnded;
    [strip scrubNumber:press];
    NSCAssert([requests.lastObject[@"phase"] isEqual:@"end"] && [requests.lastObject[@"value"] doubleValue] == 4.1,
              @"Final precise value is sent even inside the preview throttle interval");
    NSCAssert(field.borderStyle == UITextBorderStyleRoundedRect, @"Redo numbers use the same native field style");
    press.testPoint = CGPointZero;
    press.testState = UIGestureRecognizerStateBegan;
    [strip scrubNumber:press];
    press.testState = UIGestureRecognizerStateChanged;
    press.testPoint = CGPointMake(20, 80);
    [strip scrubNumber:press];
    NSNumberFormatter *formatter = [[[NSNumberFormatter alloc] init] autorelease];
    formatter.numberStyle = NSNumberFormatterDecimalStyle;
    NSCAssert(fabs([[formatter numberFromString:field.text] doubleValue] - 4.2) < 1e-9,
              @"Downward redo scrubbing provides finer adjustments");
    press.testPoint = CGPointMake(20, -80);
    [strip scrubNumber:press];
    NSCAssert(fabs([[formatter numberFromString:field.text] doubleValue] - 4.2) < 1e-9,
              @"Changing redo sensitivity cannot jump the value");
    press.testPoint = CGPointMake(30, -80);
    press.testState = UIGestureRecognizerStateEnded;
    [strip scrubNumber:press];
    NSCAssert(fabs([requests.lastObject[@"value"] doubleValue] - 4.4) < 1e-9,
              @"Upward redo scrubbing provides coarser adjustments and an exact final value");
    NSUInteger count = requests.count;
    field.text = @"nan";
    [strip commitField:field];
    NSCAssert(requests.count == count, @"Invalid numbers never reach Blender");
    UIButton *done = [strip valueForKey:@"_done"];
    NSCAssert([[done actionsForTarget:strip forControlEvent:UIControlEventTouchUpInside] containsObject:@"dismiss:"], @"Close control routes to dismissal");
    [strip dismiss:done];
    NSCAssert(strip.view.hidden && !strip.collapsedButton.hidden && requests.count == count,
              @"Close control collapses locally without dismissing the Blender operation");
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
    NSCAssert(strip.view.hidden && !strip.collapsedButton.hidden, @"A new operation stays collapsed");
    NSCAssert([strip.collapsedButton.configuration.title isEqual:@"Extrude"],
              @"Long macro name uses a concise pill title");
    count = requests.count;
    field.text = @"12";
    [strip commitField:field];
    NSCAssert(requests.count == count, @"Retired controls cannot edit or dismiss a new operation");
    [field release];
    [strip expand:strip.collapsedButton];
    model[@"id"] = @"3";
    [strip updateSnapshot:model];
    NSCAssert(strip.view.hidden && !strip.collapsedButton.hidden,
              @"A new operation defaults to a pill even when the previous one was expanded");
    model[@"id"] = @"2";
    [strip updateSnapshot:model];
    NSCAssert(strip.view.hidden && !strip.collapsedButton.hidden,
              @"Returning to an older operator cannot restore an obsolete expanded state");
    [strip updateSnapshot:nil];
    NSCAssert(strip.view.hidden, @"Unavailable operation hides strip");
    NSLog(@"PASS: native redo strip controls, asynchronous edits, dismissal, stale identity, and compact layout");
  }
}
