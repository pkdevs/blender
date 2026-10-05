/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Harness for test_ipad_pencil.py. Production methods replace the markers below. */
#import <UIKit/UIKit.h>
#include <cassert>
#include <cmath>
#include <vector>

#define GHOST_ASSERT(condition, message) assert(condition)
#define BLI_assert_unreachable() assert(false)
#define IOS_INPUT_LOG(...) ((void)0)
#define UNUSED_VARS(...) ((void)0)
#define FALSE false
static bool navigationHover;
static void WM_apple_ios_ui_navigation_hover_set(bool active) { navigationHover = active; }
enum {
  GHOST_kTabletModeStylus = 1,
  GHOST_kEventCursorMove,
  GHOST_kEventButtonDown,
  GHOST_kEventButtonUp,
  GHOST_kEventKeyDown,
  GHOST_kEventKeyUp,
  GHOST_kButtonMaskLeft,
  GHOST_kButtonMaskRight,
  GHOST_kKeyEsc,
  GHOST_kTrackpadEventRotate,
  GHOST_kTrackpadEventScroll,
  GHOST_kTrackpadEventMagnify,
};
enum GHOST_TSuccess { GHOST_kFailure, GHOST_kSuccess };
struct GHOST_TabletData {
  int Active = 0;
  float Pressure = 0, Xtilt = 0, Ytilt = 0;
};
static const GHOST_TabletData GHOST_TABLET_DATA_NONE;
using GHOST_SystemHandle = void *;
static uint64_t GHOST_GetMilliSeconds(GHOST_SystemHandle) { return 1000; }
class GHOST_WindowIOS {
  int32_t pointer_position_[2] = {};
  bool pointer_position_valid_ = false;
 public:
  UIView *view = nil;
  UIView *getView() { return view; }
  CGPoint scalePointToWindow(CGPoint point) { return CGPointMake(point.x * 2, point.y * 2); }
  // POINTER_METHODS
};
struct GHOST_Event {
  int type, x = -1, y = -1, button = 0, key = 0;
  GHOST_TabletData tablet;
  GHOST_Event(int type) : type(type) {}
  virtual ~GHOST_Event() = default;
};
struct GHOST_EventCursor : GHOST_Event {
  GHOST_EventCursor(uint64_t, int type, GHOST_WindowIOS *, int x, int y, GHOST_TabletData tablet)
      : GHOST_Event(type) { this->x = x; this->y = y; this->tablet = tablet; }
};
struct GHOST_EventButton : GHOST_Event {
  GHOST_EventButton(uint64_t, int type, GHOST_WindowIOS *, int button, GHOST_TabletData tablet)
      : GHOST_Event(type) { this->button = button; this->tablet = tablet; }
};
struct GHOST_EventKey : GHOST_Event {
  GHOST_EventKey(uint64_t, int type, GHOST_WindowIOS *, int key, bool)
      : GHOST_Event(type) { this->key = key; }
};
struct GHOST_EventTrackpad : GHOST_Event {
  template<typename... Args> GHOST_EventTrackpad(Args...) : GHOST_Event(-100) {}
};
struct GHOST_SystemIOS {
  std::vector<GHOST_Event *> events;
  uint64_t getMilliSeconds() { return 1000; }
  void pushEvent(GHOST_Event *event) { events.push_back(event); }
  void clear() { for (auto *event : events) delete event; events.clear(); }
};

// INPUT_EVENT

@interface TestTouch : NSObject
@property(nonatomic) UITouchType type;
@property(nonatomic) UITouchPhase phase;
@property(nonatomic) CGPoint point;
@property(nonatomic) CGFloat force;
@property(nonatomic) CGFloat maximumPossibleForce;
@property(nonatomic, assign) UIView *view;
@end
@implementation TestTouch
- (CGPoint)preciseLocationInView:(UIView *)view { return self.point; }
- (CGFloat)azimuthAngleInView:(UIView *)view { return 0.5; }
- (CGFloat)altitudeAngle { return 0.8; }
@end

@interface TestEvent : NSObject
@property(nonatomic) UIEventType type;
@property(nonatomic, retain) NSSet *allTouches;
@property(nonatomic, retain) NSArray *samples;
@end
@implementation TestEvent
- (NSArray *)coalescedTouchesForTouch:(UITouch *)touch { return self.samples; }
@end

