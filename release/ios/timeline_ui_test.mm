/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import "GHOST_IOSTimeline.hh"

static NSMutableArray<NSArray<NSString *> *> *sent;
extern "C" void WM_apple_ios_ui_setting_enqueue(const char *key, const char *value)
{
  [sent addObject:@[[NSString stringWithUTF8String:key], [NSString stringWithUTF8String:value]]];
}

@interface TestPan : UIPanGestureRecognizer
@property(nonatomic) UIGestureRecognizerState testState;
@property(nonatomic) CGPoint testPoint, testTranslation;
@end
@implementation TestPan
- (UIGestureRecognizerState)state { return _testState; }
- (CGPoint)locationInView:(UIView *)view { return _testPoint; }
- (CGPoint)translationInView:(UIView *)view { return _testTranslation; }
@end
@interface TestTap : UITapGestureRecognizer
@property(nonatomic) CGPoint testPoint;
@end
@implementation TestTap
- (CGPoint)locationInView:(UIView *)view { return _testPoint; }
@end

@interface NSObject (ScrubberTesting)
- (CGFloat)xForFrame:(CGFloat)frame;
- (CGFloat)keysY;
@end

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
              timeline.view.frame.size.height == timeline.height &&
              fabs(timeline.view.frame.size.width - lane.size.width) < 0.5,
              @"Timeline spans the bottom of its lane");

    [timeline updateSnapshot:@{@"frame": @42, @"start": @10, @"end": @90, @"playing": @NO, @"preview": @NO}];
    [timeline.view layoutIfNeeded];
    UIView *scrubber = find(timeline.view, @"blender.timeline.scrubber");
    UILabel *frame = (UILabel *)find(timeline.view, @"blender.timeline.frame");
    UIView *knob = frame.superview.superview;
    UIButton *play = (UIButton *)find(timeline.view, @"blender.timeline.play");
    CGFloat (^xFor)(NSInteger) = ^CGFloat(NSInteger f) { return [(id)scrubber xForFrame:f]; };
    const CGFloat trackY = scrubber.bounds.size.height - 28;
    NSCAssert([frame.text isEqual:@"42"] && [scrubber.accessibilityValue isEqual:@"42"] &&
              (scrubber.accessibilityTraits & UIAccessibilityTraitAdjustable) &&
              [play.accessibilityLabel isEqual:@"Play"],
              @"Snapshot drives the frame shown in the knob and the play state");
    NSCAssert([knob isKindOfClass:UIVisualEffectView.class] && fabs(knob.bounds.size.height - 32) < 0.5 &&
              knob.bounds.size.width >= 46 &&
              fabs(knob.center.x - xFor(42)) < 0.5 && fabs(knob.center.y - trackY) < 0.5,
              @"A round glass knob sits on the track at the current frame");

    [timeline updateSnapshot:@{@"frame": @43, @"start": @10, @"end": @90, @"playing": @YES, @"preview": @NO}];
    NSCAssert([play.accessibilityLabel isEqual:@"Pause"], @"Playing shows a pause button");

    [sent removeAllObjects];
    [play sendActionsForControlEvents:UIControlEventTouchUpInside];
    [(UIButton *)find(timeline.view, @"blender.timeline.next_key") sendActionsForControlEvents:UIControlEventTouchUpInside];
    NSCAssert([sent isEqual:(@[@[@"timeline", @"play"], @[@"timeline", @"next_key"]])],
              @"Transport buttons send engine commands");

    [sent removeAllObjects];
    TestPan *pan = [[[TestPan alloc] init] autorelease];
    pan.testState = UIGestureRecognizerStateBegan;
    pan.testPoint = CGPointMake(xFor(60), trackY);
    [scrubber performSelector:@selector(pan:) withObject:pan];
    pan.testState = UIGestureRecognizerStateChanged;
    [scrubber performSelector:@selector(pan:) withObject:pan];
    NSCAssert(knob.userInteractionEnabled && knob.transform.a > 1.1 && [frame.textColor isEqual:UIColor.labelColor],
              @"Held, the knob swells into clear glass like a system slider thumb");
    NSCAssert(knob.bounds.size.height * knob.transform.a <= 50,
              @"The swollen knob still fits the band, so the panel never clips it");
    [timeline updateSnapshot:@{@"frame": @44, @"start": @10, @"end": @90, @"playing": @NO, @"preview": @NO}];
    NSCAssert([frame.text isEqual:@"60"] && fabs(knob.center.x - xFor(60)) < 0.5,
              @"Blender snapshots do not move the knob under the finger");
    pan.testPoint = CGPointMake(xFor(61), trackY);
    [scrubber performSelector:@selector(pan:) withObject:pan];
    pan.testState = UIGestureRecognizerStateEnded;
    [scrubber performSelector:@selector(pan:) withObject:pan];
    NSCAssert([sent isEqual:(@[@[@"timeline_frame", @"60"], @[@"timeline_frame", @"61"]])],
              @"Scrubbing sends each whole frame once: %@", sent);
    NSCAssert(CGAffineTransformIsIdentity(knob.transform) && [frame.textColor isEqual:UIColor.whiteColor],
              @"Released, the knob settles back into the tinted playhead");
    [timeline updateSnapshot:@{@"frame": @61, @"start": @10, @"end": @90, @"playing": @NO, @"preview": @NO}];
    [sent removeAllObjects];
    [scrubber accessibilityIncrement];
    NSCAssert([sent isEqual:(@[@[@"timeline_frame", @"62"]])], @"VoiceOver steps one frame at a time");

    [timeline layoutInRect:CGRectMake(12, 70, 360, 600)];
    NSCAssert(find(timeline.view, @"blender.timeline.start").hidden && !play.hidden,
              @"Compact widths keep play and the scrubber");

    /* Keyframes: marks on the track, and a key track that grows above the knob to edit them. */
    [timeline layoutInRect:lane];
    [timeline.view layoutIfNeeded];
    NSArray *keys = @[@{@"f": @20, @"s": @NO}, @{@"f": @50, @"s": @YES}];
    [timeline updateSnapshot:@{@"frame": @30, @"start": @10, @"end": @90, @"playing": @NO, @"keys": keys}];
    NSCAssert([[scrubber valueForKey:@"keys"] isEqual:keys], @"Keyed frames reach the scrubber");
    [sent removeAllObjects];
    TestTap *tap = [[[TestTap alloc] init] autorelease];
    tap.testPoint = CGPointMake(xFor(50), trackY);
    [scrubber performSelector:@selector(tap:) withObject:tap];
    NSCAssert([sent isEqual:(@[@[@"timeline_frame", @"50"]])], @"Tapping the track jumps to that frame");
    UIButton *deleteKeys = (UIButton *)find(timeline.view, @"blender.timeline.delete-keys");
    NSCAssert(deleteKeys.hidden, @"Collapsed, the timeline does not offer to delete keys");
    const CGFloat collapsed = timeline.height;
    const int before = relayouts;
    UIButton *keysToggle = (UIButton *)find(timeline.view, @"blender.timeline.keys-toggle");
    [keysToggle sendActionsForControlEvents:UIControlEventTouchUpInside];
    [timeline layoutInRect:lane];
    [timeline.view layoutIfNeeded];
    UIView *transportRow = play.superview;
    NSCAssert(timeline.expanded && fabs(timeline.height - 136) < 0.5 && relayouts == before + 1 &&
              fabs(scrubber.bounds.size.height - (timeline.height - 44)) < 0.5 &&
              scrubber.frame.size.width > timeline.view.bounds.size.width - 20 &&
              CGRectGetMinY(transportRow.frame) >= CGRectGetMaxY(scrubber.frame) - 0.5,
              @"Expanded, the controls move to a bottom row and the scrubber takes the full width above");
    NSCAssert(fabs(CGRectGetMidX(transportRow.frame) - CGRectGetMidX(timeline.view.bounds)) < 0.5,
              @"Expanded, the playback controls are centered");
    NSCAssert(!deleteKeys.hidden && CGRectGetMaxX(deleteKeys.frame) < CGRectGetMinX(transportRow.frame),
              @"With keys selected, Delete shows left of the controls");
    NSCAssert(fabs(CGRectGetMaxY(timeline.view.frame) - CGRectGetMaxY(lane)) < 0.5,
              @"The expanded timeline grows upward from the bottom of its lane");
    const CGFloat expandedTrackY = scrubber.bounds.size.height - 25;
    NSCAssert(fabs(knob.center.x - xFor(50)) < 0.5 && fabs(knob.center.y - expandedTrackY) < 0.5,
              @"The knob keeps its frame and the playhead line rises from it");
    const CGFloat keyY = [(id)scrubber keysY];
    NSCAssert(keyY < expandedTrackY - 30, @"Keys sit above the knob");
    void (^drag)(CGFloat, CGFloat, CGFloat) = ^(CGFloat from, CGFloat to, CGFloat y) {
      pan.testState = UIGestureRecognizerStateBegan;
      pan.testTranslation = CGPointZero;
      pan.testPoint = CGPointMake(from, y);
      [scrubber performSelector:@selector(pan:) withObject:pan];
      pan.testState = UIGestureRecognizerStateChanged;
      pan.testTranslation = CGPointMake(to - from, 0);
      pan.testPoint = CGPointMake(to, y);
      [scrubber performSelector:@selector(pan:) withObject:pan];
      pan.testState = UIGestureRecognizerStateEnded;
      [scrubber performSelector:@selector(pan:) withObject:pan];
    };
    NSArray *(^frames)(BOOL) = ^NSArray *(BOOL selectedOnly) {
      NSMutableArray *result = [NSMutableArray array];
      for (NSDictionary *key in [scrubber valueForKey:@"keys"]) {
        if (!selectedOnly || [key[@"s"] boolValue]) {
          [result addObject:key[@"f"]];
        }
      }
      return result;
    };

    [sent removeAllObjects];
    pan.testState = UIGestureRecognizerStateBegan;
    pan.testPoint = CGPointMake(xFor(50) + 12, keyY);
    pan.testTranslation = CGPointMake(12, 0);
    [scrubber performSelector:@selector(pan:) withObject:pan];
    pan.testState = UIGestureRecognizerStateChanged;
    pan.testPoint = CGPointMake(xFor(70), keyY);
    [scrubber performSelector:@selector(pan:) withObject:pan];
    NSCAssert(sent.count == 0 && [frame.text isEqual:@"50"],
              @"Dragging a key previews it without editing or scrubbing until release");
    pan.testState = UIGestureRecognizerStateEnded;
    [scrubber performSelector:@selector(pan:) withObject:pan];
    NSCAssert([sent isEqual:(@[@[@"timeline_key_move", @"20"]])], @"Releasing moves the selected keys: %@", sent);
    NSCAssert([frames(NO) isEqual:(@[@20, @70])] && [frames(YES) isEqual:(@[@70])],
              @"The key shows at its new frame before Blender confirms it");

    [sent removeAllObjects];
    drag(xFor(20), xFor(25), keyY);
    NSCAssert([sent isEqual:(@[@[@"timeline_key_select", @"20|0"], @[@"timeline_key_move", @"5"]])] &&
              [frames(YES) isEqual:(@[@25])] && [frames(NO) isEqual:(@[@25, @70])],
              @"Dragging an unselected key selects it alone and moves it: %@", sent);

    [sent removeAllObjects];
    tap.testPoint = CGPointMake(xFor(70) + 6, keyY);
    [scrubber performSelector:@selector(tap:) withObject:tap];
    NSCAssert([sent isEqual:(@[@[@"timeline_key_select", @"70|0"]])] && [frames(YES) isEqual:(@[@70])] &&
              [frame.text isEqual:@"50"],
              @"Tapping a key selects it without moving the playhead: %@", sent);
    [sent removeAllObjects];
    tap.testPoint = CGPointMake(xFor(45), keyY);
    [scrubber performSelector:@selector(tap:) withObject:tap];
    NSCAssert([sent isEqual:(@[@[@"timeline_key_select", @"|0"]])] && [frames(YES) count] == 0 && deleteKeys.hidden,
              @"Tapping between keys clears the selection and hides Delete: %@", sent);

    [sent removeAllObjects];
    pan.testState = UIGestureRecognizerStateBegan;
    pan.testTranslation = CGPointZero;
    pan.testPoint = CGPointMake(xFor(15), keyY);
    [scrubber performSelector:@selector(pan:) withObject:pan];
    pan.testState = UIGestureRecognizerStateChanged;
    pan.testPoint = CGPointMake(xFor(75), keyY + 6);
    [scrubber performSelector:@selector(pan:) withObject:pan];
    NSCAssert(sent.count == 0 && [[scrubber valueForKey:@"boxing"] boolValue] && [frame.text isEqual:@"50"],
              @"Dragging over empty key track draws a selection box, not a scrub");
    pan.testState = UIGestureRecognizerStateEnded;
    [scrubber performSelector:@selector(pan:) withObject:pan];
    NSCAssert([sent isEqual:(@[@[@"timeline_key_select", @"25,70|0"]])] && [frames(YES) isEqual:(@[@25, @70])] &&
              !deleteKeys.hidden,
              @"Releasing the box selects the keys inside it: %@", sent);
    [sent removeAllObjects];
    drag(xFor(25), xFor(28), keyY);
    NSCAssert([sent isEqual:(@[@[@"timeline_key_move", @"3"]])] && [frames(YES) isEqual:(@[@28, @73])],
              @"Selected keys move together: %@", sent);

    [sent removeAllObjects];
    [deleteKeys sendActionsForControlEvents:UIControlEventTouchUpInside];
    NSCAssert([sent isEqual:(@[@[@"timeline_key_delete", @""]])] && [frames(NO) count] == 0 && deleteKeys.hidden,
              @"Delete removes the selected keys: %@", sent);

    [sent removeAllObjects];
    tap.testPoint = CGPointMake(xFor(40), 6);
    [scrubber performSelector:@selector(tap:) withObject:tap];
    drag(xFor(40), xFor(33), expandedTrackY);
    NSCAssert([sent.firstObject isEqual:(@[@"timeline_frame", @"40"])] &&
              [sent.lastObject isEqual:(@[@"timeline_frame", @"33"])] && fabs(knob.center.x - xFor(33)) < 0.5,
              @"The ruler and the knob's band still scrub: %@", sent);
    [keysToggle sendActionsForControlEvents:UIControlEventTouchUpInside];
    NSCAssert(!timeline.expanded && timeline.height == collapsed, @"The key track collapses again");

    [sent removeAllObjects];
    [timeline toggle:nil];
    [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    NSCAssert([sent.firstObject isEqual:(@[@"timeline_visible", @"0"])],
              @"Blender stops listing keyframes once the timeline is hidden");
    NSCAssert(!timeline.visible && !timeline.button.selected && timeline.view.hidden && relayouts == before + 3,
              @"Toggle hides the timeline");
    NSLog(@"PASS: floating timeline");
  }
  return 0;
}
