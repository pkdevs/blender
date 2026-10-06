/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSTimeline.hh"

static NSMutableArray<NSArray<NSString *> *> *sent;
extern "C" void WM_apple_ios_ui_setting_enqueue(const char *key, const char *value)
{
  [sent addObject:@[[NSString stringWithUTF8String:key], [NSString stringWithUTF8String:value]]];
}

static UIView *find(UIView *root, NSString *identifier)
{
  if ([root.accessibilityIdentifier isEqual:identifier]) {
    return root;
  }
  for (UIView *child in root.subviews) {
    if (UIView *match = find(child, identifier)) {
      return match;
    }
  }
  return nil;
}

int main()
{
  @autoreleasepool {
    [UIApplication sharedApplication];
    [UIView setAnimationsEnabled:NO];
    sent = [NSMutableArray array];
    UIWindow *window = [[[UIWindow alloc] initWithFrame:CGRectMake(0, 0, 1194, 834)] autorelease];
    UIViewController *controller = [[[UIViewController alloc] init] autorelease];
    window.rootViewController = controller;
    [window makeKeyAndVisible];
    GHOSTIOSTimeline *timeline = [[[GHOSTIOSTimeline alloc] initWithOverlay:controller.view] autorelease];
    __block int relayouts = 0;
    timeline.visibilityDidChange = ^{ relayouts++; };
    NSCAssert([timeline.button.accessibilityIdentifier isEqual:@"blender.timeline"] &&
              [timeline.button.title isEqual:@"Timeline"], @"Timeline has an accessible toolbar toggle");
    NSCAssert(timeline.view.hidden && !timeline.visible, @"Timeline starts hidden");

    [timeline.button.target performSelector:timeline.button.action withObject:timeline.button];
    NSCAssert(timeline.visible && timeline.button.selected && !timeline.view.hidden && relayouts == 1,
              @"Toggle shows the timeline and asks the host to relayout");

    const CGRect lane = CGRectMake(100, 70, 900, 680);
    [timeline layoutInRect:lane];
    [timeline.view layoutIfNeeded];
    NSCAssert(fabs(CGRectGetMaxY(timeline.view.frame) - CGRectGetMaxY(lane)) < 0.5 &&
              fabs(CGRectGetMidX(timeline.view.frame) - CGRectGetMidX(lane)) < 0.5 &&
              timeline.view.frame.size.height == timeline.height && timeline.view.frame.size.width <= 760,
              @"Timeline floats centered along the bottom of its lane");

    [timeline updateSnapshot:@{@"frame": @42, @"start": @10, @"end": @90, @"playing": @NO, @"preview": @NO}];
    UISlider *slider = (UISlider *)find(timeline.view, @"blender.timeline.scrubber");
    UILabel *frame = (UILabel *)find(timeline.view, @"blender.timeline.frame");
    UIButton *play = (UIButton *)find(timeline.view, @"blender.timeline.play");
    NSCAssert(slider.minimumValue == 10 && slider.maximumValue == 90 && slider.value == 42 &&
              [frame.text isEqual:@"42"] && [play.accessibilityLabel isEqual:@"Play"],
              @"Snapshot drives the scrubber range, frame readout and play state");

    [timeline updateSnapshot:@{@"frame": @43, @"start": @10, @"end": @90, @"playing": @YES, @"preview": @NO}];
    NSCAssert([play.accessibilityLabel isEqual:@"Pause"], @"Playing shows a pause button");

    [sent removeAllObjects];
    [play sendActionsForControlEvents:UIControlEventTouchUpInside];
    [(UIButton *)find(timeline.view, @"blender.timeline.next_key") sendActionsForControlEvents:UIControlEventTouchUpInside];
    NSCAssert([sent isEqual:(@[@[@"timeline", @"play"], @[@"timeline", @"next_key"]])],
              @"Transport buttons send engine commands");

    [sent removeAllObjects];
    [slider sendActionsForControlEvents:UIControlEventTouchDown];
    slider.value = 60.4f;
    [slider sendActionsForControlEvents:UIControlEventValueChanged];
    slider.value = 60.2f;
    [slider sendActionsForControlEvents:UIControlEventValueChanged];
    [timeline updateSnapshot:@{@"frame": @44, @"start": @10, @"end": @90, @"playing": @NO, @"preview": @NO}];
    NSCAssert([frame.text isEqual:@"60"], @"Blender snapshots do not move the thumb under the finger");
    slider.value = 61.0f;
    [slider sendActionsForControlEvents:UIControlEventTouchUpInside];
    NSCAssert([sent isEqual:(@[@[@"timeline_frame", @"60"], @[@"timeline_frame", @"61"]])],
              @"Scrubbing sends each whole frame once");

    [timeline layoutInRect:CGRectMake(12, 70, 360, 600)];
    NSCAssert(find(timeline.view, @"blender.timeline.start").hidden && !play.hidden,
              @"Compact widths keep play and the scrubber");

    [timeline toggle:nil];
    [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    NSCAssert(!timeline.visible && !timeline.button.selected && timeline.view.hidden && relayouts == 2,
              @"Toggle hides the timeline");
    NSLog(@"PASS: floating timeline");
  }
  return 0;
}