@interface GHOSTUIHoverGestureRecognizer : UIHoverGestureRecognizer
@property(nonatomic) UIGestureRecognizerState state;
@property(nonatomic) UIGestureRecognizerState testState;
@property(nonatomic) CGPoint point;
@end
@implementation GHOSTUIHoverGestureRecognizer
- (UIGestureRecognizerState)state { return self.testState; }
- (void)setState:(UIGestureRecognizerState)state { self.testState = state; }
- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window { return window->scalePointToWindow(self.point); }
- (CGPoint)locationInView:(UIView *)view { return self.point; }
@end

@interface TestMenuDelegate : NSObject <GHOSTIOSPencilMenuDelegate>
@property(nonatomic) NSUInteger choices;
@property(nonatomic) CGPoint anchor;
@property(nonatomic, copy) NSString *chosen;
@property(nonatomic, copy) NSDictionary *chosenRoute;
@property(nonatomic, copy) NSDictionary *requestedRoute;
@property(nonatomic, copy) NSArray *items;
@property(nonatomic) BOOL deferred;
@property(nonatomic, copy) void (^pending)(NSArray *);
@end
@implementation TestMenuDelegate
- (void)pencilMenu:(GHOSTIOSPencilMenu *)menu requestItemsForRoute:(NSDictionary *)route
          atPoint:(CGPoint)point completion:(void (^)(NSArray<NSDictionary *> *))completion {
  self.requestedRoute = route;
  if (self.deferred) {
    self.pending = completion;
    return;
  }
  if (self.items) {
    completion(self.items);
    return;
  }
  NSMutableArray *items = [NSMutableArray array];
  for (int i = 0; i < 18; ++i) {
    [items addObject:@{@"title": [NSString stringWithFormat:@"Action %d", i],
                      @"operator": [NSString stringWithFormat:@"TEST_OT_%d", i],
                      @"image": [UIImage systemImageNamed:@"cube"]}];
  }
  completion(items);
}
- (void)pencilMenu:(GHOSTIOSPencilMenu *)menu didChooseItem:(NSDictionary *)item atPoint:(CGPoint)point {
  self.choices++;
  self.anchor = point;
  self.chosen = item[@"operator"];
  self.chosenRoute = item[@"route"];
}
@end

static UIPencilPreferredAction tapAction = UIPencilPreferredActionShowContextualPalette;
static UIPencilPreferredAction squeezeAction = UIPencilPreferredActionShowContextualPalette;
@interface TestPencilInteraction : NSObject
@property(nonatomic, assign) UIView *view;
@end
@implementation TestPencilInteraction
+ (UIPencilPreferredAction)preferredTapAction { return tapAction; }
+ (UIPencilPreferredAction)preferredSqueezeAction { return squeezeAction; }
@end
#define UIPencilInteraction TestPencilInteraction

@interface TestPencilGesture : NSObject
@property(nonatomic) UIPencilInteractionPhase phase;
@property(nonatomic) CGPoint location;
@property(nonatomic, retain) TestPencilGesture *hoverPose;
@end
@implementation TestPencilGesture
@end

@interface TestWindowBase : UIView
- (void)sendEvent:(UIEvent *)event;
@end
@implementation TestWindowBase
- (void)sendEvent:(UIEvent *)event {}
@end

@interface TestWindow : TestWindowBase {
 @public
  GHOST_SystemIOS *system;
  GHOST_WindowIOS *window;
  UITouch *current_pencil_touch;
  GHOST_TabletData tablet_data;
  bool pencil_button_active, last_tap_with_pencil;
  bool finger_tool_enabled;
  GHOSTIOSPencilMenu *pencil_context_menu;
  UIView *hit_view;
}
@end
@implementation TestWindow
- (void)stopOrbitMomentum {}
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event { return hit_view ?: window->getView(); }
// NATIVE_METHODS
@end

static TestTouch *touch(UIView *view, CGPoint point, UITouchPhase phase = UITouchPhaseBegan)
{
  TestTouch *touch = [[[TestTouch alloc] init] autorelease];
  touch.type = UITouchTypePencil;
  touch.view = view;
  touch.point = point;
  touch.phase = phase;
  touch.maximumPossibleForce = 4;
  touch.force = 2;
  return touch;
}

static void send(TestWindow *window, TestTouch *touch, NSArray *samples = nil)
{
  TestEvent *event = [[[TestEvent alloc] init] autorelease];
  event.type = UIEventTypeTouches;
  event.allTouches = [NSSet setWithObject:touch];
  event.samples = samples;
  [window sendEvent:(id)event];
}

int main()
{
  @autoreleasepool {
    GHOST_SystemIOS system;
    GHOST_WindowIOS viewport;
    viewport.view = [[[UIView alloc] init] autorelease];
    UIView *toolbar = [[[UIView alloc] init] autorelease];
    TestWindow *window = [[[TestWindow alloc] init] autorelease];
    window->system = &system;
    window->window = &viewport;
    int32_t x = 17, y = 23;
    assert(viewport.getPointerPosition(x, y) == GHOST_kFailure && x == 17 && y == 23);

    // Consecutive distant strokes must each carry their own start and final point.
    for (int i = 0; i < 8; ++i) {
      system.clear();
      const CGPoint start = CGPointMake(20 + i * 70, 500 - i * 50);
      const CGPoint end = CGPointMake(start.x + 30, start.y - 40);
      TestTouch *stroke = touch(viewport.view, start);
      send(window, stroke);
      assert(system.events.size() == 2);
      send(window, stroke); // Duplicate begin in a multi-touch event must not re-press.
      assert(system.events.size() == 2);
      stroke.phase = UITouchPhaseMoved;
      stroke.point = end;
      NSArray *samples = @[
        touch(viewport.view, CGPointMake(start.x + 10, start.y - 10), UITouchPhaseMoved),
        touch(viewport.view, end, UITouchPhaseMoved),
      ];
      send(window, stroke, samples);
      assert(system.events.size() == 4);
      stroke.phase = UITouchPhaseEnded;
      // Termination follows touch identity even if the original view goes away.
      stroke.view = nil;
      stroke.point = CGPointMake(end.x + 5, end.y - 5);
      send(window, stroke);
      assert(system.events.size() == 6);
      assert(system.events[0]->type == GHOST_kEventCursorMove);
      assert(system.events[0]->x == start.x * 2 && system.events[0]->y == start.y * 2);
      assert(system.events[1]->type == GHOST_kEventButtonDown);
      assert(system.events[1]->tablet.Pressure == 0.5f);
      assert(system.events[2]->x == (start.x + 10) * 2);
      assert(system.events[3]->x == end.x * 2);
      assert(system.events[4]->x == stroke.point.x * 2);
      assert(system.events[4]->y == stroke.point.y * 2);
      assert(system.events[5]->type == GHOST_kEventButtonUp);
      assert(system.events[5]->tablet.Pressure == 0.0f);
      assert(viewport.getPointerPosition(x, y) == GHOST_kSuccess);
      assert(x == stroke.point.x * 2 && y == stroke.point.y * 2);
      assert(!window->pencil_button_active && !window->current_pencil_touch);
      assert(window->tablet_data.Active == 0);
      send(window, stroke); // Duplicate end must not release twice.
      assert(system.events.size() == 6);
    }

    system.clear();
    TestTouch *controlTouch = touch(toolbar, CGPointMake(10, 10));
    send(window, controlTouch);
    controlTouch.view = viewport.view;
    controlTouch.phase = UITouchPhaseMoved;
    send(window, controlTouch);
    controlTouch.phase = UITouchPhaseEnded;
    send(window, controlTouch);
    TestTouch *finger = touch(viewport.view, CGPointMake(20, 20));
    finger.type = UITouchTypeDirect;
    send(window, finger);
    assert(system.events.empty());

    // Phones use the same ordered press/move/release path without stylus pressure.
    [window setFingerToolEnabled:YES];
    for (int i = 0; i < 4; i++) {
      system.clear();
      TestTouch *stroke = touch(viewport.view, CGPointMake(50 + i * 90, 100));
      stroke.type = UITouchTypeDirect;
      send(window, stroke);
      stroke.phase = UITouchPhaseMoved;
      stroke.point = CGPointMake(80 + i * 90, 150);
      send(window, stroke);
      stroke.phase = UITouchPhaseEnded;
      send(window, stroke);
      assert(system.events.size() == 5);
      assert(system.events[0]->x == (50 + i * 90) * 2);
      assert(system.events[1]->type == GHOST_kEventButtonDown);
      assert(system.events[1]->tablet.Active == 0);
      assert(system.events[4]->type == GHOST_kEventButtonUp);
      assert(!window->pencil_button_active);
    }
    system.clear();
    TestTouch *first = touch(viewport.view, CGPointMake(40, 80));
    first.type = UITouchTypeDirect;
    send(window, first);
    first.phase = UITouchPhaseStationary;
    TestTouch *second = touch(viewport.view, CGPointMake(140, 80));
    second.type = UITouchTypeDirect;
    TestEvent *multi = [[[TestEvent alloc] init] autorelease];
    multi.type = UIEventTypeTouches;
    multi.allTouches = [NSSet setWithObjects:first, second, nil];
    [window sendEvent:(id)multi];
    assert(!window->pencil_button_active && system.events.size() == 5);
    assert(system.events[2]->type == GHOST_kEventKeyDown);
    assert(system.events.back()->type == GHOST_kEventButtonUp);
    second.phase = UITouchPhaseEnded;
    [window sendEvent:(id)multi];
    first.phase = UITouchPhaseMoved;
    send(window, first);
    assert(system.events.size() == 5); // Lifting one finger must not restart a tool.
    system.clear();
    [window setFingerToolEnabled:NO];
    first.phase = UITouchPhaseBegan;
    send(window, first);
    assert(system.events.empty());
    [window setFingerToolEnabled:YES];
    send(window, first);
    assert(window->pencil_button_active);
    [window setFingerToolEnabled:NO];
    assert(!window->pencil_button_active && system.events.back()->type == GHOST_kEventButtonUp);
    system.clear();

    GHOSTUIHoverGestureRecognizer *hover = [[[GHOSTUIHoverGestureRecognizer alloc] init] autorelease];
    hover.state = UIGestureRecognizerStateChanged;
    hover.point = CGPointMake(800, 900);
    [window handleHover:hover];
    assert(system.events.size() == 1 && system.events[0]->type == GHOST_kEventCursorMove);
    assert(system.events[0]->x == 1600 && system.events[0]->tablet.Pressure == 0.0f);
    assert(navigationHover);
    system.clear();

    // Pencil bypasses the finger-only navigation overlay, including a tap without prior hover.
    UIView *navigation = [[[UIView alloc] init] autorelease];
    navigation.accessibilityIdentifier = @"blender.viewport.navigation";
    TestTouch *axisTap = touch(navigation, CGPointMake(780, 1100));
    assert([window touchBeganInBlenderViewport:(id)axisTap]);
    send(window, axisTap);
    assert(system.events.size() == 2 && system.events[1]->type == GHOST_kEventButtonDown);
    assert(system.events[0]->tablet.Active == GHOST_kTabletModeStylus);
    assert(system.events[1]->tablet.Active == GHOST_kTabletModeStylus);
    assert(system.events[1]->tablet.Pressure > 0.0f); // Absolute input, never cursor-warp emulation.
    axisTap.phase = UITouchPhaseEnded;
    send(window, axisTap);
    assert(system.events.size() == 4 && system.events[3]->type == GHOST_kEventButtonUp);
    assert(system.events[3]->tablet.Active == GHOST_kTabletModeStylus && !navigationHover);
    assert(system.events[3]->tablet.Pressure == 0.0f);
    system.clear();
    axisTap.type = UITouchTypeDirect;
    assert(![window touchBeganInBlenderViewport:(id)axisTap]);
    window->hit_view = navigation;
    [window handleHover:hover];
    assert(navigationHover && system.events.size() == 1);
    system.clear();
    window->hit_view = toolbar;
    [window handleHover:hover];
    assert(!navigationHover && system.events.empty());
    window->hit_view = navigation;
    [window handleHover:hover];
    hover.state = UIGestureRecognizerStateEnded;
    [window handleHover:hover];
    assert(!navigationHover); // Lifting out of hover range must shrink the axis again.
    hover.state = UIGestureRecognizerStateChanged;
    window->hit_view = nil;
    system.clear();

    // OS cancellation, lost focus, and a replacement touch all cancel then release.
    for (int reason = 0; reason < 3; ++reason) {
      TestTouch *stroke = touch(viewport.view, CGPointMake(100, 200));
      send(window, stroke);
      [window handleHover:hover];
      assert(system.events.size() == 2); // Hover cannot move an active stroke.
      stroke.phase = UITouchPhaseMoved;
      stroke.point = CGPointMake(130, 240);
      send(window, stroke); // Non-coalesced move fallback.
      system.clear();
      if (reason == 0) {
        stroke.phase = UITouchPhaseCancelled;
        send(window, stroke);
      }
      else if (reason == 1) {
        [window applicationWillResignActive:nil];
      }
      else {
        send(window, touch(viewport.view, CGPointMake(700, 800)));
      }
      assert(system.events.size() == (reason == 2 ? 5 : 3));
      assert(system.events[0]->type == GHOST_kEventKeyDown && system.events[0]->key == GHOST_kKeyEsc);
      assert(system.events[1]->type == GHOST_kEventKeyUp && system.events[1]->key == GHOST_kKeyEsc);
      assert(system.events[2]->type == GHOST_kEventButtonUp);
      stroke.phase = UITouchPhaseEnded;
      send(window, stroke); // Retired touch must not terminate its replacement.
      assert(system.events.size() == (reason == 2 ? 5 : 3));
      if (reason == 2) {
        assert(window->pencil_button_active);
        assert(system.events[3]->x == 1400 && system.events[3]->y == 1600);
        assert(system.events[4]->type == GHOST_kEventButtonDown);
        [window cancelPencilStroke];
      }
      assert(!window->pencil_button_active && !window->current_pencil_touch);
      system.clear();
    }
    viewport.view.frame = CGRectMake(10, 20, 834, 1210);
    [window addSubview:viewport.view];
    GHOSTIOSPencilMenu *menu = [[[GHOSTIOSPencilMenu alloc] initWithFrame:viewport.view.bounds] autorelease];
    TestMenuDelegate *menuDelegate = [[[TestMenuDelegate alloc] init] autorelease];
    menu.delegate = menuDelegate;
    [viewport.view addSubview:menu];
    window->pencil_context_menu = menu;
    TestPencilInteraction *interaction = [[[TestPencilInteraction alloc] init] autorelease];
    interaction.view = viewport.view;
    TestPencilGesture *gesture = [[[TestPencilGesture alloc] init] autorelease];
    TestPencilGesture *pose = [[[TestPencilGesture alloc] init] autorelease];
    pose.location = CGPointMake(400, 500);
    gesture.hoverPose = pose;
    auto toggle = [&]() { [window pencilInteraction:interaction didReceiveTap:(id)gesture]; };
    auto select = [&](UIButton *button) {
      TestTouch *pen = touch(menu, button.center);
      send(window, pen);
      pen.phase = UITouchPhaseEnded;
      send(window, pen);
    };

    // Hardware double-tap opens in viewport coordinates without any emulated right press.
    toggle();
    assert(menu.active && menu.menuVisible && system.events.empty());
    assert([menu hitTest:CGPointMake(20, 20) withEvent:nil] == menu);
    NSArray<UIButton *> *buttons = [menu valueForKey:@"buttons"];
    assert(buttons.count == 8 && buttons.lastObject.tag == -1);
    assert(!buttons[0].userInteractionEnabled);
    select(buttons[0]);
    assert(!menu.active && !menu.menuVisible && system.events.empty());
    assert([menu hitTest:CGPointMake(20, 20) withEvent:nil] == nil);
    assert(menuDelegate.choices == 1 && [menuDelegate.chosen isEqual:@"TEST_OT_0"]);
    assert(CGPointEqualToPoint(menuDelegate.anchor, pose.location));

    // Honor disabled gestures and system shortcuts; squeeze fires only on completion.
    for (UIPencilPreferredAction action : {UIPencilPreferredActionIgnore,
                                          UIPencilPreferredActionRunSystemShortcut}) {
      tapAction = squeezeAction = action;
      toggle();
      gesture.phase = UIPencilInteractionPhaseEnded;
      [window pencilInteraction:interaction didReceiveSqueeze:(id)gesture];
      assert(!menu.active && system.events.empty());
    }
    tapAction = squeezeAction = UIPencilPreferredActionShowContextualPalette;
    for (UIPencilInteractionPhase phase : {UIPencilInteractionPhaseBegan,
                                           UIPencilInteractionPhaseChanged,
                                           UIPencilInteractionPhaseCancelled}) {
      gesture.phase = phase;
      [window pencilInteraction:interaction didReceiveSqueeze:(id)gesture];
      assert(!menu.active);
    }
    gesture.phase = UIPencilInteractionPhaseEnded;
    [window pencilInteraction:interaction didReceiveSqueeze:(id)gesture];
    assert(menu.active);
    [window pencilInteraction:interaction didReceiveSqueeze:(id)gesture];
    assert(!menu.active && menuDelegate.choices == 1);

    // Hover highlights without invoking; a fresh contact chooses exactly once.
    toggle();
    buttons = [menu valueForKey:@"buttons"];
    hover.state = UIGestureRecognizerStateChanged;
    hover.point = buttons[1].center;
    [window handleHover:hover];
    assert(buttons[1].selected && menuDelegate.choices == 1 && system.events.empty());
    select(buttons[1]);
    assert(menuDelegate.choices == 2 && [menuDelegate.chosen isEqual:@"TEST_OT_1"]);
    hover.state = UIGestureRecognizerStateEnded;
    [window handleHover:hover];

    // Paging keeps the palette open; the next Pencil contact chooses from that page.
    toggle();
    buttons = [menu valueForKey:@"buttons"];
    select(buttons.lastObject);
    buttons = [menu valueForKey:@"buttons"];
    assert(menu.menuVisible && menu.active && buttons[0].tag == 7);
    select(buttons[0]);
    assert(menuDelegate.choices == 3 && [menuDelegate.chosen isEqual:@"TEST_OT_7"]);
    assert(system.events.empty());

    // Closing during a menu contact swallows its remaining motion and release.
    toggle();
    TestTouch *pen = touch(menu, CGPointMake(400, 500));
    send(window, pen);
    toggle();
    pen.phase = UITouchPhaseMoved;
    pen.point = CGPointMake(600, 500);
    send(window, pen);
    pen.phase = UITouchPhaseEnded;
    send(window, pen);
    assert(!menu.active && system.events.empty() && menuDelegate.choices == 3);

    // Triggering mid-tool cancels the tool, but that old stroke cannot select an item.
    pen = touch(viewport.view, CGPointMake(400, 500));
    send(window, pen);
    assert(system.events.size() == 2);
    system.clear();
    gesture.hoverPose = nil;
    toggle();
    assert(menu.menuVisible && system.events.size() == 3);
    assert(system.events[0]->type == GHOST_kEventKeyDown && system.events[0]->key == GHOST_kKeyEsc);
    assert(system.events[2]->type == GHOST_kEventButtonUp);
    system.clear();
    buttons = [menu valueForKey:@"buttons"];
    pen.point = buttons[0].center;
    pen.phase = UITouchPhaseMoved;
    send(window, pen);
    pen.phase = UITouchPhaseEnded;
    send(window, pen);
    assert(menu.active && menuDelegate.choices == 3 && system.events.empty());
    [window applicationWillResignActive:nil];
    assert(!menu.active);
    gesture.hoverPose = pose;

    // An outside finger dismisses; Pencil over a native control does not open.
    toggle();
    finger = touch(menu, CGPointMake(20, 20));
    finger.type = UITouchTypeDirect;
    send(window, finger);
    assert(!menu.active && system.events.empty());
    window->hit_view = toolbar;
    toggle();
    assert(!menu.active);
    window->hit_view = nil;

    // Check non-overlap and containment at every corner and compact window size.
    for (CGSize size : {CGSizeMake(834, 1210), CGSizeMake(1210, 834), CGSizeMake(320, 650)}) {
      menu.frame = CGRectMake(0, 0, size.width, size.height);
      for (CGPoint anchor : {CGPointMake(1, 1), CGPointMake(size.width - 1, 1),
                             CGPointMake(1, size.height - 1),
                             CGPointMake(size.width - 1, size.height - 1)}) {
        pose.location = anchor;
        toggle();
        [menu layoutIfNeeded];
        buttons = [menu valueForKey:@"buttons"];
        for (UIButton *button in buttons) {
          assert(CGRectContainsRect(menu.bounds, button.frame));
          for (UIButton *other in buttons) {
            if (button != other) assert(!CGRectIntersectsRect(button.frame, other.frame));
          }
        }
        toggle();
        assert(!menu.active && menuDelegate.choices == 3 && system.events.empty());
      }
    }
    menu.frame = CGRectMake(0, 0, 834, 1210);
    pose.location = CGPointMake(400, 500);
    toggle();
    menu.frame = CGRectMake(0, 0, 800, 1100);
    [menu layoutIfNeeded];
    assert(!menu.active); // Resizing invalidates the radial geometry.
    menu.frame = CGRectMake(0, 0, 834, 1210);

    // No hover hardware: use last contact, or arm until first contact if none is known.
    GHOSTIOSPencilMenu *freshMenu = [[[GHOSTIOSPencilMenu alloc]
        initWithFrame:viewport.view.bounds] autorelease];
    freshMenu.delegate = menuDelegate;
    [viewport.view addSubview:freshMenu];
    window->pencil_context_menu = freshMenu;
    gesture.hoverPose = nil;
    toggle();
    assert(freshMenu.active && !freshMenu.menuVisible);
    pen = touch(freshMenu, CGPointMake(300, 400));
    send(window, pen);
    pen.phase = UITouchPhaseEnded;
    send(window, pen);
    assert(freshMenu.active && freshMenu.menuVisible && menuDelegate.choices == 3);
    toggle();
    toggle();
    assert(freshMenu.menuVisible);
    assert(CGPointEqualToPoint([[freshMenu valueForKey:@"anchor"] CGPointValue], pen.point));
    toggle();
    [freshMenu removeFromSuperview];
    window->pencil_context_menu = menu;
    gesture.hoverPose = pose;

    // Late replies cannot resurrect a dismissed palette or replace a newer menu.
    menuDelegate.deferred = YES;
    toggle();
    assert(menu.menuVisible && [[menu valueForKey:@"buttons"] count] == 0);
    assert(([menuDelegate.requestedRoute isEqual:@{@"root": @"context", @"path": @[]}]));
    void (^staleReply)(NSArray *) = [[menuDelegate.pending copy] autorelease];
    pen = touch(menu, pose.location);
    send(window, pen);
    pen.phase = UITouchPhaseEnded;
    send(window, pen);
    assert(menu.menuVisible); // A quick lift must not cancel an in-flight query.
    toggle();
    staleReply(@[@{@"title": @"Stale Action", @"operator": @"TEST_OT_stale"}]);
    assert(!menu.menuVisible);
    toggle();
    staleReply(@[@{@"title": @"Stale Action", @"operator": @"TEST_OT_stale"}]);
    assert([[menu valueForKey:@"buttons"] count] == 0);

    NSDictionary *submenuRoute = @{@"root": @"VIEW3D_MT_object_context_menu", @"path": @[@"origin"]};
    NSDictionary *leafRoute = @{@"root": submenuRoute[@"root"], @"path": @[@"origin", @"cursor"]};
    NSMutableArray *rootItems = [NSMutableArray array];
    for (int i = 0; i < 7; i++) {
      [rootItems addObject:@{@"title": @"Disabled", @"disabled": @YES}];
    }
    [rootItems addObject:@{@"title": @"Set Origin", @"submenu": @YES, @"route": submenuRoute}];
    menuDelegate.pending(rootItems);
    buttons = [menu valueForKey:@"buttons"];
    pen = touch(menu, buttons[0].center);
    send(window, pen);
    assert(!buttons[0].enabled && !buttons[0].selected && menuDelegate.choices == 3);
    pen.point = buttons.lastObject.center;
    pen.phase = UITouchPhaseEnded;
    send(window, pen);
    buttons = [menu valueForKey:@"buttons"];
    assert(buttons[0].tag == 7);
    select(buttons[0]);
    assert(menu.active && menu.menuVisible && menuDelegate.choices == 3);
    assert([menuDelegate.requestedRoute isEqual:submenuRoute]);
    NSArray *children = @[@{@"title": @"Origin to 3D Cursor", @"operator": @"OBJECT_OT_origin_set",
                             @"route": leafRoute, @"checked": @YES}];
    menuDelegate.pending(children);
    buttons = [menu valueForKey:@"buttons"];
    assert(buttons.count == 2 && [buttons[0].accessibilityIdentifier isEqual:@"blender.pencil.back"]);
    assert([buttons[1].accessibilityValue isEqual:@"On"]);
    select(buttons[0]);
    buttons = [menu valueForKey:@"buttons"];
    assert(buttons[0].tag == 7); // Back restores the parent page.
    select(buttons[0]);
    menuDelegate.pending(children);
    buttons = [menu valueForKey:@"buttons"];
    select(buttons[1]);
    assert(menuDelegate.choices == 4 && [menuDelegate.chosenRoute isEqual:leafRoute]);
    assert(CGPointEqualToPoint(menuDelegate.anchor, CGPointMake(400, 500)));
    assert(!menu.active && system.events.empty());
    menuDelegate.pending = nil;
    NSLog(@"PASS: Pencil event ordering, hardware secondary gestures, radial selection, cancellation, async menus and submenus");
  }
}
