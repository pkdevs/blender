/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GHOST_WindowIOS.hh"
#include "GHOST_IOSRuntime.hh"

#include "GHOST_ContextIOS.hh"
#include "GHOST_SystemIOS.hh"

#include "GHOST_Debug.hh"
#include "GHOST_EventButton.hh"
#include "GHOST_EventCursor.hh"
#include "GHOST_EventDragnDrop.hh"
#include "GHOST_EventKey.hh"
#import "GHOST_IOSPencilMenu.hh"
#include "GHOST_EventTouch.hh"
#include "GHOST_EventTrackpad.hh"

#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <UIKit/UIPencilInteraction.h>

#include <unordered_map>

// #define IOS_INPUT_LOGGING
#if defined(IOS_INPUT_LOGGING)
#  define IOS_INPUT_LOG(...) NSLog(__VA_ARGS__)
#else
#  define IOS_INPUT_LOG(...)
#endif

// #define IOS_WINDOW_LOGGING
#if defined(IOS_WINDOW_LOGGING)
#  define IOS_WINDOW_LOG(...) NSLog(__VA_ARGS__)
#else
#  define IOS_WINDOW_LOG(...)
#endif

extern "C" void WM_apple_ios_ui_command_enqueue(int command);
#import "GHOST_IOSScenePanels.hh"
#import "GHOST_IOSIcons.hh"
#import "GHOST_IOSRedoStrip.hh"
#import "GHOST_IOSTimeline.hh"
#import "GHOST_IOSAssets.hh"
extern "C" void WM_apple_ios_ui_operator_enqueue(const char *op_name);
extern "C" void WM_apple_ios_ui_operator_at_position_enqueue(const char *op_name, int x, int y);
extern "C" void WM_apple_ios_ui_operator_enum_enqueue(const char *op_name,
                                                       const char *prop_name,
                                                       const char *prop_value);
extern "C" uint64_t WM_apple_ios_ui_menu_request(const char *route, bool execute);
extern "C" uint64_t WM_apple_ios_ui_menu_at_position_request(const char *route,
                                                            bool execute,
                                                            int x,
                                                            int y);
extern "C" char *WM_apple_ios_ui_menu_take_result(uint64_t identifier);
extern "C" void WM_apple_ios_ui_setting_enqueue(const char *setting, const char *value);
extern "C" void WM_apple_ios_ui_tool_set_by_id(const char *tool_id);
extern "C" void WM_apple_ios_ui_brush_activate(const char *brush_id);
extern "C" void WM_apple_ios_ui_view_roll_enqueue(float angle);
extern "C" void WM_apple_ios_ui_navigation_hover_set(bool active);
extern "C" void WM_apple_ios_ui_pencil_hover_set(bool active, int x, int y);
extern "C" char *WM_apple_ios_ui_toolbar_copy_json();
extern "C" char *WM_apple_ios_ui_panels_take_json();
extern "C" char *WM_apple_ios_ui_timeline_take_json();
extern "C" void WM_apple_ios_ui_bottom_tray_set(float fraction);
extern "C" char *WM_apple_ios_ui_assets_take_json();
extern "C" void WM_apple_ios_ui_toolbar_free_json(char *json);

enum GHOSTIOSUICommand {
  GHOST_IOS_UI_COMMAND_SET_MODE_OBJECT = 0,
  GHOST_IOS_UI_COMMAND_SET_MODE_EDIT = 1,
  GHOST_IOS_UI_COMMAND_SET_MODE_SCULPT = 2,
  GHOST_IOS_UI_COMMAND_SET_MODE_VERTEX_PAINT = 3,
  GHOST_IOS_UI_COMMAND_SET_MODE_WEIGHT_PAINT = 4,
  GHOST_IOS_UI_COMMAND_SET_MODE_TEXTURE_PAINT = 5,
  GHOST_IOS_UI_COMMAND_SELECT_TOOL_SELECT_BOX = 6,
  GHOST_IOS_UI_COMMAND_SELECT_TOOL_CURSOR = 7,
  GHOST_IOS_UI_COMMAND_SELECT_TOOL_MOVE = 8,
  GHOST_IOS_UI_COMMAND_SELECT_TOOL_ROTATE = 9,
  GHOST_IOS_UI_COMMAND_SELECT_TOOL_SCALE = 10,
  GHOST_IOS_UI_COMMAND_SELECT_TOOL_TRANSFORM = 11,
  GHOST_IOS_UI_COMMAND_SELECT_TOOL_ANNOTATE = 12,
  GHOST_IOS_UI_COMMAND_SELECT_TOOL_MEASURE = 13,
  GHOST_IOS_UI_COMMAND_FRAME_SELECTED = 14,
  GHOST_IOS_UI_COMMAND_RESET_VIEW = 15,
  GHOST_IOS_UI_COMMAND_ORBIT_LEFT = 16,
  GHOST_IOS_UI_COMMAND_ORBIT_RIGHT = 17,
  GHOST_IOS_UI_COMMAND_ORBIT_UP = 18,
  GHOST_IOS_UI_COMMAND_ORBIT_DOWN = 19,
  GHOST_IOS_UI_COMMAND_SET_VIEWPORT_WIREFRAME = 20,
  GHOST_IOS_UI_COMMAND_SET_VIEWPORT_SOLID = 21,
  GHOST_IOS_UI_COMMAND_SET_VIEWPORT_MATERIAL = 22,
  GHOST_IOS_UI_COMMAND_SET_VIEWPORT_RENDERED = 23,
  GHOST_IOS_UI_COMMAND_VIEW_ALL = 24,
  GHOST_IOS_UI_COMMAND_VIEW_LOCAL = 25,
  GHOST_IOS_UI_COMMAND_VIEW_CAMERA = 26,
  GHOST_IOS_UI_COMMAND_VIEW_PERSP_ORTHO = 27,
  GHOST_IOS_UI_COMMAND_VIEW_FRONT = 28,
  GHOST_IOS_UI_COMMAND_VIEW_BACK = 29,
  GHOST_IOS_UI_COMMAND_VIEW_RIGHT = 30,
  GHOST_IOS_UI_COMMAND_VIEW_LEFT = 31,
  GHOST_IOS_UI_COMMAND_VIEW_TOP = 32,
  GHOST_IOS_UI_COMMAND_VIEW_BOTTOM = 33,
};

/* Semantic keys in release/ios/blender_icons.json select Blender artwork without
 * conflating SF Symbols reused for different tools. UIImage caches the assets;
 * tool artwork retains its colors, while interface icons use UIKit template tinting. */
static UIImage *blenderIOSIcon(NSString *key, NSString *systemImage)
{
  const BOOL selected_tool = [key hasPrefix:@"selected.tool."];
  if (selected_tool) { key = [key substringFromIndex:@"selected.".length]; }
  static NSDictionary *icons = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSURL *url = [NSBundle.mainBundle URLForResource:@"blender_icons" withExtension:@"json"];
    NSData *data = url ? [NSData dataWithContentsOfURL:url] : nil;
    id decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if ([decoded isKindOfClass:NSDictionary.class]) {
      icons = [decoded copy];
    }
  });
  NSString *name = key ? icons[key] : nil;
  if (name == nil && systemImage != nil) {
    name = icons[[@"symbol." stringByAppendingString:systemImage]];
  }
  NSString *prefix = [key hasPrefix:@"tool."] ? @"blender.tool." : @"blender.";
  if (name && [key hasPrefix:@"tool."]) {
    NSString *colored_prefix = selected_tool ? @"blender.tool.selected." : @"blender.tool.color.";
    UIImage *colored = [UIImage imageNamed:[colored_prefix stringByAppendingString:name]];
    if (colored) {
      return [colored imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
    }
  }
  UIImage *image = nil;
  if (name && [key hasPrefix:@"tool."]) {
    image = [[UIImage imageNamed:[prefix stringByAppendingString:name]]
        imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  }
  else if (name) {
    image = GHOSTIOSBlenderIcon(name, NO);
  }
  if (image != nil) {
    return image;
  }
  return systemImage ? [UIImage systemImageNamed:systemImage] : nil;
}

/** Brush previews are square renders; crop them to a circle like Blender's brush cursor. */
static UIImage *blenderIOSBrushImage(NSString *previewPath, CGFloat diameter)
{
  static NSCache<NSString *, UIImage *> *cache = [[NSCache alloc] init];
  NSString *cache_key = [NSString stringWithFormat:@"%@@%.0f", previewPath ?: @"", diameter];
  if (UIImage *cached = [cache objectForKey:cache_key]) {
    return cached;
  }
  UIImage *preview = previewPath.length ? [UIImage imageWithContentsOfFile:previewPath] : nil;
  UIImage *symbol = preview ? nil : [UIImage systemImageNamed:@"paintbrush.pointed.fill"
      withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:diameter * 0.42]];
  const CGRect bounds = CGRectMake(0, 0, diameter, diameter);
  UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
  UIGraphicsImageRenderer *renderer = [[[UIGraphicsImageRenderer alloc]
      initWithSize:bounds.size format:format] autorelease];
  UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext * /*context*/) {
    UIBezierPath *circle = [UIBezierPath bezierPathWithOvalInRect:bounds];
    [circle addClip];
    if (preview) {
      /* Some previews are partly transparent; keep every brush a solid disc on the glass. */
      [[UIColor colorWithWhite:0.16 alpha:1.0] setFill];
      UIRectFill(bounds);
      /* Aspect-fill the square preview so the circle has no empty corners. */
      const CGSize size = preview.size;
      const CGFloat scale = diameter / MAX(1.0, MIN(size.width, size.height));
      const CGSize fill = CGSizeMake(size.width * scale, size.height * scale);
      [preview drawInRect:CGRectMake((diameter - fill.width) / 2, (diameter - fill.height) / 2,
                                     fill.width, fill.height)];
    }
    else {
      [[UIColor colorWithWhite:0.5 alpha:0.28] setFill];
      UIRectFill(bounds);
      UIImage *tinted = [symbol imageWithTintColor:[UIColor colorWithWhite:0.92 alpha:1.0]
                                     renderingMode:UIImageRenderingModeAlwaysOriginal];
      [tinted drawAtPoint:CGPointMake((diameter - tinted.size.width) / 2,
                                      (diameter - tinted.size.height) / 2)];
    }
    UIBezierPath *ring = [UIBezierPath bezierPathWithOvalInRect:CGRectInset(bounds, 0.5, 0.5)];
    ring.lineWidth = 1.0;
    [[UIColor colorWithWhite:1.0 alpha:0.22] setStroke];
    [ring stroke];
  }];
  image = [image imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
  if (preview || !previewPath.length) {
    /* A missing file may still be written by Blender; only cache what is final. */
    [cache setObject:image forKey:cache_key];
  }
  return image;
}

static UIImage *blenderIOSCommandIcon(NSInteger command, NSString *systemImage)
{
  return blenderIOSIcon([NSString stringWithFormat:@"command.%ld", (long)command], systemImage);
}

static UIImage *blenderIOSActionIcon(NSString *opName,
                                    NSString *propertyName,
                                    NSString *propertyValue)
{
  NSString *key = [@"operator." stringByAppendingString:opName ?: @""];
  if (propertyName.length > 0 && propertyValue.length > 0) {
    UIImage *image = blenderIOSIcon(
        [NSString stringWithFormat:@"%@.%@.%@", key, propertyName, propertyValue], nil);
    if (image != nil) {
      return image;
    }
  }

  /* Only unambiguous, operator-specific SF fallbacks belong in action shortcuts.
   * Unmapped actions are text-only, never given a generic placeholder. */
  NSDictionary *fallbacks = @{
    @"ED_OT_undo": @"arrow.uturn.backward",
    @"ED_OT_redo": @"arrow.uturn.forward",
    @"OBJECT_OT_duplicate_move": @"plus.square.on.square",
    @"OBJECT_OT_delete": @"trash",
    @"MESH_OT_delete": @"trash",
  };
  return blenderIOSIcon(key, opName ? fallbacks[opName] : nil);
}

struct TouchData {
  CGPoint pos;
  bool part_of_multitouch = false;
};

typedef struct UserInputEvent {
  enum EventTypes {
    CURSOR_MOVE,
    ORBIT_GESTURE,
    PAN_GESTURE,
    PAN_GESTURE_TWO_FINGERS,
    PINCH_GESTURE,
    LEFT_BUTTON_DOWN,
    LEFT_BUTTON_UP,
    CANCEL_TOOL,
  };
  EventTypes event_list[10];
  int num_events;
  CGPoint location;
  CGPoint translation;
  CGFloat distance;
  bool pencil_used;
  uint64_t gesture_id = 0;

  UserInputEvent(CGPoint *loc, CGPoint *tran, CGFloat *dist, bool pencil)
  {
    num_events = 0;
    location = loc ? *loc : CGPointMake(-1.0f, -1.0f);
    translation = tran ? *tran : CGPointMake(0.0f, 0.0f);
    distance = dist ? *dist : 0.0f;
    pencil_used = pencil;
  }

  void add_event(EventTypes event_type)
  {
    GHOST_ASSERT(num_events <= sizeof(event_list) / sizeof(*event_list),
                 "add_event: Failed to add event");
    event_list[num_events] = event_type;
    num_events++;
  }

  NSString *getEventTypeDesc(EventTypes event_type) const
  {
    switch (event_type) {
      case CURSOR_MOVE:
        return @"CM";
      case ORBIT_GESTURE:
        return @"ORBIT";
      case PAN_GESTURE:
        return @"PAN";
      case PAN_GESTURE_TWO_FINGERS:
        return @"PAN2F";
      case PINCH_GESTURE:
        return @"PINCH";
      case LEFT_BUTTON_DOWN:
        return @"LB-DOWN";
      case LEFT_BUTTON_UP:
        return @"LB-UP";
      case CANCEL_TOOL:
        return @"CANCEL";
    }
    BLI_assert_unreachable();
    return @"Event undefined";
  }

} UserInputEvent;

/* GHOSTUITapGesture interface for capturing taps. */
@interface GHOSTUITapGestureRecognizer : UITapGestureRecognizer

- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window;

@end

@implementation GHOSTUITapGestureRecognizer

- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window
{
  CGPoint touch_point = [self locationInView:window->getView()];
  return window->scalePointToWindow(touch_point);
}

@end

/* GHOSTUITapGesture interface for capturing taps. */
@interface GHOSTUIPanGestureRecognizer : UIPanGestureRecognizer
{
  CGPoint cached_translation;
}
- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window;
- (CGPoint)getScaledTranslation:(GHOST_WindowIOS *)window;

- (void)setCachedTranslation:(CGPoint)translation;
- (CGPoint)getCachedTranslation;
@end

@implementation GHOSTUIPanGestureRecognizer

- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window
{
  CGPoint touch_point = [self locationInView:window->getView()];
  return window->scalePointToWindow(touch_point);
}

- (CGPoint)getScaledTranslation:(GHOST_WindowIOS *)window
{
  CGPoint translation = [self translationInView:window->getView()];
  return window->scalePointToWindow(translation);
}

- (CGPoint)getRelativeTranslation:(CGPoint)translation
{
  CGPoint relative_translation;
  relative_translation.x = translation.x - cached_translation.x;
  relative_translation.y = translation.y - cached_translation.y;
  return relative_translation;
}

- (void)setCachedTranslation:(CGPoint)translation
{
  cached_translation = translation;
}

- (CGPoint)getCachedTranslation
{
  return cached_translation;
}
@end

@interface GHOSTUIHoverGestureRecognizer : UIHoverGestureRecognizer
- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window;
@end

@implementation GHOSTUIHoverGestureRecognizer

- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window
{
  CGPoint touch_point = [self locationInView:window->getView()];
  return window->scalePointToWindow(touch_point);
}
@end

@interface GHOSTUIPinchGestureRecognizer : UIPinchGestureRecognizer
{
  CGFloat cached_distance;
}
- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window touch_id:(int)touch_id;
- (CGFloat)getScaledDistance:(GHOST_WindowIOS *)window;
- (CGPoint)getPinchMidpoint:(GHOST_WindowIOS *)window;
- (void)setCachedDistance:(CGFloat)distance;
- (CGFloat)getCachedDistance;
@end

@implementation GHOSTUIPinchGestureRecognizer
- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window touch_id:(int)touch_id
{
  CGPoint touch_point = [self locationOfTouch:touch_id inView:window->getView()];
  return window->scalePointToWindow(touch_point);
}

- (CGFloat)getScaledDistance:(GHOST_WindowIOS *)window
{
  CGPoint touch_point0 = [self locationOfTouch:0 inView:window->getView()];
  CGPoint touch_point1 = [self locationOfTouch:1 inView:window->getView()];
  touch_point0 = window->scalePointToWindow(touch_point0);
  touch_point1 = window->scalePointToWindow(touch_point1);
  float dx = touch_point1.x - touch_point0.x;
  float dy = touch_point1.y - touch_point0.y;
  CGFloat point_distance = sqrt(dx * dx + dy * dy);
  return point_distance;
}

- (CGPoint)getPinchMidpoint:(GHOST_WindowIOS *)window
{
  CGPoint touch_point0 = [self locationOfTouch:0 inView:window->getView()];
  CGPoint touch_point1 = [self locationOfTouch:1 inView:window->getView()];
  touch_point0 = window->scalePointToWindow(touch_point0);
  touch_point1 = window->scalePointToWindow(touch_point1);
  CGPoint midPoint = CGPointMake((touch_point0.x + touch_point1.x) / 2.0f,
                                 (touch_point0.y + touch_point1.y) / 2.0f);
  return midPoint;
}

- (void)setCachedDistance:(CGFloat)distance
{
  cached_distance = distance;
}

- (CGFloat)getCachedDistance
{
  return cached_distance;
}
@end

@interface GHOSTIOSPassThroughOverlayView : UIView
@end

@implementation GHOSTIOSPassThroughOverlayView

- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event
{
  UIView *hit_view = [super hitTest:point withEvent:event];
  return hit_view == self ? nil : hit_view;
}

@end

@interface GHOSTIOSPassThroughStackView : UIStackView
@end

@implementation GHOSTIOSPassThroughStackView

- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event
{
  UIView *hit_view = [super hitTest:point withEvent:event];
  return hit_view == self ? nil : hit_view;
}

@end

@interface GHOSTIOSToolStackView : GHOSTIOSPassThroughStackView {
  UIButton *_selection;
  UIButton *_selectionTarget;
}
@end

@implementation GHOSTIOSToolStackView
- (instancetype)initWithFrame:(CGRect)frame
{
  if ((self = [super initWithFrame:frame])) {
    _selection = [UIButton buttonWithType:UIButtonTypeSystem];
    _selection.userInteractionEnabled = NO;
    _selection.isAccessibilityElement = NO;
    _selection.accessibilityIdentifier = @"blender.toolbar.selection";
    _selection.hidden = YES;
    UIButtonConfiguration *configuration = [UIButtonConfiguration prominentGlassButtonConfiguration];
    configuration.baseBackgroundColor = UIColor.systemBlueColor;
    configuration.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    configuration.automaticallyUpdateForSelection = NO;
    _selection.configuration = configuration;
    [self insertSubview:_selection atIndex:0];
  }
  return self;
}
- (void)layoutSubviews
{
  [super layoutSubviews];
  UIButton *target = nil;
  for (UIView *view in self.arrangedSubviews) {
    if ([view isKindOfClass:UIButton.class] && ((UIButton *)view).selected && !view.hidden) {
      target = (UIButton *)view;
      break;
    }
  }
  if (!target) {
    _selection.hidden = YES;
    [_selection.layer removeAllAnimations];
    _selectionTarget = nil;
    return;
  }
  const BOOL animate = !_selection.hidden && _selectionTarget != target &&
                       !UIAccessibilityIsReduceMotionEnabled();
  _selectionTarget = target;
  _selection.hidden = NO;
  [self sendSubviewToBack:_selection];
  if (animate) {
    [UIView animateWithDuration:0.16 delay:0 usingSpringWithDamping:0.9 initialSpringVelocity:0
        options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
        animations:^{ self->_selection.frame = target.frame; } completion:nil];
  }
  else if (!CGRectEqualToRect(_selection.frame, target.frame)) {
    _selection.frame = target.frame;
  }
}
@end

@interface GHOSTIOSNavigationBar : UINavigationBar
@end

@implementation GHOSTIOSNavigationBar
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event
{
  UIView *hit = [super hitTest:point withEvent:event];
  for (UIView *view = hit; view && view != self; view = view.superview) {
    /* Scroll views let the tool settings strip be swiped from between its controls. */
    if ([view isKindOfClass:UIControl.class] || [view isKindOfClass:UIScrollView.class]) {
      return hit;
    }
  }
  return nil;
}
@end

@interface GHOSTIOSNavigationController : UINavigationController
@end

@implementation GHOSTIOSNavigationController
- (void)buildMenuWithBuilder:(id<UIMenuBuilder>)builder
{
  [self.topViewController buildMenuWithBuilder:builder];
}
- (UIViewController *)childViewControllerForHomeIndicatorAutoHidden
{
  return self.topViewController;
}
@end

@interface GHOSTIOSPassThroughScrollView : UIScrollView
@end

@implementation GHOSTIOSPassThroughScrollView

- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event
{
  UIView *hit_view = [super hitTest:point withEvent:event];
  return hit_view == self ? nil : hit_view;
}

@end

@interface GHOSTIOSButtonScrollView : UIScrollView
@end

@implementation GHOSTIOSButtonScrollView

- (BOOL)touchesShouldCancelInContentView:(UIView *)view
{
  return [view isKindOfClass:UIButton.class] || [super touchesShouldCancelInContentView:view];
}

@end

@interface GHOSTIOSFadingButtonScrollView : GHOSTIOSButtonScrollView
@end

@implementation GHOSTIOSFadingButtonScrollView {
  CAGradientLayer *_edge_fade;
}

- (instancetype)initWithFrame:(CGRect)frame
{
  self = [super initWithFrame:frame];
  if (self) {
    _edge_fade = [[CAGradientLayer alloc] init];
    _edge_fade.startPoint = CGPointMake(0, 0.5);
    _edge_fade.endPoint = CGPointMake(1, 0.5);
    self.layer.mask = _edge_fade;
  }
  return self;
}

- (void)dealloc
{
  [_edge_fade release];
  [super dealloc];
}

- (void)layoutSubviews
{
  [super layoutSubviews];
  CGFloat width = self.bounds.size.width;
  CGFloat fade = width > 0 ? MIN(18.0, width * 0.25) / width : 0.0;
  BOOL leading = self.contentOffset.x + self.adjustedContentInset.left > 0.5;
  BOOL trailing = self.contentSize.width + self.adjustedContentInset.right -
                      CGRectGetMaxX(self.bounds) > 0.5;
  id opaque = (id)UIColor.blackColor.CGColor;
  id clear = (id)UIColor.clearColor.CGColor;
  /* UIScrollView's bounds origin follows its offset. Keep the mask fixed to the
   * visible window, with no implicit animation lag while dragging or resizing. */
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  _edge_fade.frame = self.bounds;
  _edge_fade.locations = @[@0, @(fade), @(1.0 - fade), @1];
  _edge_fade.colors = @[leading ? clear : opaque, opaque, opaque, trailing ? clear : opaque];
  [CATransaction commit];
}

@end

@interface GHOSTIOSPassThroughVisualEffectView : UIVisualEffectView
@end

@implementation GHOSTIOSPassThroughVisualEffectView

- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event
{
  UIView *hit_view = [super hitTest:point withEvent:event];
  return (hit_view == self || hit_view == self.contentView) ? nil : hit_view;
}

@end

/* GHOSTUIWindow interface. */
@interface GHOSTUIWindow : UIWindow <UIGestureRecognizerDelegate, UIPencilInteractionDelegate>
{
  GHOST_SystemIOS *system;
  GHOST_WindowIOS *window;

  GHOSTUITapGestureRecognizer *tap_gesture_recognizer;
  GHOSTUITapGestureRecognizer *tap2f_gesture_recognizer;
  GHOSTUITapGestureRecognizer *tap3f_gesture_recognizer;
  GHOSTUITapGestureRecognizer *tap4f_gesture_recognizer;
  GHOSTUIPanGestureRecognizer *pan_gesture_recognizer;
  GHOSTUIPanGestureRecognizer *pan2f_gesture_recognizer;
  GHOSTUIPinchGestureRecognizer *zoom_gesture_recognizer;
  UIRotationGestureRecognizer *rotation_gesture_recognizer;
  GHOSTUIHoverGestureRecognizer *hover_gesture_recognizer;
  UIPencilInteraction *pencil_interaction;
  UIScreenEdgePanGestureRecognizer *edge_swipe_left;
  UIScreenEdgePanGestureRecognizer *edge_swipe_right;
  // GHOSTUILongPressGestureRecognizer *long_press_gesture_recognizer;

  /* Data from the Apple pencil */
  UITouch *current_pencil_touch;
  GHOST_TabletData tablet_data;
  bool pencil_button_active;
  bool pencil_navigation_pending;
  CGPoint pencil_navigation_start;
  bool last_tap_with_pencil;
  bool finger_tool_enabled;
  GHOSTIOSPencilMenu *pencil_context_menu;

  CADisplayLink *orbit_display_link;
  CGPoint orbit_velocity;
  CGPoint orbit_location;
  CGPoint orbit_remainder;
  CFTimeInterval orbit_timestamp;
  uint64_t orbit_gesture_id;

  /* Keyboard handling. */
  UITextField *text_field;
  NSString *original_text;
  bool onscreen_keyboard_active;
  const char *text_field_string;
  GHOST_KeyboardProperties current_keyboard_properties;
  bool external_keyboard_connected;

  /* Toolbar */
  bool toolbar_enabled;
  UIToolbar *toolbar;
  UIBarButtonItem *toolbar_tip_item;
  UIBarButtonItem *toolbar_live_text_item;
  UIBarButtonItem *toolbar_done_editing_item;
  UIBarButtonItem *toolbar_cancel_editing_item;
}

- (void)setSystemAndWindowIOS:(GHOST_SystemIOS *)sysCocoa windowIOS:(GHOST_WindowIOS *)winCocoa;
- (void)setPencilContextMenu:(GHOSTIOSPencilMenu *)menu;
- (void)setFingerToolEnabled:(BOOL)enabled;
- (void)detachBlender;

/* Blender event generation. */
- (void)generateUserInputEvents:(const UserInputEvent &)event_info;
- (void)cancelPencilStroke;
- (void)handleNavigationTap:(UITapGestureRecognizer *)gesture;

/* Gesture recognizers. */
- (void)registerGestureRecognizers;
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer
    shouldRecognizeSimultaneouslyWithGestureRecognizer:
        (UIGestureRecognizer *)otherGestureRecognizer;
- (void)handleTap:(GHOSTUITapGestureRecognizer *)sender;
- (void)handlePan:(GHOSTUIPanGestureRecognizer *)sender;
- (void)stopOrbitMomentum;
- (void)updateOrbitMomentum:(CADisplayLink *)display_link;
- (void)applicationWillResignActive:(NSNotification *)notification;
- (void)handlePan2f:(GHOSTUIPanGestureRecognizer *)sender;
- (void)handleZoom:(GHOSTUIPinchGestureRecognizer *)sender;
- (void)handleRotation:(UIRotationGestureRecognizer *)sender;

/* On screen keyboard handling */
- (UITextField *)getUITextField;
- (const GHOST_TabletData)getTabletData;
- (void)updateTabletDataFromPencilTouch:(UITouch *)touch;
- (CGPoint)getScaledPencilPoint:(UITouch *)touch;
- (void)sendPencilCursorEvent:(UITouch *)touch;
- (void)sendPencilButtonEvent:(UITouch *)touch isDown:(BOOL)isDown;
- (void)cancelPencilStroke;
- (BOOL)touchBeganInBlenderViewport:(UITouch *)touch;
- (BOOL)isBlenderPencilInputView:(UIView *)view;
- (GHOST_TSuccess)popupOnscreenKeyboard:(const GHOST_KeyboardProperties &)keyboard_properties;
- (GHOST_TSuccess)hideOnscreenKeyboard;
- (const char *)getLastKeyboardString;
@end

@implementation GHOSTUIWindow
- (void)detachBlender
{
  [self cancelPencilStroke];
  [self stopOrbitMomentum];
  [self hideOnscreenKeyboard];
  [NSNotificationCenter.defaultCenter removeObserver:self];
  self.userInteractionEnabled = NO;
  NSArray *recognizers = [window->getView().gestureRecognizers copy];
  for (UIGestureRecognizer *recognizer in recognizers) {
    recognizer.enabled = NO;
    [window->getView() removeGestureRecognizer:recognizer];
  }
  [recognizers release];
  recognizers = [self.gestureRecognizers copy];
  for (UIGestureRecognizer *recognizer in recognizers) {
    recognizer.enabled = NO;
    [self removeGestureRecognizer:recognizer];
  }
  [recognizers release];
  [pencil_context_menu cancel];
  pencil_context_menu = nil;
  window = nullptr;
  system = nullptr;
}

- (void)setFingerToolEnabled:(BOOL)enabled
{
  [self cancelPencilStroke];
  [self stopOrbitMomentum];
  finger_tool_enabled = enabled;
}

- (void)setPencilContextMenu:(GHOSTIOSPencilMenu *)menu
{
  pencil_context_menu = menu;
}

- (void)setSystemAndWindowIOS:(GHOST_SystemIOS *)sys windowIOS:(GHOST_WindowIOS *)win
{
  system = sys;
  window = win;
  text_field = nil;
  original_text = nil;
  onscreen_keyboard_active = false;
  text_field_string = nullptr;
  current_pencil_touch = nil;
  tablet_data = GHOST_TABLET_DATA_NONE;
  pencil_button_active = false;
  toolbar_enabled = true;
  toolbar = nil;
  last_tap_with_pencil = false;
  finger_tool_enabled = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone;
  orbit_display_link = nil;
  external_keyboard_connected = [GCKeyboard coalescedKeyboard] != nil;

  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(applicationWillResignActive:)
                                               name:UIApplicationWillResignActiveNotification
                                             object:nil];

  /* Register for notifications of chnanges to the onscreen keyboard. */
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(keyboardWillChange:)
                                               name:UIKeyboardWillChangeFrameNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(keyboardWillChange:)
                                               name:UIKeyboardWillShowNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(keyboardWillChange:)
                                               name:UIKeyboardWillHideNotification
                                             object:nil];

  /* Check whether we've linked the GameController framework. */
  if (&GCKeyboardDidConnectNotification != NULL) {
    /* Register for notifcations an external keyboard has been added/removed. */
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(externalKeyboardChange:)
                                                 name:GCKeyboardDidConnectNotification
                                               object:nil];

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(externalKeyboardChange:)
                                                 name:GCKeyboardDidDisconnectNotification
                                               object:nil];
  }
}

- (void)registerGestureRecognizers
{
  /** Create Gesture recognisers. */
  /* Tap gesture recognizer. */
  tap_gesture_recognizer = [[GHOSTUITapGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleTap:)];
  tap_gesture_recognizer.delegate = self;
  tap_gesture_recognizer.cancelsTouchesInView = false;
  tap_gesture_recognizer.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  [window->getView() addGestureRecognizer:tap_gesture_recognizer];

  /* Two-finger tap gesture recognizer. */
  tap2f_gesture_recognizer = [[GHOSTUITapGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleTap2F:)];
  tap2f_gesture_recognizer.delegate = self;
  tap2f_gesture_recognizer.cancelsTouchesInView = false;
  tap2f_gesture_recognizer.delaysTouchesBegan = YES;
  tap2f_gesture_recognizer.numberOfTouchesRequired = 2;
  tap2f_gesture_recognizer.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  [window->getView() addGestureRecognizer:tap2f_gesture_recognizer];

  /* Three-finger tap gesture recognizer. */
  tap3f_gesture_recognizer = [[GHOSTUITapGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleTap3F:)];
  tap3f_gesture_recognizer.delegate = self;
  tap3f_gesture_recognizer.cancelsTouchesInView = false;
  tap3f_gesture_recognizer.delaysTouchesBegan = YES;
  tap3f_gesture_recognizer.numberOfTouchesRequired = 3;
  tap3f_gesture_recognizer.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  [window->getView() addGestureRecognizer:tap3f_gesture_recognizer];

  /* Four-finger tap gesture recognizer. */
  tap4f_gesture_recognizer = [[GHOSTUITapGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleTap4F:)];
  tap4f_gesture_recognizer.delegate = self;
  tap4f_gesture_recognizer.cancelsTouchesInView = false;
  tap4f_gesture_recognizer.delaysTouchesBegan = YES;
  tap4f_gesture_recognizer.numberOfTouchesRequired = 4;
  tap4f_gesture_recognizer.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  [window->getView() addGestureRecognizer:tap4f_gesture_recognizer];

  /* Pan gesture recognizer - static UI. */
  pan_gesture_recognizer = [[GHOSTUIPanGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handlePan:)];
  pan_gesture_recognizer.delegate = self;
  pan_gesture_recognizer.cancelsTouchesInView = false;
  /* Allow scrolling only with a single finger. */
  pan_gesture_recognizer.minimumNumberOfTouches = 1;
  pan_gesture_recognizer.maximumNumberOfTouches = 1;
  pan_gesture_recognizer.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  [window->getView() addGestureRecognizer:pan_gesture_recognizer];

  /* Pan gesture recognizer - two fingers 3D UI. */
  pan2f_gesture_recognizer = [[GHOSTUIPanGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handlePan2f:)];
  pan2f_gesture_recognizer.delegate = self;
  pan2f_gesture_recognizer.cancelsTouchesInView = false;
  /* Two finger gestures only.  */
  pan2f_gesture_recognizer.minimumNumberOfTouches = 2;
  pan2f_gesture_recognizer.maximumNumberOfTouches = 2;
  pan2f_gesture_recognizer.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  [window->getView() addGestureRecognizer:pan2f_gesture_recognizer];

  /* Pinch/Zoom gesture recognizer. */
  zoom_gesture_recognizer = [[GHOSTUIPinchGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleZoom:)];
  zoom_gesture_recognizer.delegate = self;
  zoom_gesture_recognizer.cancelsTouchesInView = false;
  zoom_gesture_recognizer.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  [window->getView() addGestureRecognizer:zoom_gesture_recognizer];

  /* Twist/roll gesture recognizer. */
  rotation_gesture_recognizer = [[UIRotationGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleRotation:)];
  rotation_gesture_recognizer.delegate = self;
  rotation_gesture_recognizer.cancelsTouchesInView = false;
  rotation_gesture_recognizer.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  [window->getView() addGestureRecognizer:rotation_gesture_recognizer];

  /* Edge swipe. */
  edge_swipe_left = [[UIScreenEdgePanGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleEdgeSwipe:)];
  edge_swipe_left.edges = UIRectEdgeLeft;
  edge_swipe_left.delegate = self;
  edge_swipe_left.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  [window->getView() addGestureRecognizer:edge_swipe_left];

  edge_swipe_right = [[UIScreenEdgePanGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleEdgeSwipe:)];
  edge_swipe_right.edges = UIRectEdgeRight;
  edge_swipe_right.delegate = self;
  edge_swipe_right.allowedTouchTypes = @[ @(UITouchTypeDirect) ];
  [window->getView() addGestureRecognizer:edge_swipe_right];

  /* Apple Pencil hover recognizer. */
  hover_gesture_recognizer = [[GHOSTUIHoverGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleHover:)];
  hover_gesture_recognizer.delegate = self;
  hover_gesture_recognizer.allowedTouchTypes = @[ @(UITouchTypePencil) ];
  /* Observe both the viewport and the modal native radial overlay. */
  [self addGestureRecognizer:hover_gesture_recognizer];
  current_pencil_touch = nil;

  /**  Apple Pencil double-tap. */
  pencil_interaction = [[UIPencilInteraction alloc] init];
  pencil_interaction.delegate = self;
  [window->getView() addInteraction:pencil_interaction];
}

/* Turn the user inputs into Blender events.
 * We batch up the events rather than send them directly in the gesture
 * recognisers to ensure we don't interleave events if we detect simultaneous
 * inputs. */
- (void)generateUserInputEvents:(const UserInputEvent &)event_info
{
  if (!window || !system) {
    return;
  }
  /* Lock access to ensure all input-events are received sequentially. */
  @synchronized(self) {
    for (int i = 0; i < event_info.num_events; i++) {
      UserInputEvent::EventTypes event_type = event_info.event_list[i];
      IOS_INPUT_LOG(@"%d-%@ %f,%f",
                    i,
                    event_info.getEventTypeDesc(event_type),
                    event_info.location.x,
                    event_info.location.y);

      switch (event_type) {
        case UserInputEvent::EventTypes::CURSOR_MOVE:
          window->updatePointerPosition(event_info.location.x, event_info.location.y);
          system->pushEvent(
              std::make_unique<GHOST_EventCursor>(system->getMilliSeconds(),
                                    GHOST_kEventCursorMove,
                                    window,
                                    event_info.location.x,
                                    event_info.location.y,
                                    tablet_data));
          break;
        case UserInputEvent::EventTypes::ORBIT_GESTURE:
          system->pushEvent(
              std::make_unique<GHOST_EventTrackpad>(system->getMilliSeconds(),
                                      window,
                                      GHOST_kTrackpadEventRotate,
                                      event_info.location.x,
                                      event_info.location.y,
                                      event_info.translation.x,
                                      event_info.translation.y,
                                      false,
                                      1,
                                      event_info.gesture_id));
          break;
        case UserInputEvent::EventTypes::PAN_GESTURE:
          system->pushEvent(std::make_unique<GHOST_EventTrackpad>(system->getMilliSeconds(),
                                                                  window,
                                                                  GHOST_kTrackpadEventScroll,
                                                                  event_info.location.x,
                                                                  event_info.location.y,
                                                                  event_info.translation.x,
                                                                  event_info.translation.y,
                                                                  false,
                                                                  1));
          break;
        case UserInputEvent::EventTypes::PAN_GESTURE_TWO_FINGERS:
          system->pushEvent(std::make_unique<GHOST_EventTrackpad>(system->getMilliSeconds(),
                                                                  window,
                                                                  GHOST_kTrackpadEventScroll,
                                                                  event_info.location.x,
                                                                  event_info.location.y,
                                                                  event_info.translation.x,
                                                                  event_info.translation.y,
                                                                  true,
                                                                  2));
          break;
        case UserInputEvent::EventTypes::LEFT_BUTTON_DOWN:
          system->pushEvent(std::make_unique<GHOST_EventButton>(system->getMilliSeconds(),
                                                                GHOST_kEventButtonDown,
                                                                window,
                                                                GHOST_kButtonMaskLeft,
                                                                tablet_data));
          break;
        case UserInputEvent::EventTypes::LEFT_BUTTON_UP:
          system->pushEvent(std::make_unique<GHOST_EventButton>(system->getMilliSeconds(),
                                                                GHOST_kEventButtonUp,
                                                                window,
                                                                GHOST_kButtonMaskLeft,
                                                                tablet_data));
          break;
        case UserInputEvent::EventTypes::PINCH_GESTURE:
          system->pushEvent(std::make_unique<GHOST_EventTrackpad>(system->getMilliSeconds(),
                                                                  window,
                                                                  GHOST_kTrackpadEventMagnify,
                                                                  event_info.location.x,
                                                                  event_info.location.y,
                                                                  event_info.distance,
                                                                  0,
                                                                  false,
                                                                  2));
          break;
        case UserInputEvent::EventTypes::CANCEL_TOOL:
          system->pushEvent(std::make_unique<GHOST_EventKey>(system->getMilliSeconds(),
                                               GHOST_kEventKeyDown,
                                               window,
                                               GHOST_kKeyEsc,
                                               false));
          system->pushEvent(std::make_unique<GHOST_EventKey>(system->getMilliSeconds(),
                                               GHOST_kEventKeyUp,
                                               window,
                                               GHOST_kKeyEsc,
                                               false));
          break;
        default:
          GHOST_ASSERT(FALSE, "GHOST_SystemIOS::generateUserInputEvents unsupported event type");
      }
    }
  }
}

/* Pan, pinch, and twist are independent parts of the same two-finger gesture. */
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer
    shouldRecognizeSimultaneouslyWithGestureRecognizer:
        (UIGestureRecognizer *)otherGestureRecognizer
{
  if ([gestureRecognizer isKindOfClass:UIHoverGestureRecognizer.class] &&
      [otherGestureRecognizer isKindOfClass:UIHoverGestureRecognizer.class]) {
    return YES;
  }
  const BOOL is_navigation = gestureRecognizer == pan2f_gesture_recognizer ||
                             gestureRecognizer == zoom_gesture_recognizer ||
                             gestureRecognizer == rotation_gesture_recognizer;
  const BOOL other_is_navigation = otherGestureRecognizer == pan2f_gesture_recognizer ||
                                   otherGestureRecognizer == zoom_gesture_recognizer ||
                                   otherGestureRecognizer == rotation_gesture_recognizer;
  return gestureRecognizer != otherGestureRecognizer && is_navigation && other_is_navigation;
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer shouldReceiveTouch:(UITouch *)touch
{
  if (finger_tool_enabled && (gestureRecognizer == pan_gesture_recognizer ||
                              gestureRecognizer == tap_gesture_recognizer)) {
    return NO;
  }
  if (gestureRecognizer == hover_gesture_recognizer && pencil_context_menu.active) {
    return YES;
  }
  if (pencil_context_menu.active && gestureRecognizer != hover_gesture_recognizer) {
    return NO;
  }
  if (gestureRecognizer == tap_gesture_recognizer ||
      gestureRecognizer == tap2f_gesture_recognizer ||
      gestureRecognizer == tap3f_gesture_recognizer ||
      gestureRecognizer == tap4f_gesture_recognizer ||
      gestureRecognizer == pan_gesture_recognizer ||
      gestureRecognizer == pan2f_gesture_recognizer ||
      gestureRecognizer == zoom_gesture_recognizer ||
      gestureRecognizer == rotation_gesture_recognizer || gestureRecognizer == edge_swipe_left ||
      gestureRecognizer == edge_swipe_right || gestureRecognizer == hover_gesture_recognizer)
  {
    return [self touchBeganInBlenderViewport:touch];
  }
  return YES;
}

- (BOOL)touchBeganInBlenderViewport:(UITouch *)touch
{
  return touch.view == window->getView() ||
         (touch.type == UITouchTypePencil && [self isBlenderPencilInputView:touch.view]);
}

- (BOOL)isBlenderPencilInputView:(UIView *)view
{
  return view == window->getView() ||
         [view.accessibilityIdentifier isEqualToString:@"blender.viewport.navigation"];
}

- (void)updateTabletDataFromPencilTouch:(UITouch *)touch
{
  if (touch.type == UITouchTypeDirect) {
    tablet_data = GHOST_TABLET_DATA_NONE;
    return;
  }
  /* Preserve absolute Pencil coordinates on navigation gizmos too. Click versus
   * drag is decided by movement, not by disguising a Pencil as a relative mouse. */
  tablet_data.Active = GHOST_kTabletModeStylus;

  if (touch.maximumPossibleForce > 0.0) {
    tablet_data.Pressure = touch.force / touch.maximumPossibleForce;
  }
  else {
    tablet_data.Pressure = 1.0f;
  }

  CGFloat azimuthAngle = [touch azimuthAngleInView:window->getView()];
  CGFloat altitudeAngle = [touch altitudeAngle];

  /* Calculate the maximum possible tilt (1.0) when altitude is 0. */
  CGFloat maxTilt = cos(0);

  /* Convert to x and y tilt - range -1.0 (left) to +1.0 (right). */
  tablet_data.Xtilt = sin(azimuthAngle) * cos(altitudeAngle) / maxTilt;
  tablet_data.Ytilt = -cos(azimuthAngle) * cos(altitudeAngle) / maxTilt;
  IOS_INPUT_LOG(
      @"TABLET: X:%f,Y:%f,P:%f", tablet_data.Xtilt, tablet_data.Ytilt, tablet_data.Pressure);
}

- (CGPoint)getScaledPencilPoint:(UITouch *)touch
{
  CGPoint touch_point = [touch preciseLocationInView:window->getView()];
  return window->scalePointToWindow(touch_point);
}

- (void)sendPencilCursorEvent:(UITouch *)touch
{
  [self updateTabletDataFromPencilTouch:touch];
  CGPoint touch_point = [self getScaledPencilPoint:touch];
  UserInputEvent event_info(&touch_point, nullptr, nullptr, true);
  event_info.add_event(UserInputEvent::EventTypes::CURSOR_MOVE);
  [self generateUserInputEvents:event_info];
}

- (void)sendPencilButtonEvent:(UITouch *)touch isDown:(BOOL)isDown
{
  [self updateTabletDataFromPencilTouch:touch];
  if (!isDown) {
    tablet_data.Pressure = 0.0f;
  }
  CGPoint touch_point = [self getScaledPencilPoint:touch];
  last_tap_with_pencil = true;

  UserInputEvent event_info(&touch_point, nullptr, nullptr, true);
  event_info.add_event(UserInputEvent::EventTypes::CURSOR_MOVE);
  event_info.add_event(isDown ? UserInputEvent::EventTypes::LEFT_BUTTON_DOWN :
                                UserInputEvent::EventTypes::LEFT_BUTTON_UP);
  [self generateUserInputEvents:event_info];
}

- (void)cancelPencilStroke
{
  WM_apple_ios_ui_pencil_hover_set(false, 0, 0);
  if (pencil_button_active) {
    /* Cancel the modal tool before releasing, so interruption does not commit it.
     * Do not dereference UITouch here: UIKit may already have retired the touch. */
    tablet_data.Pressure = 0.0f;
    UserInputEvent event_info(nullptr, nullptr, nullptr, true);
    event_info.add_event(UserInputEvent::EventTypes::CANCEL_TOOL);
    event_info.add_event(UserInputEvent::EventTypes::LEFT_BUTTON_UP);
    [self generateUserInputEvents:event_info];
  }
  pencil_button_active = false;
  pencil_navigation_pending = false;
  current_pencil_touch = nil;
  tablet_data = GHOST_TABLET_DATA_NONE;
}

/* Observe Pencil input before UIKit's responder chain can consume the viewport touches. */
- (void)sendEvent:(UIEvent *)event
{
  if (!window) {
    [super sendEvent:event];
    return;
  }
  if (event.type == UIEventTypeTouches) {
    NSUInteger fingers = 0;
    for (UITouch *touch in event.allTouches) {
      if (touch.type == UITouchTypeDirect && touch.phase != UITouchPhaseEnded &&
          touch.phase != UITouchPhaseCancelled && [self touchBeganInBlenderViewport:touch]) {
        fingers++;
      }
    }
    /* Hand the sequence to two-finger navigation without committing a partial tool drag. */
    if (fingers > 1 && current_pencil_touch.type == UITouchTypeDirect) {
      [self cancelPencilStroke];
    }
    for (UITouch *touch in event.allTouches) {
      if (touch.phase == UITouchPhaseBegan) {
        WM_apple_ios_ui_pencil_hover_set(false, 0, 0);
        [self stopOrbitMomentum];
      }
      if (touch.type == UITouchTypeDirect && pencil_context_menu.active) {
        if (touch.phase == UITouchPhaseBegan) {
          [pencil_context_menu cancel];
        }
        continue;
      }
      if (touch.type != UITouchTypePencil &&
          !(finger_tool_enabled && touch.type == UITouchTypeDirect && fingers <= 1)) {
        continue;
      }
      if ((pencil_context_menu.active || touch.view == pencil_context_menu ||
           [self touchBeganInBlenderViewport:touch]) &&
          [pencil_context_menu handlePencilTouch:touch])
      {
        continue;
      }

      switch (touch.phase) {
        case UITouchPhaseBegan:
          if (touch == current_pencil_touch || ![self touchBeganInBlenderViewport:touch]) {
            break;
          }
          [self cancelPencilStroke];
          WM_apple_ios_ui_navigation_hover_set(false);
          current_pencil_touch = touch;
          /* Only the navigation hotspot waits to distinguish a tap from a hold.
           * Viewport tools still receive Pencil-down immediately. */
          pencil_navigation_pending = [touch.view.accessibilityIdentifier
              isEqualToString:@"blender.viewport.navigation"];
          pencil_navigation_start = [touch preciseLocationInView:window->getView()];
          pencil_button_active = !pencil_navigation_pending;
          if (pencil_button_active) {
            [self sendPencilButtonEvent:touch isDown:YES];
          }
          break;
        case UITouchPhaseMoved:
          if (touch == current_pencil_touch && pencil_navigation_pending) {
            CGPoint point = [touch preciseLocationInView:window->getView()];
            if (hypot(point.x - pencil_navigation_start.x,
                      point.y - pencil_navigation_start.y) > 10.0) {
              pencil_navigation_pending = false;
              pencil_button_active = true;
              [self updateTabletDataFromPencilTouch:touch];
              CGPoint start = window->scalePointToWindow(pencil_navigation_start);
              UserInputEvent down(&start, nullptr, nullptr, true);
              down.add_event(UserInputEvent::EventTypes::CURSOR_MOVE);
              down.add_event(UserInputEvent::EventTypes::LEFT_BUTTON_DOWN);
              [self generateUserInputEvents:down];
            }
          }
          if (touch == current_pencil_touch && pencil_button_active) {
            NSArray<UITouch *> *samples = [event coalescedTouchesForTouch:touch];
            if (samples.count == 0) {
              samples = @[ touch ];
            }
            for (UITouch *sample in samples) {
              [self sendPencilCursorEvent:sample];
            }
          }
          break;
        case UITouchPhaseEnded:
          if (touch == current_pencil_touch) {
            if (pencil_navigation_pending) {
              [self sendPencilButtonEvent:touch isDown:YES];
              pencil_button_active = true;
            }
            if (pencil_button_active) {
              [self sendPencilButtonEvent:touch isDown:NO];
            }
            pencil_button_active = false;
            pencil_navigation_pending = false;
            current_pencil_touch = nil;
            tablet_data = GHOST_TABLET_DATA_NONE;
          }
          break;
        case UITouchPhaseCancelled:
          if (touch == current_pencil_touch) {
            [self cancelPencilStroke];
          }
          break;
        default:
          break;
      }
    }
  }
  [super sendEvent:event];
}

- (void)handleNavigationTap:(UITapGestureRecognizer *)gesture
{
  if (!window || gesture.state != UIGestureRecognizerStateEnded) {
    return;
  }
  CGPoint location = [gesture locationInView:window->getView()];
  CGPoint point = window->scalePointToWindow(location);
  tablet_data = GHOST_TABLET_DATA_NONE;
  /* Axis picking needs an absolute event, including a finger tap without hover. */
  tablet_data.Active = GHOST_kTabletModeStylus;
  tablet_data.Pressure = 1.0f;
  UserInputEvent event(&point, nullptr, nullptr, true);
  event.add_event(UserInputEvent::EventTypes::CURSOR_MOVE);
  event.add_event(UserInputEvent::EventTypes::LEFT_BUTTON_DOWN);
  [self generateUserInputEvents:event];
  tablet_data.Pressure = 0.0f;
  UserInputEvent up(&point, nullptr, nullptr, true);
  up.add_event(UserInputEvent::EventTypes::LEFT_BUTTON_UP);
  [self generateUserInputEvents:up];
  tablet_data = GHOST_TABLET_DATA_NONE;
}

- (void)handleTap:(GHOSTUITapGestureRecognizer *)sender
{
  if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone &&
      !finger_tool_enabled && sender.state == UIGestureRecognizerStateEnded) {
    CGPoint point = [sender getScaledTouchPoint:window];
    tablet_data = GHOST_TABLET_DATA_NONE;
    UserInputEvent event_info(&point, nullptr, nullptr, false);
    event_info.add_event(UserInputEvent::EventTypes::CURSOR_MOVE);
    event_info.add_event(UserInputEvent::EventTypes::LEFT_BUTTON_DOWN);
    event_info.add_event(UserInputEvent::EventTypes::LEFT_BUTTON_UP);
    [self generateUserInputEvents:event_info];
  }
  /* Finger taps are reserved for native iPad UI affordances and viewport gestures. Selection and
   * active-tool execution are handled by Apple Pencil instead. */
  UNUSED_VARS(sender);
}

- (void)handleTap2F:(GHOSTUITapGestureRecognizer *)sender
{
  if (sender.state != UIGestureRecognizerStateEnded) {
    return;
  }

  CGPoint touch_point = [sender locationInView:window->getView()];
  CGFloat scale = [window->getView() contentScaleFactor];
  touch_point.x *= scale;
  touch_point.y *= scale;

  system->pushEvent(
      std::make_unique<GHOST_Event>(system->getMilliSeconds(), GHOST_kEventTwoFingerTap, window));
}

- (void)handleTap3F:(GHOSTUITapGestureRecognizer *)sender
{
  if (sender.state != UIGestureRecognizerStateEnded) {
    return;
  }

  CGPoint touch_point = [sender locationInView:window->getView()];
  CGFloat scale = [window->getView() contentScaleFactor];
  touch_point.x *= scale;
  touch_point.y *= scale;

  system->pushEvent(std::make_unique<GHOST_Event>(
      system->getMilliSeconds(), GHOST_kEventThreeFingerTap, window));
}

- (void)handleTap4F:(GHOSTUITapGestureRecognizer *)sender
{
  if (sender.state != UIGestureRecognizerStateEnded) {
    return;
  }

  CGPoint touch_point = [sender locationInView:window->getView()];
  CGFloat scale = [window->getView() contentScaleFactor];
  touch_point.x *= scale;
  touch_point.y *= scale;

  system->pushEvent(
      std::make_unique<GHOST_Event>(system->getMilliSeconds(), GHOST_kEventFourFingerTap, window));
}

- (void)handlePan:(GHOSTUIPanGestureRecognizer *)sender
{
  CGPoint touch_point = [sender getScaledTouchPoint:window];
  CGPoint translation = [sender getScaledTranslation:window];
  UserInputEvent event_info(&touch_point, nullptr, nullptr, false);

  if (sender.state == UIGestureRecognizerStateBegan ||
      sender.state == UIGestureRecognizerStateChanged)
  {
    if (sender.state == UIGestureRecognizerStateBegan) {
      [self stopOrbitMomentum];
      /* UIKit delivers gestures on the main thread. IDs are unique across windows, and
       * remain attached to queued events even if the next drag has already begun. */
      static uint64_t next_orbit_gesture_id = 0;
      orbit_gesture_id = ++next_orbit_gesture_id;
      [sender setCachedTranslation:translation];
    }

    /* Calculate translation change since last begin/change event */
    CGPoint relative_translation = [sender getRelativeTranslation:translation];
    /* Update cached translation */
    [sender setCachedTranslation:translation];

    /* A single finger drag navigates the viewport. Apple Pencil remains the active-tool input. */
    if (!CGPointEqualToPoint(relative_translation, CGPointMake(0.0f, 0.0f))) {
      event_info.translation = CGPointMake(-relative_translation.x, -relative_translation.y);
      event_info.add_event(UserInputEvent::EventTypes::ORBIT_GESTURE);
    }
  }
  else if (sender.state == UIGestureRecognizerStateEnded ||
           sender.state == UIGestureRecognizerStateCancelled ||
           sender.state == UIGestureRecognizerStateFailed)
  {
    [sender setCachedTranslation:CGPointMake(0.0f, 0.0f)];
    if (sender.state == UIGestureRecognizerStateEnded && !pencil_button_active) {
      CGPoint velocity = [sender velocityInView:window->getView()];
      const CGFloat speed = hypot(velocity.x, velocity.y);
      if (speed >= 80.0) {
        const CGFloat limit = MIN(1.0, 3000.0 / speed);
        orbit_velocity = CGPointMake(velocity.x * limit, velocity.y * limit);
        orbit_location = touch_point;
        orbit_remainder = CGPointZero;
        orbit_timestamp = CACurrentMediaTime();
        orbit_display_link = [CADisplayLink displayLinkWithTarget:self
                                                       selector:@selector(updateOrbitMomentum:)];
        [orbit_display_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
      }
    }
  }

  if (event_info.num_events > 0) {
    event_info.gesture_id = orbit_gesture_id;
    [self generateUserInputEvents:event_info];
  }
}

- (void)stopOrbitMomentum
{
  [orbit_display_link invalidate];
  orbit_display_link = nil;
}

- (void)applicationWillResignActive:(NSNotification *)notification
{
  UNUSED_VARS(notification);
  [self stopOrbitMomentum];
  [self cancelPencilStroke];
  [pencil_context_menu cancel];
  WM_apple_ios_ui_navigation_hover_set(false);
}

- (void)updateOrbitMomentum:(CADisplayLink *)display_link
{
  if (UIApplication.sharedApplication.applicationState != UIApplicationStateActive ||
      pencil_button_active)
  {
    [self stopOrbitMomentum];
    return;
  }

  const CFTimeInterval elapsed = display_link.timestamp - orbit_timestamp;
  if (elapsed <= 0.0) {
    return;
  }
  orbit_timestamp = display_link.timestamp;
  /* Integrate UIKit's exponential deceleration independently of the display refresh rate. */
  const double decay_rate = -1000.0 * log(UIScrollViewDecelerationRateNormal);
  const double decay = exp(-decay_rate * elapsed);
  const double distance_scale = (1.0 - decay) / decay_rate;
  CGPoint translation = CGPointMake(-orbit_velocity.x * distance_scale,
                                    -orbit_velocity.y * distance_scale);
  translation = window->scalePointToWindow(translation);
  translation.x += orbit_remainder.x;
  translation.y += orbit_remainder.y;
  CGPoint pixels = CGPointMake(trunc(translation.x), trunc(translation.y));
  orbit_remainder = CGPointMake(translation.x - pixels.x, translation.y - pixels.y);
  orbit_velocity.x *= decay;
  orbit_velocity.y *= decay;

  if (!CGPointEqualToPoint(pixels, CGPointZero)) {
    UserInputEvent event_info(&orbit_location, &pixels, nullptr, false);
    event_info.gesture_id = orbit_gesture_id;
    event_info.add_event(UserInputEvent::EventTypes::ORBIT_GESTURE);
    [self generateUserInputEvents:event_info];
  }
  if (hypot(orbit_velocity.x, orbit_velocity.y) < 8.0) {
    [self stopOrbitMomentum];
  }
}

- (void)handlePan2f:(GHOSTUIPanGestureRecognizer *)sender
{
  /* Translation can be non-zero on begin event */
  if (sender.state == UIGestureRecognizerStateBegan ||
      sender.state == UIGestureRecognizerStateChanged)
  {
    CGPoint translation = [sender getScaledTranslation:window];

    /* Calculate translation relative to previous cached value. */
    CGPoint relative_translation = [sender getRelativeTranslation:translation];

    /* Cache new translation. */
    [sender setCachedTranslation:translation];

    /* Generate pan event if translation is non zero. */
    if (!CGPointEqualToPoint(relative_translation, CGPointMake(0.0f, 0.0f))) {
      CGPoint touch_point = [sender getScaledTouchPoint:window];
      bool pencil_pan = current_pencil_touch ? true : false;
      UserInputEvent event_info(&touch_point, &relative_translation, nullptr, pencil_pan);
      event_info.add_event(UserInputEvent::EventTypes::PAN_GESTURE_TWO_FINGERS);
      [self generateUserInputEvents:event_info];
    }
  }
  else if (sender.state == UIGestureRecognizerStateEnded ||
           sender.state == UIGestureRecognizerStateCancelled ||
           sender.state == UIGestureRecognizerStateFailed)
  {
    /* Set translation back to zero. */
    [sender setCachedTranslation:CGPointMake(0.0f, 0.0f)];
  }
}

- (void)handleEdgeSwipe:(UIScreenEdgePanGestureRecognizer *)gesture
{
  if (gesture.state != UIGestureRecognizerStateEnded) {
    return;
  }

  UIView *view = window->getView();
  CGPoint location = [gesture locationInView:view];
  CGSize viewSize = view.bounds.size;

  GHOST_TTouchEventSubTypes ghostEventType;

  if (gesture.edges == UIRectEdgeLeft) {
    ghostEventType = GHOST_kTouchEventEdgeSwipeInLeft;
  }
  else if (gesture.edges == UIRectEdgeRight) {
    ghostEventType = GHOST_kTouchEventEdgeSwipeInRight;
  }
  else {
    /* For now only handle left/right. */
    return;
  }

  system->pushEvent(std::make_unique<GHOST_EventTouch>(
      system->getMilliSeconds(), window, ghostEventType, location.x, location.y));
}

- (void)handleHover:(GHOSTUIHoverGestureRecognizer *)sender
{
  if (!pencil_context_menu.active &&
      (sender.state == UIGestureRecognizerStateBegan ||
       sender.state == UIGestureRecognizerStateChanged) &&
      ![self isBlenderPencilInputView:[self hitTest:[sender locationInView:(UIView *)self] withEvent:nil]])
  {
    WM_apple_ios_ui_navigation_hover_set(false);
    WM_apple_ios_ui_pencil_hover_set(false, 0, 0);
    return;
  }
  if ([pencil_context_menu handlePencilHover:sender]) {
    WM_apple_ios_ui_navigation_hover_set(false);
    WM_apple_ios_ui_pencil_hover_set(false, 0, 0);
    return;
  }
  if (pencil_button_active) {
    WM_apple_ios_ui_pencil_hover_set(false, 0, 0);
    return;
  }
  if (sender.state == UIGestureRecognizerStateBegan ||
      sender.state == UIGestureRecognizerStateChanged)
  {
    WM_apple_ios_ui_navigation_hover_set(true);
    /* Tablet needs to be set to stylus mode because we need
     * wmTabletData.is_motion_absolute set to true. */
    tablet_data.Active = GHOST_kTabletModeStylus;
    tablet_data.Pressure = 0.0f;
    CGPoint hover_point = [sender getScaledTouchPoint:window];
    UIView *hit = [self hitTest:[sender locationInView:self] withEvent:nil];
    WM_apple_ios_ui_pencil_hover_set(
        ![hit.accessibilityIdentifier isEqualToString:@"blender.viewport.navigation"],
        hover_point.x,
        hover_point.y);
    /* Add cursor move event. */
    UserInputEvent event_info(&hover_point, nullptr, nullptr, true);
    event_info.add_event(UserInputEvent::EventTypes::CURSOR_MOVE);
    [self generateUserInputEvents:event_info];
  }
  else if (sender.state == UIGestureRecognizerStateEnded ||
           sender.state == UIGestureRecognizerStateCancelled ||
           sender.state == UIGestureRecognizerStateFailed)
  {
    WM_apple_ios_ui_navigation_hover_set(false);
    tablet_data = GHOST_TABLET_DATA_NONE;
    WM_apple_ios_ui_pencil_hover_set(false, 0, 0);
  }
}

- (void)handleZoom:(GHOSTUIPinchGestureRecognizer *)sender
{
  /* Ignore any calls where don't have two touches to work with. */
  if ([sender numberOfTouches] < 2) {
    return;
  }

  /* Pinch/Zoom gestures */
  if (sender.state == UIGestureRecognizerStateBegan) {
    /* Set an initial distance value. */
    CGFloat point_distance = [sender getScaledDistance:window];
    [sender setCachedDistance:point_distance];
  }
  else if (sender.state == UIGestureRecognizerStateChanged) {

    /* Calculate change in distance since last event */
    CGFloat point_distance = [sender getScaledDistance:window];
    CGFloat relative_dist = point_distance - [sender getCachedDistance];

    /* Updated cached distance. */
    [sender setCachedDistance:point_distance];

    /* Send pinch/zoom event. */
    if (fabs(relative_dist) > 0.0) {
      /* Calculate midpoint between the two touch points. */
      CGPoint midPoint = [sender getPinchMidpoint:window];

      UserInputEvent event_info(&midPoint, nullptr, &relative_dist, false);
      event_info.add_event(UserInputEvent::EventTypes::PINCH_GESTURE);
      [self generateUserInputEvents:event_info];
    }
  }
  /* Nothing to do here. */
  else if (sender.state == UIGestureRecognizerStateEnded ||
           sender.state == UIGestureRecognizerStateCancelled ||
           sender.state == UIGestureRecognizerStateFailed)
  {
  }
}

- (void)handleRotation:(UIRotationGestureRecognizer *)sender
{
  if (sender.state == UIGestureRecognizerStateBegan ||
      sender.state == UIGestureRecognizerStateChanged)
  {
    const CGFloat relative_rotation = sender.rotation;
    sender.rotation = 0.0;

    if (fabs(relative_rotation) > 0.0) {
      WM_apple_ios_ui_view_roll_enqueue(float(relative_rotation));
    }
  }
  else if (sender.state == UIGestureRecognizerStateEnded ||
           sender.state == UIGestureRecognizerStateCancelled ||
           sender.state == UIGestureRecognizerStateFailed)
  {
    sender.rotation = 0.0;
  }
}

- (void)togglePencilMenuForInteraction:(UIPencilInteraction *)interaction
                            hoverPose:(UIPencilHoverPose *)pose
{
  NSValue *location = nil;
  if (pose) {
    CGPoint point = [interaction.view convertPoint:pose.location toView:self];
    if (!pencil_context_menu.active &&
        ![self isBlenderPencilInputView:[self hitTest:point withEvent:nil]])
    {
      return;
    }
    location = [NSValue valueWithCGPoint:[self convertPoint:point toView:pencil_context_menu]];
  }
  else if (current_pencil_touch) {
    location = [NSValue valueWithCGPoint:
        [current_pencil_touch preciseLocationInView:pencil_context_menu]];
  }
  WM_apple_ios_ui_navigation_hover_set(false);
  [self stopOrbitMomentum];
  [self cancelPencilStroke];
  [pencil_context_menu toggleWithPencilLocation:location];
}

- (void)pencilInteraction:(UIPencilInteraction *)interaction didReceiveTap:(UIPencilInteractionTap *)tap
{
  const UIPencilPreferredAction action = UIPencilInteraction.preferredTapAction;
  if (action != UIPencilPreferredActionIgnore && action != UIPencilPreferredActionRunSystemShortcut) {
    [self togglePencilMenuForInteraction:interaction hoverPose:tap.hoverPose];
  }
}

- (void)pencilInteraction:(UIPencilInteraction *)interaction
      didReceiveSqueeze:(UIPencilInteractionSqueeze *)squeeze
{
  const UIPencilPreferredAction action = UIPencilInteraction.preferredSqueezeAction;
  if (squeeze.phase == UIPencilInteractionPhaseEnded &&
      action != UIPencilPreferredActionIgnore && action != UIPencilPreferredActionRunSystemShortcut)
  {
    [self togglePencilMenuForInteraction:interaction hoverPose:squeeze.hoverPose];
  }
}

- (void)beginFrame
{
}

- (void)endFrame
{
}

- (void)initToolbar
{
  /* This gets the current view size */
  UIView *ui_view = window->getView();
  CGSize frame_size = [ui_view sizeThatFits:CGSizeMake(0.0f, 0.0f)];
  /* Create a toolbar the width of the screen. */
  toolbar = [[UIToolbar alloc] initWithFrame:CGRectMake(0, 0, frame_size.width, 44)];
  toolbar.barStyle = UIBarStyleDefault;
  toolbar.translucent = true;
  /* IOS_FIXME - Despite following Apple guidelines this toolbar still
   * appears to apparently violate the view constraints. It displays fine
   * but generates a lot of warning output to the console. */
  toolbar.autoresizingMask = UIViewAutoresizingFlexibleWidth;
  toolbar.translatesAutoresizingMaskIntoConstraints = NO;
  [toolbar sizeToFit];

  toolbar_tip_item = [[UIBarButtonItem alloc] initWithTitle:@""
                                                      style:UIBarButtonItemStylePlain
                                                     target:nil
                                                     action:nil];

  toolbar_live_text_item = [[UIBarButtonItem alloc] initWithTitle:@""
                                                            style:UIBarButtonItemStylePlain
                                                           target:nil
                                                           action:nil];

  toolbar_done_editing_item = [[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                           target:nil
                           action:@selector(handleDoneButton)];

  toolbar_cancel_editing_item = [[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
                           target:nil
                           action:@selector(handleCancelButton)];

  /* Prevents editing of tip and live text fields. */
  toolbar_tip_item.enabled = NO;
  toolbar_live_text_item.enabled = NO;
  toolbar_live_text_item.tintColor = UIColor.blackColor;

  /* Set the live text to a fixed width. */
  /* IOS_FIXME - should this be set dynamically? Need to move out of init if so. */
  toolbar_live_text_item.width = 150.0f;

  toolbar.items = @[
    toolbar_tip_item,
    toolbar_live_text_item,
    toolbar_done_editing_item,
    toolbar_cancel_editing_item
  ];
}

- (void)generateKeyboardReturnEvent
{
  /*
   Only push the event back if the keyboard is active otherwise we may generate new
   spurious events.
   */
  if (onscreen_keyboard_active) {
    /*
     This event should cause ui_textedit_end() to be called which will
     hide the keyboard.
     */
    system->pushEvent(std::make_unique<GHOST_EventKey>(
        system->getMilliSeconds(), GHOST_kEventKeyDown, window, GHOST_kKeyEnter, false, nullptr));
  }
  else {
    IOS_INPUT_LOG(@"Ignoring handleKeyboardReturn %@", text_field.text);
  }
}

- (void)handleKeyboardReturn:(UITextField *)text_field
{
  @synchronized(self) {
    IOS_INPUT_LOG(@"handleKeyboardReturn %@", text_field.text);
    [self generateKeyboardReturnEvent];
  }
}

- (void)handleKeyboardEditChange:(UITextField *)text_field
{
  @synchronized(self) {

    /* Update the text in the tool bar as the edits arrive. */
    if (toolbar_live_text_item) {
      toolbar_live_text_item.title = text_field.text;
      /* Force toolbar to update */
      [toolbar setNeedsLayout];
      [toolbar layoutIfNeeded];
    }
    IOS_INPUT_LOG(@"Keyboard Edit change detected %@", text_field.text);

    /* IOS_FIXME - Enabling this will propogate text changes back into the Blender text field
     as they happen. Since pushing back individual key presses appears to be difficult this
     might be the best we can do. However this currently causes a segmentation fault if you delete
     text as the Blender-side string ends up being NULL in some cases. */
    bool push_edits_back_to_blender = false;

    if (push_edits_back_to_blender) {
      system->pushEvent(std::make_unique<GHOST_EventKey>(system->getMilliSeconds(),
                                                         GHOST_kEventKeyDown,
                                                         window,
                                                         GHOST_kKeyTextEdit,
                                                         false,
                                                         nullptr));
    }
  }
}

- (void)handleKeyboardEditBegin:(UITextField *)text_field
{
  @synchronized(self) {
    IOS_INPUT_LOG(@"Keyboard Edit begin detected %@", text_field.text);
  }
}

- (void)handleKeyboardEditEnd:(UITextField *)text_field
{
  @synchronized(self) {
    /*
     This can get called when the keyboard is minimised
     so send a return keypress to emulate effective end
     of editing. Otherwise Blender's focus will remain
     on the text field.
     */
    IOS_INPUT_LOG(@"Keyboard Edit end detected %@", text_field.text);
    [self generateKeyboardReturnEvent];
  }
}

- (void)handleDoneButton
{
  IOS_INPUT_LOG(@"Keyboard Done button press detected %@", text_field.text);
  [self generateKeyboardReturnEvent];
}

- (void)handleCancelButton
{
  IOS_INPUT_LOG(@"Keyboard Cancel button press detected %@", text_field.text);
  /* Restore the original text and return */
  text_field.text = original_text;
  [self generateKeyboardReturnEvent];
}

/*
 * Add a text field so we can handle input from a popup keyboard and
 * attach it to our root window.
 */
- (void)initUITextField
{
  /* Initialise it if we have not already done so. */
  if (!text_field) {
    text_field = [[UITextField alloc] init];

    text_field.contentScaleFactor = window->getWindowScaleFactor();

    if (toolbar_enabled) {
      [self initToolbar];
      text_field.inputAccessoryView = toolbar;
    }

    [window->rootWindow addSubview:text_field];

    /* Add a handler for when 'return' is pressed on keyboard. */
    [text_field addTarget:self
                   action:@selector(handleKeyboardReturn:)
         forControlEvents:UIControlEventEditingDidEndOnExit];

    /* Add a handler for when the text field changes. */
    [text_field addTarget:self
                   action:@selector(handleKeyboardEditChange:)
         forControlEvents:UIControlEventEditingChanged];

    /* Add a handler for when user edits a text field. */
    [text_field addTarget:self
                   action:@selector(handleKeyboardEditBegin:)
         forControlEvents:UIControlEventEditingDidBegin];

    /* Add a handler for when user finishes editing a text field. */
    [text_field addTarget:self
                   action:@selector(handleKeyboardEditEnd:)
         forControlEvents:UIControlEventEditingDidEnd];
  }
}

- (void)convertWindowCoordToDisplayCoordWithWindow:(int)windowX
                                           windowY:(int)windowY
                                          displayX:(double *)displayX
                                          displayY:(double *)displayY
                                             flipY:(BOOL)flipY
{
  float pixelScale = window->getWindowScaleFactor();
  CGSize logicalWindowSize = window->getLogicalWindowSize();

  *displayX = (double)windowX / pixelScale;
  *displayY = (double)windowY / pixelScale;

  if (flipY) {
    *displayY = logicalWindowSize.height - *displayY;
  }
}

- (UITextField *)getUITextField
{
  return text_field;
}

- (void)setupKeyboard:(const GHOST_KeyboardProperties &)keyboard_properties
{
  /* Initialise it if we have not already done so */
  if (!text_field) {
    [self initUITextField];
  }

  /* Save this set of keyboard properties */
  current_keyboard_properties = keyboard_properties;

  /* Convert the text box coords to display coords */
  CGRect displayRect;
  [self convertWindowCoordToDisplayCoordWithWindow:keyboard_properties.text_box_origin[0]
                                           windowY:keyboard_properties.text_box_origin[1]
                                          displayX:&displayRect.origin.x
                                          displayY:&displayRect.origin.y
                                             flipY:true];

  [self convertWindowCoordToDisplayCoordWithWindow:keyboard_properties.text_box_size[0]
                                           windowY:keyboard_properties.text_box_size[1]
                                          displayX:&displayRect.size.width
                                          displayY:&displayRect.size.height
                                             flipY:false];

  /* Where to display the text on-screen. */
  text_field.frame = displayRect;

  /* Initialise text with existing string. */
  text_field.text = keyboard_properties.text_string ?
                        [NSString stringWithUTF8String:keyboard_properties.text_string] :
                        @"";
  /* Take a copy of the string so we can restore it if neccessary */
  original_text = keyboard_properties.text_string ?
                      [NSString stringWithUTF8String:keyboard_properties.text_string] :
                      @"";

  /* Set keyboard type and text alignment.
   * NOTE - the keyboard type is only honoured if using an Apple
   * pencil or if the keyboard is floating.
   * Otherwise it will just be the default full screen type. */
  switch (keyboard_properties.keyboard_type) {
    case GHOST_KeyboardProperties::ascii_keyboard_type: {
      text_field.keyboardType = UIKeyboardTypeASCIICapable;
      text_field.textAlignment = NSTextAlignmentLeft;
      break;
    }
    case GHOST_KeyboardProperties::decimal_numpad_keyboard_type: {
      text_field.keyboardType = UIKeyboardTypeDecimalPad;
      text_field.textAlignment = NSTextAlignmentCenter;
      break;
    }
    case GHOST_KeyboardProperties::numpad_keyboard_type: {
      text_field.keyboardType = UIKeyboardTypeNumberPad;
      text_field.textAlignment = NSTextAlignmentCenter;
      break;
    }
    default: {
      /* What's the sensible baviour here? Default? Assert? */
      text_field.keyboardType = UIKeyboardTypeDefault;
      text_field.textAlignment = NSTextAlignmentLeft;
    }
  }
  /* Reset keyboard type to default if not using Apple Pencil
   * or it's not floating. (Need to add floating detection.) */
  if (!last_tap_with_pencil) {
    // text_field.keyboardType = UIKeyboardTypeDefault;
  }

  /* Set light/dark mode or adopt system default. */
  text_field.keyboardAppearance = UIKeyboardAppearanceDefault;

  /* This seems sensible given Blender's typical behaviour. */
  text_field.autocorrectionType = UITextAutocorrectionTypeNo;
  text_field.spellCheckingType = UITextSpellCheckingTypeNo;

  /* Set font size. */
  float fontSize = keyboard_properties.font_size / window->getWindowScaleFactor();
  text_field.font = [UIFont systemFontOfSize:fontSize];

  /* Set font color. */
  text_field.textColor = [UIColor colorWithRed:keyboard_properties.font_color[0]
                                         green:keyboard_properties.font_color[1]
                                          blue:keyboard_properties.font_color[2]
                                         alpha:keyboard_properties.font_color[3]];

  /* Initial highlighting and text-cursor position. */
  switch (keyboard_properties.inital_text_state) {
    case GHOST_KeyboardProperties::select_all_text: {
      [text_field selectAll:nil];
      break;
    }
    case GHOST_KeyboardProperties::select_text_range: {
      UITextPosition *startPosition = [text_field
          positionFromPosition:text_field.beginningOfDocument
                        offset:keyboard_properties.text_select_range[0]];
      UITextPosition *endPosition = [text_field
          positionFromPosition:text_field.beginningOfDocument
                        offset:keyboard_properties.text_select_range[1]];
      text_field.selectedTextRange = [text_field textRangeFromPosition:startPosition
                                                            toPosition:endPosition];
      break;
    }
    case GHOST_KeyboardProperties::move_cursor_to_start: {
      UITextPosition *beginning = text_field.beginningOfDocument;
      text_field.selectedTextRange = [text_field textRangeFromPosition:beginning
                                                            toPosition:beginning];
      break;
    }
    case GHOST_KeyboardProperties::move_cursor_to_end: {
      UITextPosition *end = text_field.endOfDocument;
      text_field.selectedTextRange = [text_field textRangeFromPosition:end toPosition:end];
      break;
    }
    default: {
      GHOST_ASSERT(FALSE, "GHOST_SystemIOS::setupTextField unsupported text select option");
    }
  }

  /* Setup the tool bar if it's enabled. */
  if (toolbar_enabled) {
    toolbar_live_text_item.title = text_field.text;
    toolbar_tip_item.title = keyboard_properties.tip_text ?
                                 [NSString stringWithCString:keyboard_properties.tip_text
                                                    encoding:NSUTF8StringEncoding] :
                                 @"";
  }
}

- (void)externalKeyboardChange:(NSNotification *)notification
{
  external_keyboard_connected = [GCKeyboard coalescedKeyboard] != nil;
  IOS_INPUT_LOG(@"External Keyboard %s",
                external_keyboard_connected ? "Connected" : "Disconnected");
}

/* IOS_FIXME - Not currently used, could be removed. */
- (void)keyboardWillChange:(NSNotification *)notification
{

  CGRect keyboardRect = [notification.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
  /* Sometimes we see a zero value for the end-frame value, possibly because... timing? */
  if (keyboardRect.size.width == 0 || keyboardRect.size.height == 0) {
    keyboardRect = [notification.userInfo[UIKeyboardFrameBeginUserInfoKey] CGRectValue];
  }
}

- (const GHOST_TabletData)getTabletData
{
  return tablet_data;
}

- (GHOST_TSuccess)popupOnscreenKeyboard:(const GHOST_KeyboardProperties &)keyboard_properties
{
  @synchronized(self) {
    IOS_INPUT_LOG(@"Keyboard popup request received %@", text_field.text);
    [self setupKeyboard:keyboard_properties];

    if (!onscreen_keyboard_active) {
      text_field.userInteractionEnabled = YES;
      if (![text_field becomeFirstResponder]) {
        GHOST_ASSERT(FALSE, "GHOST_SystemIOS::popupOnScreenKeyboard Failed to display keyboard");
      }
      onscreen_keyboard_active = true;
    }
  }
  return GHOST_kSuccess;
}

- (GHOST_TSuccess)hideOnscreenKeyboard
{
  /* Lock access around keyboard handling events. */
  @synchronized(self) {
    IOS_INPUT_LOG(@"Keyboard hide request received %@", text_field.text);

    if (onscreen_keyboard_active) {
      /*
       This must come first so that any of the keyboard event handlers that get
       triggered in response to shutting down the keyboard don't do anything
       (like generating events back to Blender)
       */
      onscreen_keyboard_active = false;

      /* Shut down the keyboard. */
      [text_field resignFirstResponder];
      /*
       IOS_FIXME - Note: This may cause the console to display the warning message:
       "-[UIApplication _touchesEvent] will no longer work as expected. Please stop using it."
       But since this is being generated by Apple OS code there's nothing obvious to fix it right
       now.
       */

      IOS_INPUT_LOG(@"Resigned keyboard responder");
      /*
       This is required to disable any subsequent interactions with the text field that could
       potentially bypass Blender's input handling (since the UITextField is now live
       on the view)
       */
      text_field.userInteractionEnabled = NO;

      /* Save the input to a c-string */
      text_field_string = [[text_field text] UTF8String];

      /* Delete the text field copy of the string */
      text_field.text = nil;
    }
  }
  IOS_INPUT_LOG(@"Text field value was %s", text_field_string);
  return GHOST_kSuccess;
}

- (const char *)getLastKeyboardString
{
  /* Lock access around keyboard handling events */
  @synchronized(self) {

    /* Update text string if one exists */
    if (text_field.text && ![text_field.text isEqualToString:@""]) {
      /* Save the input to a c-string */
      text_field_string = [[text_field text] UTF8String];
    }
  }
  return text_field_string;
}

@end

static NSString *blenderIOSMenuRoute(NSDictionary *route)
{
  NSData *data = [NSJSONSerialization dataWithJSONObject:route options:0 error:nil];
  return [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
}

static UIImage *blenderIOSMenuIcon(NSDictionary *item)
{
  NSString *name = item[@"icon"];
  if (![name isKindOfClass:NSString.class] || name.length == 0 ||
      [@[@"NONE", @"BLANK1"] containsObject:name])
  {
    return nil;
  }
  return GHOSTIOSBlenderIcon(name, NO);
}

static NSArray *blenderIOSMenuResult(uint64_t identifier)
{
  char *json = WM_apple_ios_ui_menu_take_result(identifier);
  if (!json) {
    return nil;
  }
  NSData *data = [NSData dataWithBytes:json length:strlen(json)];
  free(json);
  id items = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  return [items isKindOfClass:[NSArray class]] ? items : @[];
}

static NSArray<UIMenuElement *> *blenderIOSNativeMenuElements(NSArray *items);

static UIMenu *blenderIOSNativeSubmenu(NSDictionary *item, NSString *identifier)
{
  NSString *route = blenderIOSMenuRoute(item[@"route"]);
  /* Uncached providers refresh dynamic menus (recent files, undo, add-ons) on open.
   * The engine fills the request in its event pump; UIKit never calls into Python. */
  UIDeferredMenuElement *content = [UIDeferredMenuElement
      elementWithUncachedProvider:^(void (^completion)(NSArray<UIMenuElement *> *)) {
        const uint64_t request = WM_apple_ios_ui_menu_request(route.UTF8String, false);
        const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 60.0;
        NSTimer *poll = [NSTimer timerWithTimeInterval:0.025 repeats:YES block:^(NSTimer *timer) {
          NSArray *result = blenderIOSMenuResult(request);
          if (result || CFAbsoluteTimeGetCurrent() >= deadline) {
            [timer invalidate];
            NSArray *elements = blenderIOSNativeMenuElements(result);
            if (elements.count == 0) {
              UIAction *empty = [UIAction actionWithTitle:result ? @"No Available Commands" : @"Blender Is Busy"
                                                   image:nil identifier:nil handler:^(__kindof UIAction *) {}];
              empty.attributes = UIMenuElementAttributesDisabled;
              elements = @[empty];
            }
            completion(elements);
          }
        }];
        [NSRunLoop.mainRunLoop addTimer:poll forMode:NSRunLoopCommonModes];
      }];
  return [UIMenu menuWithTitle:item[@"title"] image:blenderIOSMenuIcon(item) identifier:identifier
                      options:0 children:@[content]];
}

static NSArray<UIMenuElement *> *blenderIOSNativeMenuElements(NSArray *items)
{
  NSMutableArray *sections = [NSMutableArray array];
  NSMutableArray *children = [NSMutableArray array];
  void (^flush)(void) = ^{
    if (children.count) {
      [sections addObject:[UIMenu menuWithTitle:@"" image:nil identifier:nil
                                       options:UIMenuOptionsDisplayInline children:[[children copy] autorelease]]];
      [children removeAllObjects];
    }
  };
  for (NSDictionary *item in items) {
    if (![item isKindOfClass:NSDictionary.class]) {
      continue;
    }
    if ([item[@"separator"] boolValue]) {
      flush();
      continue;
    }
    if (![item[@"title"] isKindOfClass:NSString.class] || ![item[@"title"] length] ||
        ![item[@"route"] isKindOfClass:NSDictionary.class]) {
      continue;
    }
    if ([item[@"submenu"] boolValue] && ![item[@"disabled"] boolValue]) {
      [children addObject:blenderIOSNativeSubmenu(item, nil)];
      continue;
    }
    NSString *route = blenderIOSMenuRoute(item[@"route"]);
    UIImage *icon = blenderIOSMenuIcon(item);
    UIAction *action = [UIAction actionWithTitle:item[@"title"] image:icon identifier:nil
                                       handler:^(__kindof UIAction *) {
                                         WM_apple_ios_ui_menu_request(route.UTF8String, true);
                                       }];
    action.attributes = [item[@"disabled"] boolValue] ? UIMenuElementAttributesDisabled : 0;
    action.state = [item[@"checked"] boolValue] ? UIMenuElementStateOn : UIMenuElementStateOff;
    [children addObject:action];
  }
  flush();
  return sections;
}

@interface GHOST_IOSViewController : UIViewController <UIEditMenuInteractionDelegate,
                                                       GHOSTIOSPencilMenuDelegate,
                                                       UIGestureRecognizerDelegate>

- (nonnull instancetype)initWithMetalKitView:(nonnull MTKView *)mtkView
                                      window:(nonnull GHOST_WindowIOS *)window;
- (void)detachBlender;

@end

@implementation GHOST_IOSViewController
{
  MTKView *_view;
  GHOST_WindowIOS *_ghost_window;
  GHOST_IOSMetalRenderer *_renderer;
  UIActivityIndicatorView *_viewport_loading_indicator;
  UIEditMenuInteraction *_actions_edit_menu_interaction;
  GHOSTIOSPassThroughOverlayView *_overlay_view;
  GHOSTIOSNavigationBar *_top_bar;
  UINavigationItem *_top_bar_item;
  NSLayoutConstraint *_top_bar_height_constraint;
  UIBarButtonItem *_mode_button;
  /* Blender's mode list for the active object, and its current mode identifier. */
  NSArray *_mode_items;
  NSString *_object_mode;
  UIBarButtonItem *_actions_button;
  UIBarButtonItem *_viewport_shading_button;
  UIBarButtonItem *_undo_button;
  UIBarButtonItem *_finger_input_button;
  GHOSTIOSScenePanels *_scene_panels;
  GHOSTIOSRedoStrip *_redo_strip;
  GHOSTIOSTimeline *_timeline;
  GHOSTIOSAssets *_assets;
  BOOL _finger_tool_enabled;
  GHOSTIOSPencilMenu *_pencil_menu;
  UIBarButtonItemGroup *_mesh_select_group;
  UISegmentedControl *_mesh_select_control;
  NSMutableArray *_mesh_select_buttons;
  NSMutableArray *_toolbar_buttons;
  NSMutableArray *_action_buttons;
  UIBarButtonItemGroup *_top_settings_group;
  NSDictionary *_top_settings_values;
  GHOSTIOSPassThroughVisualEffectView *_tool_rail;
  UIGlassEffect *_tool_rail_glass_effect;
  UIScrollView *_tool_scroll;
  UIButton *_tool_drag_handle;
  NSLayoutConstraint *_tool_drag_handle_height_constraint;
  UIStackView *_tool_stack;
  GHOSTIOSPassThroughVisualEffectView *_action_rail;
  UIGlassEffect *_action_rail_glass_effect;
  UIScrollView *_action_scroll;
  UIStackView *_action_stack;
  UIView *_action_group;
  UIButton *_action_toggle;
  NSLayoutConstraint *_action_expanded_width_constraint;
  NSLayoutConstraint *_action_collapsed_width_constraint;
  NSLayoutConstraint *_action_toggle_width_constraint;
  NSLayoutConstraint *_action_toggle_trailing_constraint;
  NSLayoutConstraint *_action_group_leading_constraint;
  NSLayoutConstraint *_action_group_center_constraint;
  CGRect _redo_lane_rect;
  BOOL _redo_lane_wide;
  NSLayoutConstraint *_action_scroll_trailing_constraint;
  NSLayoutConstraint *_redo_pill_width_constraint;
  NSLayoutConstraint *_redo_pill_min_width_constraint;
  BOOL _actions_collapsed;
  NSUInteger _action_transition_generation;
  UIView *_view_gizmo_hotspot;
  UIView *_view_gizmo_feedback;
  NSArray *_view_gizmo_geometry;
  UISelectionFeedbackGenerator *_view_gizmo_selection_feedback;
  UIImpactFeedbackGenerator *_view_gizmo_open_feedback;
  BOOL _view_gizmo_hovered;
  BOOL _view_gizmo_pressed;
  NSMutableArray *_view_pie_buttons;
  NSTimer *_toolbar_refresh_timer;
  NSTimer *_panels_refresh_timer;
  NSArray *_native_menubar_items;
  uint64_t _native_menubar_request;
  NSString *_toolbar_signature;
  NSString *_action_signature;
  NSString *_active_tool_identifier;
  NSLayoutConstraint *_tool_rail_width_constraint;
  NSLayoutConstraint *_tool_rail_height_constraint;
  NSLayoutConstraint *_tool_rail_bottom_constraint;
  NSLayoutConstraint *_tool_rail_center_y_constraint;
  NSLayoutConstraint *_action_phone_bottom_constraint;
  /* The open asset pane is a tray along the bottom; these controls sit on top of it. */
  NSLayoutConstraint *_action_bottom_constraint;
  CGFloat _bottom_tray;
  NSLayoutConstraint *_action_phone_trailing_constraint;
  UIButton *_toolbar_hover_button;
  UIVisualEffectView *_toolbar_label_bubble;
  UILabel *_toolbar_label;
  UIButton *_toolbar_label_button;
  NSUInteger _toolbar_label_generation;
  BOOL _toolbar_label_transient;
  UIButton *_view_pie_hover_button;
  BOOL _tool_rail_docked_left_center;
  /* Brush rail: a second left rail listing the brush assets of the current paint mode. */
  GHOSTIOSPassThroughVisualEffectView *_brush_rail;
  UIScrollView *_brush_scroll;
  UIStackView *_brush_stack;
  UIButton *_brush_drag_handle;
  NSMutableArray *_brush_buttons;
  NSString *_brush_signature;
  NSString *_active_brush_identifier;
  NSLayoutConstraint *_brush_rail_leading_constraint;
  NSLayoutConstraint *_brush_rail_width_constraint;
  NSLayoutConstraint *_brush_rail_height_constraint;
  NSLayoutConstraint *_brush_rail_bottom_constraint;
  NSLayoutConstraint *_brush_drag_handle_height_constraint;
  UIButton *_brush_hover_button;
  BOOL _brush_rail_available;
  BOOL _brush_rail_docked_left_center;
  BOOL _brush_rail_expanded;
  BOOL _brush_press_select_active;
  BOOL _toolbar_expanded;
  /* Blender's own interface, as on a desktop computer, with the native controls hidden. */
  BOOL _desktop_interface;
  BOOL _toolbar_press_select_active;
  BOOL _view_pie_visible;
}

- (nonnull instancetype)initWithMetalKitView:(nonnull MTKView *)mtkView
                                      window:(nonnull GHOST_WindowIOS *)window
{
  self = [super init];
  if (self) {
    _ghost_window = window;
    _view = mtkView;
    _view.multipleTouchEnabled = YES;
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
  _view.enableSetNeedsDisplay = NO;
  _view.clearColor = MTLClearColorMake(0, 0, 0, 1.0);
  /* MTKView only manages native layout. It must never acquire a drawable on UIKit. */
  _view.paused = YES;
  _view.colorPixelFormat = MTLPixelFormatRGBA16Float;
  _view.autoResizeDrawable = YES;
  _view.contentMode = UIViewContentModeScaleToFill;
  [self updateMTKViewDrawableScale];
  /* Set the refresh rate to the screen's maximum. There may be some value in capping
   * this value to preserve battery life (60fps seems to work well). */
  _view.preferredFramesPerSecond = [UIScreen mainScreen].maximumFramesPerSecond;
  _renderer = [[GHOST_IOSMetalRenderer alloc] initWithWindow:_ghost_window];
  if (!_renderer) {
    NSLog(@"Renderer initialization failed");
    return;
  }

  [_renderer mtkView:_view drawableSizeWillChange:_view.drawableSize];

  _view.delegate = _renderer;

  [self installIPadOverlay];
  [self installViewportLoadingIndicator];
  if ([NSUserDefaults.standardUserDefaults boolForKey:@"BlenderDesktopInterface"]) {
    /* Start in the interface the user left; Blender follows from its inbox. */
    WM_apple_ios_ui_setting_enqueue("interface", "desktop");
    [self setDesktopInterface:YES];
  }
  _toolbar_refresh_timer = [NSTimer scheduledTimerWithTimeInterval:0.25
                                                            target:self
                                                          selector:@selector(refreshToolbarFromBlender)
                                                          userInfo:nil
                                                           repeats:YES];
  [self refreshToolbarFromBlender];
  _panels_refresh_timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 target:self
      selector:@selector(refreshScenePanelsFromBlender) userInfo:nil repeats:YES];
  [NSRunLoop.mainRunLoop addTimer:_panels_refresh_timer forMode:NSRunLoopCommonModes];
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
  actions_menu_press.enabled = UIDevice.currentDevice.userInterfaceIdiom != UIUserInterfaceIdiomPhone;
  [_view addGestureRecognizer:actions_menu_press];
  [actions_menu_press release];
  [self setNeedsUpdateOfHomeIndicatorAutoHidden];
}

- (void)updateMTKViewDrawableScale
{
  UIScreen *screen = _view.window.screen ?: [UIScreen mainScreen];
  CGFloat scale = screen.nativeScale > 0.0 ? screen.nativeScale : screen.scale;
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
  if (_viewport_loading_indicator.isAnimating && _ghost_window && _ghost_window->surface &&
      _ghost_window->surface->hasPresentedFrame()) {
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
  [_renderer detach];
  _view.delegate = nil;
  _ghost_window = nullptr;
}

- (void)dealloc
{
  [_scene_panels release];
  [_redo_strip release];
  [_mode_items release];
  [_object_mode release];
  [_timeline release];
  [_assets release];
  [NSNotificationCenter.defaultCenter removeObserver:self];
  [_renderer release];
  [_view_gizmo_geometry release];
  [_view_gizmo_selection_feedback release];
  [_view_gizmo_open_feedback release];
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
  [_redo_pill_width_constraint release];
  [_redo_pill_min_width_constraint release];
  [_view_pie_buttons release];
  [super dealloc];
}

- (NSArray<NSDictionary *> *)radialItemsFromBlenderMenu:(NSArray *)sources
{
  NSMutableArray *items = [NSMutableArray array];
  for (NSDictionary *source in sources) {
    if (![source isKindOfClass:NSDictionary.class]) {
      continue;
    }
    if ([source[@"heading"] isKindOfClass:NSString.class]) {
      /* The menu's own name, shown in the radial menu's center. */
      [items addObject:@{@"heading": source[@"heading"]}];
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

- (void)pencilMenu:(GHOSTIOSPencilMenu *)menu
    requestItemsForRoute:(NSDictionary *)route
                atPoint:(CGPoint)point
             completion:(void (^)(NSArray<NSDictionary *> *))completion
{
  if (!_ghost_window) {
    completion(@[]);
    return;
  }
  CGPoint viewport_point = [menu convertPoint:point toView:_view];
  CGPoint pixels = _ghost_window->scalePointToWindow(viewport_point);
  NSString *request_route = blenderIOSMenuRoute(route);
  const uint64_t request = WM_apple_ios_ui_menu_at_position_request(
      request_route.UTF8String, false, pixels.x, pixels.y);
  const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 60.0;
  NSTimer *poll = [NSTimer timerWithTimeInterval:0.025 repeats:YES block:^(NSTimer *timer) {
    NSArray *result = blenderIOSMenuResult(request);
    if (result || CFAbsoluteTimeGetCurrent() >= deadline) {
      [timer invalidate];
      completion([self radialItemsFromBlenderMenu:result]);
    }
  }];
  [NSRunLoop.mainRunLoop addTimer:poll forMode:NSRunLoopCommonModes];
}

- (void)pencilMenu:(GHOSTIOSPencilMenu *)menu
    didChooseItem:(NSDictionary *)item
          atPoint:(CGPoint)point
{
  if (!_ghost_window || [item[@"disabled"] boolValue] ||
      ![item[@"route"] isKindOfClass:NSDictionary.class])
  {
    return;
  }
  CGPoint viewport_point = [menu convertPoint:point toView:_view];
  CGPoint pixels = _ghost_window->scalePointToWindow(viewport_point);
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
  }, nil);
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
  if (gesture.state != UIGestureRecognizerStateBegan || _pencil_menu.active || _desktop_interface) {
    return;
  }

  UIMenu *menu = [self nativeBlenderContextMenu];
  if (menu.children.count == 0) {
    return;
  }

  const CGPoint location = [gesture locationInView:_view];
  UIEditMenuConfiguration *configuration =
      [UIEditMenuConfiguration configurationWithIdentifier:nil sourcePoint:location];
  [_actions_edit_menu_interaction presentEditMenuWithConfiguration:configuration];
}

- (UIButton *)overlayButtonWithTitle:(NSString *)title
                         systemImage:(NSString *)systemImage
                             command:(NSInteger)command
                      clearGlassStyle:(BOOL)clearGlassStyle
{
  UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
  button.tag = command;
  button.tintColor = UIColor.systemBlueColor;
  button.accessibilityLabel = title;
  button.translatesAutoresizingMaskIntoConstraints = NO;

  UIImage *image = blenderIOSCommandIcon(command, systemImage);
  [button setImage:image forState:UIControlStateNormal];

  UIButtonConfiguration *configuration = clearGlassStyle ?
                                            [UIButtonConfiguration clearGlassButtonConfiguration] :
                                            [UIButtonConfiguration glassButtonConfiguration];
  configuration.image = image;
  configuration.contentInsets = NSDirectionalEdgeInsetsMake(8.0, 8.0, 8.0, 8.0);
  configuration.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  button.configuration = configuration;

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
  UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
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
                         tool:@{@"id": toolIdentifier, @"title": title, @"symbol": systemImage ?: @""}
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
      [UIImage imageNamed:[@"blender.tool." stringByAppendingString:icon]] : nil;
  if (image) {
    return [image imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  }
  NSString *toolIdentifier = tool[@"id"];
  NSString *prefix = selected ? @"selected.tool." : @"tool.";
  image = blenderIOSIcon([NSString stringWithFormat:@"%@%@.%@", prefix, mode, toolIdentifier], nil);
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
    UIButtonConfiguration *configuration = is_emphasized && !updated_button.selected ?
                                              [UIButtonConfiguration glassButtonConfiguration] :
                                              [UIButtonConfiguration plainButtonConfiguration];
    configuration.baseForegroundColor = updated_button.selected ? UIColor.whiteColor : UIColor.labelColor;
    configuration.automaticallyUpdateForSelection = NO;
    configuration.attributedTitle = shows_text ? [[[NSAttributedString alloc]
        initWithString:title attributes:@{NSForegroundColorAttributeName: configuration.baseForegroundColor}]
        autorelease] : nil;
    configuration.image = updated_button.selected ? selected_image : image;
    ((UIImageView *)[updated_button viewWithTag:9001]).tintColor =
        updated_button.selected ? UIColor.whiteColor : UIColor.secondaryLabelColor;
    configuration.imagePlacement = NSDirectionalRectEdgeLeading;
    configuration.imagePadding = shows_text ? 10.0 : 0.0;
    configuration.contentInsets = shows_text ?
                                      NSDirectionalEdgeInsetsMake(8.0, 12.0, 8.0, 14.0) :
                                      NSDirectionalEdgeInsetsMake(8.0, 8.0, 8.0, 8.0);
    configuration.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    updated_button.contentHorizontalAlignment = shows_text ?
                                                    UIControlContentHorizontalAlignmentLeading :
                                                    UIControlContentHorizontalAlignmentCenter;
    updated_button.configuration = configuration;
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
        [sections addObject:[UIMenu menuWithTitle:@"" image:nil identifier:nil
                                         options:UIMenuOptionsDisplayInline children:actions]];
        actions = [NSMutableArray array];
      }
      continue;
    }
    UIAction *action = [UIAction actionWithTitle:child[@"title"]
                                         image:[self toolbarImageForTool:child mode:mode]
                                    identifier:child[@"id"]
                                       handler:^(UIAction *selected) {
      UIButton *chosen_button = nil;
      /* Resolve the current button: snapshots may have replaced the menu's original anchor. */
      for (UIButton *button in self->_toolbar_buttons) {
        if ([button.menu.identifier isEqualToString:tool[@"group"]]) {
          NSMutableDictionary *updated = [NSMutableDictionary dictionaryWithDictionary:tool];
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
    action.state = [child[@"id"] isEqualToString:tool[@"id"]] ?
                      UIMenuElementStateOn : UIMenuElementStateOff;
    [actions addObject:action];
  }
  if (actions.count) {
    [sections addObject:[UIMenu menuWithTitle:@"" image:nil identifier:nil
                                     options:UIMenuOptionsDisplayInline children:actions]];
  }
  return [UIMenu menuWithTitle:@"" image:nil identifier:tool[@"group"] options:0 children:sections];
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

  [self updateToolbarVisibilityAnimated:NO];
  /* Resolve selection artwork in this event, before UIKit's glass transition. */
  [UIView performWithoutAnimation:^{
    for (UIButton *button in self->_toolbar_buttons) {
      [button updateConfiguration];
    }
  }];
}

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

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer shouldReceiveTouch:(UITouch *)touch
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
    return !_toolbar_expanded && !((UIButton *)gestureRecognizer.view).contextMenuInteractionEnabled;
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
    shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)otherGestureRecognizer
{
  UNUSED_VARS(gestureRecognizer, otherGestureRecognizer);
  return NO;
}

- (UIButton *)toolbarDragHandle
{
  UIButton *handle = [UIButton buttonWithType:UIButtonTypeCustom];
  handle.translatesAutoresizingMaskIntoConstraints = NO;
  handle.accessibilityLabel = @"Collapse Toolbar";
  handle.accessibilityIdentifier = @"blender.toolbar.handle";
  [handle addInteraction:[[[UIToolTipInteraction alloc]
      initWithDefaultToolTip:@"Collapse Toolbar"] autorelease]];
  [handle addTarget:self action:@selector(collapseToolbarFromHandle:)
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
      initWithTarget:self action:@selector(handleToolbarHandlePan:)];
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
  const CGFloat raise_distance = MAX(0.0, [self toolRailBottomDockCenterY] - [self toolRailCenterDockCenterY]);
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
      [UIView animateWithDuration:0.2 animations:^{
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
      const BOOL is_active = has_active_button ?
                                 [button.accessibilityIdentifier isEqualToString:_active_tool_identifier] :
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
      const BOOL is_active = has_active_button ?
                                 [button.accessibilityIdentifier isEqualToString:_active_tool_identifier] :
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
                      options:UIViewAnimationOptionCurveEaseInOut |
                              UIViewAnimationOptionAllowUserInteraction |
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
  NSData *data = [NSJSONSerialization dataWithJSONObject:tools options:NSJSONWritingSortedKeys error:nil];
  NSString *contents = [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
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
  if (_toolbar_label_button) {
    [self layoutToolbarLabel];
  }
  else {
    [self hideToolbarLabelAnimated:NO];
  }
}

- (void)updateAxisConstraintButton:(UIButton *)button selected:(BOOL)selected
{
  NSString *axis = button.accessibilityHint;
  UIColor *color = [axis isEqualToString:@"X"] ? UIColor.systemRedColor :
                   [axis isEqualToString:@"Y"] ? UIColor.systemGreenColor :
                                                UIColor.systemBlueColor;
  UIButtonConfiguration *configuration = [UIButtonConfiguration glassButtonConfiguration];
  configuration.attributedTitle = [[[NSAttributedString alloc]
      initWithString:axis
          attributes:@{NSFontAttributeName: [UIFont systemFontOfSize:17.0
                                                             weight:UIFontWeightSemibold]}] autorelease];
  UIColor *foreground = selected ? UIColor.blackColor : color;
  configuration.baseForegroundColor = foreground;
  configuration.baseBackgroundColor = selected ? color : nil;
  /* Keep the selected lettering readable on the bright axis-colored glass. */
  configuration.titleTextAttributesTransformer = ^NSDictionary *(NSDictionary *attributes) {
    NSMutableDictionary *result = [[attributes mutableCopy] autorelease];
    result[NSForegroundColorAttributeName] = foreground;
    return result;
  };
  configuration.imageColorTransformer = ^UIColor *(UIColor *) { return foreground; };
  configuration.image = selected ? [UIImage systemImageNamed:@"checkmark"] : nil;
  configuration.preferredSymbolConfigurationForImage =
      [UIImageSymbolConfiguration configurationWithPointSize:11.0 weight:UIImageSymbolWeightBold];
  configuration.imagePlacement = NSDirectionalRectEdgeTrailing;
  configuration.imagePadding = selected ? 4.0 : 0.0;
  configuration.contentInsets = NSDirectionalEdgeInsetsMake(8.0, 8.0, 8.0, 8.0);
  configuration.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  button.selected = selected;
  button.tintColor = color;
  button.configuration = configuration;
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
        if ([action isKindOfClass:NSDictionary.class] && [action[@"action"] isKindOfClass:NSString.class] &&
            [button.accessibilityIdentifier isEqualToString:
                [@"blender.action." stringByAppendingString:action[@"action"]]])
        {
          const BOOL selected = [action[@"selected"] respondsToSelector:@selector(boolValue)] &&
                                [action[@"selected"] boolValue];
          if (button.selected != selected) {
            button.selected = selected;
            UIButtonConfiguration *configuration = button.configuration;
            configuration.baseBackgroundColor = selected ? UIColor.systemBlueColor : nil;
            configuration.baseForegroundColor = selected ? UIColor.whiteColor : nil;
            button.configuration = configuration;
            button.accessibilityTraits = selected ? UIAccessibilityTraitButton | UIAccessibilityTraitSelected :
                                                    UIAccessibilityTraitButton;
          }
          break;
        }
      }
      continue;
    }
    NSString *axis = button.accessibilityHint;
    if (![@[@"X", @"Y", @"Z"] containsObject:axis ?: @""]) {
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
                            operator:(NSString *)opName
                            property:(NSString *)propertyName
                               value:(NSString *)propertyValue
                                 key:(NSString *)keyName
{
  UNUSED_VARS(systemImage);
  UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
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
                       blenderIOSActionIcon(opName, propertyName, propertyValue) : nil;

  UIButtonConfiguration *configuration = [UIButtonConfiguration glassButtonConfiguration];
  configuration.title = title;
  configuration.image = image;
  configuration.imagePlacement = NSDirectionalRectEdgeLeading;
  configuration.imagePadding = image != nil ? 5.0 : 0.0;
  configuration.contentInsets = NSDirectionalEdgeInsetsMake(8.0, 10.0, 8.0, 10.0);
  configuration.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  button.configuration = configuration;
  if ([@[@"X", @"Y", @"Z"] containsObject:keyName ?: @""]) {
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

- (NSString *)actionSignatureForActions:(NSArray *)actions
{
  NSMutableString *signature = [NSMutableString string];
  for (NSDictionary *action in actions) {
    if (![action isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    for (NSString *json_key in @[ @"title", @"action", @"operator", @"property", @"value", @"key" ]) {
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
    NSString *action_id = [action[@"action"] isKindOfClass:NSString.class] ? action[@"action"] : @"";
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
        UIButtonConfiguration *configuration = button.configuration;
        configuration.image = GHOSTIOSBlenderIcon(icon, NO);
        configuration.imagePadding = configuration.image ? 5.0 : 0.0;
        button.configuration = configuration;
      }
    }
    [_action_stack addArrangedSubview:button];
  }

  _action_rail.hidden = _action_buttons.count == 0 && !_redo_strip.collapsedWidth;
  _action_rail.alpha = _action_rail.hidden ? 0.0 : 1.0;
  [self updateAxisConstraintsWithActions:actions];
  [_action_scroll setContentOffset:CGPointZero animated:NO];
}

/* Fixed so the Actions pill never resizes as the redo pill comes and goes. */
static const CGFloat GHOSTIOSActionsPillWidth = 88.0;

- (void)installActionStripToggle
{
  _action_group = [[[GHOSTIOSPassThroughOverlayView alloc] init] autorelease];
  _action_group.translatesAutoresizingMaskIntoConstraints = NO;
  [_action_rail.contentView addSubview:_action_group];
  [_action_group addSubview:_action_scroll];

  _action_toggle = [UIButton buttonWithType:UIButtonTypeSystem];
  _action_toggle.translatesAutoresizingMaskIntoConstraints = NO;
  _action_toggle.tintColor = UIColor.labelColor;
  _action_toggle.accessibilityIdentifier = @"blender.quick-actions.toggle";
  _action_toggle.titleLabel.adjustsFontSizeToFitWidth = YES;
  _action_toggle.titleLabel.minimumScaleFactor = 0.7;
  [_action_toggle addTarget:self action:@selector(toggleActionStrip:)
          forControlEvents:UIControlEventTouchUpInside];
  [_action_group addSubview:_action_toggle];
  UIButton *redo_pill = _redo_strip.collapsedButton;
  redo_pill.translatesAutoresizingMaskIntoConstraints = NO;
  [_action_group addSubview:redo_pill];
  _redo_pill_width_constraint = [[redo_pill.widthAnchor constraintEqualToConstant:0] retain];
  _redo_pill_width_constraint.priority = UILayoutPriorityDefaultHigh - 1;
  _redo_pill_min_width_constraint = [[redo_pill.widthAnchor constraintGreaterThanOrEqualToConstant:0] retain];
  _action_scroll_trailing_constraint = [[_action_scroll.trailingAnchor
      constraintEqualToAnchor:redo_pill.leadingAnchor] retain];
  __block GHOST_IOSViewController *owner = self;
  _redo_strip.presentationDidChange = ^(BOOL animated) {
    [owner updateRedoPillLayout:animated];
  };

  _action_expanded_width_constraint = [[_action_group.widthAnchor
      constraintEqualToAnchor:_action_stack.widthAnchor constant:60.0] retain];
  _action_expanded_width_constraint.priority = UILayoutPriorityDefaultHigh;
  _action_collapsed_width_constraint = [[_action_group.widthAnchor
      constraintEqualToConstant:GHOSTIOSActionsPillWidth] retain];
  _action_collapsed_width_constraint.priority = UILayoutPriorityDefaultHigh;
  _action_toggle_width_constraint = [[_action_toggle.widthAnchor
      constraintEqualToConstant:44.0] retain];
  _action_toggle_width_constraint.priority = UILayoutPriorityRequired - 1;
  _action_toggle_trailing_constraint = [[_action_toggle.trailingAnchor
      constraintEqualToAnchor:redo_pill.leadingAnchor] retain];
  /* Expanded controls center on the viewport; collapsed pills rest bottom-leading. */
  _action_group_leading_constraint = [[_action_group.leadingAnchor
      constraintEqualToAnchor:_action_rail.contentView.leadingAnchor] retain];
  _action_group_center_constraint = [[_action_group.centerXAnchor
      constraintEqualToAnchor:_overlay_view.centerXAnchor] retain];
  _action_group_center_constraint.priority = UILayoutPriorityRequired - 2;
  NSLayoutConstraint *minimum_width = [_action_group.widthAnchor constraintGreaterThanOrEqualToConstant:GHOSTIOSActionsPillWidth];
  minimum_width.priority = UILayoutPriorityRequired - 1;
  [NSLayoutConstraint activateConstraints:@[
    minimum_width,
    [_action_group.leadingAnchor constraintGreaterThanOrEqualToAnchor:_action_rail.contentView.leadingAnchor],
    [_action_group.trailingAnchor constraintLessThanOrEqualToAnchor:_action_rail.contentView.trailingAnchor],
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
  [self setActionsCollapsed:NO animated:NO];
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
  _action_toggle_width_constraint.constant = _actions_collapsed ? GHOSTIOSActionsPillWidth : 44;
  _action_collapsed_width_constraint.constant = GHOSTIOSActionsPillWidth + reserved;
  _action_expanded_width_constraint.constant = 60 + reserved;
  _action_rail.hidden = _action_buttons.count == 0 && !width;
  _action_rail.alpha = _action_rail.hidden ? 0 : 1;
  [UIView animateWithDuration:animated && !UIAccessibilityIsReduceMotionEnabled() ? 0.3 : 0
      delay:0 usingSpringWithDamping:0.86 initialSpringVelocity:0
      options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
      animations:^{ [self->_action_rail layoutIfNeeded]; } completion:nil];
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
  _action_toggle_width_constraint.constant = collapsed ? GHOSTIOSActionsPillWidth : 44;
  _action_toggle_width_constraint.active = YES;
  _action_toggle_trailing_constraint.active = collapsed;

  UIButtonConfiguration *configuration = [UIButtonConfiguration glassButtonConfiguration];
  configuration.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  configuration.title = collapsed ? @"Actions" : nil;
  configuration.titleLineBreakMode = NSLineBreakByTruncatingTail;
  configuration.image = collapsed ? nil : [UIImage systemImageNamed:@"xmark"];
  configuration.preferredSymbolConfigurationForImage =
      [UIImageSymbolConfiguration configurationWithPointSize:13 weight:UIImageSymbolWeightSemibold];
  configuration.contentInsets = NSDirectionalEdgeInsetsMake(8, 10, 8, 10);
  /* Dissolve between the title and close icon instead of snapping mid-spring. */
  [UIView transitionWithView:_action_toggle
                    duration:animate ? 0.22 : 0.0
                     options:UIViewAnimationOptionTransitionCrossDissolve |
                             UIViewAnimationOptionBeginFromCurrentState |
                             UIViewAnimationOptionAllowUserInteraction
                  animations:^{ self->_action_toggle.configuration = configuration; }
                  completion:nil];
  _action_toggle.titleLabel.numberOfLines = 1;
  _action_toggle.accessibilityLabel = collapsed ? @"Show Quick Actions" : @"Hide Quick Actions";
  _action_toggle.accessibilityValue = collapsed ? @"Collapsed" : @"Expanded";
  _action_toggle.toolTip = _action_toggle.accessibilityLabel;
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
                     self->_action_scroll.transform = collapsed && animate ?
                         CGAffineTransformMakeTranslation(-20.0, 0.0) : CGAffineTransformIdentity;
                   }
                   completion:^(BOOL finished) {
                     if (self->_action_transition_generation == generation) {
                       self->_action_scroll.hidden = self->_actions_collapsed;
                     }
                   }];
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
  _native_menubar_request = WM_apple_ios_ui_menu_request(
      "{\"root\":\"menubar\",\"path\":[]}", false);
}

- (void)buildMenuWithBuilder:(id<UIMenuBuilder>)builder
{
  [super buildMenuWithBuilder:builder];
  if (builder.system != UIMenuSystem.mainSystem || !_native_menubar_items.count) {
    return;
  }
  NSDictionary *systemIdentifiers = @{
    @"TOPBAR_MT_blender": UIMenuApplication,
    @"TOPBAR_MT_file": UIMenuFile,
    @"TOPBAR_MT_edit": UIMenuEdit,
    @"TOPBAR_MT_window": UIMenuWindow,
    @"TOPBAR_MT_help": UIMenuHelp,
    @"VIEW3D_MT_view": UIMenuView,
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
    NSString *identifier = systemIdentifier ?: [@"org.blender.menu." stringByAppendingString:menuName ?: item[@"title"]];
    UIMenu *menu = blenderIOSNativeSubmenu(item, identifier);
    if (systemIdentifier && [builder menuForIdentifier:systemIdentifier]) {
      if ([systemIdentifier isEqualToString:UIMenuApplication] ||
          [systemIdentifier isEqualToString:UIMenuWindow]) {
        /* Keep iPadOS window-management and application commands available. */
        UIMenu *group = [UIMenu menuWithTitle:@"" image:nil identifier:nil
                                    options:UIMenuOptionsDisplayInline children:menu.children];
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
    [builder insertChildMenu:[UIMenu menuWithTitle:@"Commands" children:blenderIOSNativeMenuElements(commands)]
        atEndOfMenuForIdentifier:UIMenuRoot];
  }
}

- (void)setDesktopInterface:(BOOL)desktop
{
  _desktop_interface = desktop;
  [NSUserDefaults.standardUserDefaults setBool:desktop forKey:@"BlenderDesktopInterface"];
  [_pencil_menu cancel];
  /* No native controls, radial menu or bottom tray over Blender's own interface. */
  [(GHOSTUIWindow *)_ghost_window->rootWindow setPencilContextMenu:desktop ? nil : _pencil_menu];
  if (desktop) {
    WM_apple_ios_ui_bottom_tray_set(0.0f);
  }
  [self.navigationController setNavigationBarHidden:desktop animated:YES];
  UIView *overlay = _overlay_view;
  if (!desktop) {
    overlay.hidden = NO;
  }
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0 : 0.25
      animations:^{ overlay.alpha = desktop ? 0 : 1; }
      completion:^(BOOL finished) {
        if (finished && overlay.alpha == 0) {
          overlay.hidden = YES;
        }
      }];
  [self.view setNeedsLayout];
}

- (void)refreshToolbarFromBlender
{
  [self updateViewportLoadingIndicator];
  [self refreshNativeMenuBar];
  NSDictionary *model = [self toolbarModel];
  if ([model[@"desktop"] isKindOfClass:NSNumber.class] && [model[@"desktop"] boolValue] != _desktop_interface) {
    /* Blender's own Window menu can switch the interface too. */
    [self setDesktopInterface:[model[@"desktop"] boolValue]];
  }
  [_redo_strip updateSnapshot:model[@"redo"]];
  NSArray *navigation_geometry = model[@"navigationGizmo"];
  if ([navigation_geometry isKindOfClass:NSArray.class] && navigation_geometry.count == 3 &&
      ![_view_gizmo_geometry isEqual:navigation_geometry]) {
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
  UIBarButtonItemGroup *settings_group = transform || !tool_group ? _top_settings_group : tool_group;
  if (_top_bar_item.centerItemGroups.firstObject != settings_group) {
    [_scene_panels hideToolSettings];
    _top_bar_item.centerItemGroups = @[settings_group];
  }
  NSString *action_signature = [self actionSignatureForActions:actions];
  if (_action_signature == nil || ![_action_signature isEqualToString:action_signature]) {
    [_action_signature release];
    _action_signature = [action_signature copy];
    /* Build immediately: a delayed completion could restore a finished transform's buttons. */
    [self rebuildActionStripWithActions:actions];
  }
  [self updateAxisConstraintsWithActions:actions];
  [self updateActionsButtonMenu];
}

- (void)viewDidLayoutSubviews
{
  [super viewDidLayoutSubviews];
  CGRect panel_rect = _overlay_view.safeAreaLayoutGuide.layoutFrame;
  /* The bottom tray: the open asset pane along the edge and the timeline just above it. The quick
   * actions, the Adjust pill, the collapsed rails and the navigation gizmo rest on top, so the tray
   * never leaves a gap and follows the timeline as it opens or expands. */
  const CGFloat assets_tray = _assets.visible && !_desktop_interface ? _assets.height + 8 : 0;
  const CGFloat timeline_tray = _timeline.visible && !_desktop_interface ? _timeline.height + 8 : 0;
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
    WM_apple_ios_ui_bottom_tray_set(tray > 0 ? float((window_height - tray_top) / window_height) : 0.0f);
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
  /* The asset pane spans the full width along the bottom edge, and the timeline sits on the edge or
   * on the asset pane; everything else sits above them. The tray keeps the same distance from the
   * sides of the window as from its bottom. */
  const CGRect bounds = _overlay_view.bounds;
  const CGFloat tray_bottom = CGRectGetMaxY(redo_rect) - 6;
  const CGFloat edge = CGRectGetMaxY(bounds) - tray_bottom;
  const CGFloat tray_left = MAX(CGRectGetMinX(redo_rect), CGRectGetMinX(bounds) + edge);
  const CGFloat tray_width = MAX(0, MIN(CGRectGetMaxX(redo_rect), CGRectGetMaxX(bounds) - edge) - tray_left);
  [_assets layoutInRect:CGRectMake(tray_left, panel_top, tray_width, MAX(0, tray_bottom - panel_top))];
  [_timeline layoutInRect:CGRectMake(tray_left, panel_top, tray_width,
                                     MAX(0, tray_bottom - assets_tray - panel_top))];
  panel_rect.origin.y = panel_top;
  panel_rect.size.height = MAX(0, panel_bottom - panel_top);
  [_scene_panels layoutInRect:panel_rect];
  _redo_lane_rect = CGRectMake(redo_left, panel_top + 8,
      MAX(0, redo_right - redo_left), MAX(60, redo_bottom - panel_top - 8));
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

- (void)updatePhoneActionStripLayout
{
  /* Folding or rotating a phone should narrow the scrolling lane, not lift it
   * into the viewport. Reserve the actual gizmo footprint beside the lane. */
  CGRect safe = _overlay_view.safeAreaLayoutGuide.layoutFrame;
  _action_phone_bottom_constraint.constant = -6.0 - _bottom_tray;
  _action_phone_trailing_constraint.constant =
      -MAX(12.0, CGRectGetMaxX(safe) - CGRectGetMinX(_view_gizmo_hotspot.frame) + 12.0);
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

- (void)setFingerInputMode:(BOOL)useTool
{
  _finger_tool_enabled = useTool;
  [(GHOSTUIWindow *)self.view.window setFingerToolEnabled:useTool];
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
    UIImage *image = GHOSTIOSBlenderIcon(icon, NO);
    NSString *title = [item[@"label"] length] ? item[@"label"] : identifier;
    UIAction *action = [UIAction actionWithTitle:title
                                           image:image
                                      identifier:[@"blender.mode." stringByAppendingString:identifier]
                                         handler:^(__kindof UIAction *sender) {
                                           self->_mode_button.title = sender.title;
                                           self->_mode_button.image = sender.image;
                                           self->_mode_button.accessibilityValue = sender.title;
                                           WM_apple_ios_ui_setting_enqueue("object_mode",
                                                                           identifier.UTF8String);
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
    @"wireframe": @(GHOST_IOS_UI_COMMAND_SET_VIEWPORT_WIREFRAME),
    @"solid": @(GHOST_IOS_UI_COMMAND_SET_VIEWPORT_SOLID),
    @"material": @(GHOST_IOS_UI_COMMAND_SET_VIEWPORT_MATERIAL),
    @"rendered": @(GHOST_IOS_UI_COMMAND_SET_VIEWPORT_RENDERED),
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
  _viewport_shading_button.menuRepresentation =
      [menu menuByReplacingChildren:actions];
  _viewport_shading_button.tag = command.integerValue;
}

- (UIButton *)viewPieButtonWithTitle:(NSString *)title
                         systemImage:(NSString *)systemImage
                              command:(NSInteger)command
{
  UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
  button.tintColor = UIColor.labelColor;
  /* The pills float over the always-dark viewport, whatever the app's appearance. */
  button.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  button.translatesAutoresizingMaskIntoConstraints = YES;
  button.accessibilityLabel = title;
  button.tag = command;

  UIImage *image = blenderIOSCommandIcon(command, systemImage);
  if (image == nil) {
    image = [UIImage systemImageNamed:@"circle"];
  }
  button.configurationUpdateHandler = ^(UIButton *updated_button) {
    UIButtonConfiguration *configuration = updated_button.selected ?
                                              [UIButtonConfiguration prominentGlassButtonConfiguration] :
                                              [UIButtonConfiguration glassButtonConfiguration];
    /* Regular glass rather than clear: the grid and objects behind clear glass swamp the label. */
    configuration.baseForegroundColor = updated_button.selected ? UIColor.whiteColor : UIColor.labelColor;
    configuration.baseBackgroundColor = updated_button.selected ? UIColor.systemBlueColor : nil;
    configuration.title = title;
    configuration.image = image;
    configuration.imagePlacement = NSDirectionalRectEdgeLeading;
    configuration.imagePadding = 7.0;
    configuration.contentInsets = NSDirectionalEdgeInsetsMake(8.0, 14.0, 8.0, 14.0);
    configuration.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    updated_button.configuration = configuration;
  };

  [button addTarget:self
                action:@selector(handleOverlayCommand:)
      forControlEvents:UIControlEventTouchUpInside];

  button.hidden = YES;
  button.alpha = 0.0;
  button.transform = CGAffineTransformConcat(CGAffineTransformMakeTranslation(56.0, 56.0),
                                             CGAffineTransformMakeScale(0.72, 0.72));

  [button setNeedsUpdateConfiguration];
  [_view_pie_buttons addObject:button];
  return button;
}

- (void)setViewPieHoverButton:(UIButton *)hoverButton
{
  if (_view_pie_hover_button == hoverButton) {
    return;
  }

  UIButton *previous_hover_button = _view_pie_hover_button;
  _view_pie_hover_button = hoverButton;
  previous_hover_button.selected = NO;
  [previous_hover_button setNeedsUpdateConfiguration];
  _view_pie_hover_button.selected = YES;
  [_view_pie_hover_button setNeedsUpdateConfiguration];
  if (hoverButton) {
    [_view_gizmo_selection_feedback selectionChanged];
  }
}

- (void)updateViewGizmoFeedback
{
  const BOOL active = _view_pie_visible;
  const BOOL highlighted = active || _view_gizmo_hovered || _view_gizmo_pressed;
  _view_gizmo_feedback.layer.borderColor =
      [UIColor.labelColor colorWithAlphaComponent:active ? 0.5 : 0.2].CGColor;
  _view_gizmo_feedback.backgroundColor =
      [UIColor.labelColor colorWithAlphaComponent:active ? 0.12 : 0.05];
  _view_gizmo_hotspot.accessibilityValue = active ? @"View menu open" : nil;
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0.0 : 0.22
                        delay:0.0
       usingSpringWithDamping:0.78
        initialSpringVelocity:0.0
                      options:UIViewAnimationOptionBeginFromCurrentState |
                              UIViewAnimationOptionAllowUserInteraction
                   animations:^{
                     self->_view_gizmo_feedback.alpha = highlighted ? 1.0 : 0.0;
                     CGFloat scale = active ? 1.08 : (self->_view_gizmo_pressed ? 0.94 : 1.0);
                     self->_view_gizmo_feedback.transform = CGAffineTransformMakeScale(scale, scale);
                   }
                   completion:nil];
}

- (void)handleViewGizmoHover:(UIHoverGestureRecognizer *)gesture
{
  BOOL hovered = gesture.state == UIGestureRecognizerStateBegan ||
                 gesture.state == UIGestureRecognizerStateChanged;
  if (_view_gizmo_hovered == hovered) {
    return;
  }
  _view_gizmo_hovered = hovered;
  [self updateViewGizmoFeedback];
}

- (void)handleViewGizmoPress:(UIControl *)control
{
  _view_gizmo_pressed = YES;
  [_view_gizmo_open_feedback prepare];
  [self updateViewGizmoFeedback];
}

- (void)handleViewGizmoRelease:(UIControl *)control
{
  _view_gizmo_pressed = NO;
  [self updateViewGizmoFeedback];
}

- (void)cancelViewGizmoInteraction
{
  [self hideToolbarLabelAnimated:NO];
  _view_gizmo_hovered = NO;
  _view_gizmo_pressed = NO;
  [self setViewPieVisible:NO animated:NO];
}

- (void)layoutViewPie
{
  CGRect viewport = _overlay_view.bounds;
  CGFloat radius = _view_gizmo_geometry ? [_view_gizmo_geometry[2] doubleValue] * viewport.size.width : 40;
  CGPoint center = _view_gizmo_geometry ?
      CGPointMake([_view_gizmo_geometry[0] doubleValue] * viewport.size.width,
                  [_view_gizmo_geometry[1] doubleValue] * viewport.size.height) :
      CGPointMake(CGRectGetMaxX(viewport) - 50, CGRectGetMaxY(viewport) - 50);
  CGFloat target_radius = MAX(44.0, radius + 10.0);
  _view_gizmo_hotspot.bounds = CGRectMake(0, 0, target_radius * 2, target_radius * 2);
  _view_gizmo_hotspot.center = center;
  _view_gizmo_feedback.bounds = CGRectMake(0, 0, (radius + 2) * 2, (radius + 2) * 2);
  _view_gizmo_feedback.center = CGPointMake(target_radius, target_radius);
  _view_gizmo_feedback.layer.cornerRadius = radius + 2;
  CGRect safe = _overlay_view.safeAreaLayoutGuide.layoutFrame;
  if (CGRectIsEmpty(safe)) {
    return;
  }
  const BOOL compact = safe.size.height < 540.0 || safe.size.width < 430.0;
  CGFloat gap = CGRectGetMaxX(safe) - CGRectGetMinX(_view_gizmo_hotspot.frame);
  const CGFloat trailing[] = {gap + 16, gap + 26, gap + 42, gap + 42, gap + 16, 20, 20, 20};
  const CGFloat bottom[] = {12, 72, 132, 192, 244, 304, 364, 424};
  CGFloat width = MIN(180.0, (safe.size.width - 36.0) / 2.0);
  CGFloat base = MIN(136.0, MAX(68.0, safe.size.height - 300.0));
  for (NSUInteger i = 0; i < _view_pie_buttons.count; i++) {
    UIButton *button = _view_pie_buttons[i];
    CGFloat x = compact ? 12.0 + (i % 2) * (width + 12.0) : trailing[i];
    CGFloat y = compact ? base + (i / 2) * 52.0 : bottom[i];
    CGFloat w = compact ? width : MIN(180.0, [button sizeThatFits:CGSizeMake(180, 44)].width);
    /* Bounds/center remain stable while the reveal transform is animating. */
    button.bounds = CGRectMake(0, 0, w, 44);
    button.center = CGPointMake(CGRectGetMaxX(safe) - x - w / 2.0,
                               CGRectGetMaxY(safe) - y - 22.0);
  }
}

- (UIButton *)viewPieButtonAtGestureLocation:(UIGestureRecognizer *)gesture
{
  CGPoint location = [gesture locationInView:_overlay_view];
  for (UIButton *button in _view_pie_buttons) {
    if (button.hidden || button.alpha < 0.05) {
      continue;
    }
    CGPoint local_point = [button convertPoint:location fromView:_overlay_view];
    CGRect hit_bounds = CGRectInset(button.bounds, -14.0, -10.0);
    if (CGRectContainsPoint(hit_bounds, local_point)) {
      return button;
    }
  }
  return nil;
}

- (void)setViewPieVisible:(BOOL)visible animated:(BOOL)animated
{
  _view_pie_visible = visible;
  if (!visible) {
    [self setViewPieHoverButton:nil];
  }
  [self updateViewGizmoFeedback];
  if (visible) {
    [self layoutViewPie];
    for (UIButton *button in _view_pie_buttons) {
      if (button.hidden) {
        CGPoint hub = _view_gizmo_hotspot.center;
        button.transform = CGAffineTransformConcat(
            CGAffineTransformMakeTranslation((hub.x - button.center.x) * 0.18,
                                             (hub.y - button.center.y) * 0.18),
            CGAffineTransformMakeScale(0.82, 0.82));
      }
      button.hidden = NO;
    }
  }

  void (^applyVisibility)(void) = ^{
    for (UIButton *button in self->_view_pie_buttons) {
      button.alpha = visible ? 1.0 : 0.0;
      CGPoint hub = self->_view_gizmo_hotspot.center;
      CGFloat travel = visible ? 0.0 : 0.18;
      button.transform = CGAffineTransformConcat(
          CGAffineTransformMakeTranslation((hub.x - button.center.x) * travel,
                                           (hub.y - button.center.y) * travel),
          CGAffineTransformMakeScale(visible ? 1.0 : 0.82, visible ? 1.0 : 0.82));
    }
  };

  void (^finishVisibility)(BOOL) = ^(BOOL finished) {
    if (!visible && !self->_view_pie_visible) {
      for (UIButton *button in self->_view_pie_buttons) {
        button.hidden = YES;
      }
    }
  };

  if (!animated || UIAccessibilityIsReduceMotionEnabled()) {
    applyVisibility();
    finishVisibility(YES);
    return;
  }

  [UIView animateWithDuration:0.28
                        delay:0.0
       usingSpringWithDamping:0.86
        initialSpringVelocity:0.0
                      options:UIViewAnimationOptionCurveEaseOut |
                              UIViewAnimationOptionAllowUserInteraction |
                              UIViewAnimationOptionBeginFromCurrentState
                   animations:applyVisibility
                   completion:finishVisibility];
}

- (void)handleViewGizmoDrag:(UIPanGestureRecognizer *)gesture
{
  if (gesture.state == UIGestureRecognizerStateBegan) {
    [self setViewPieVisible:YES animated:YES];
    [self setViewPieHoverButton:[self viewPieButtonAtGestureLocation:gesture]];
    return;
  }

  if (gesture.state == UIGestureRecognizerStateChanged) {
    [self setViewPieHoverButton:[self viewPieButtonAtGestureLocation:gesture]];
    return;
  }

  if (gesture.state == UIGestureRecognizerStateEnded) {
    UIButton *button = [self viewPieButtonAtGestureLocation:gesture];
    if (button != nil) {
      WM_apple_ios_ui_command_enqueue((int)button.tag);
    }
    [self setViewPieVisible:NO animated:YES];
    return;
  }

  if (gesture.state == UIGestureRecognizerStateCancelled ||
      gesture.state == UIGestureRecognizerStateFailed)
  {
    [self setViewPieVisible:NO animated:YES];
  }
}

- (void)handleViewGizmoHold:(UILongPressGestureRecognizer *)gesture
{
  if (gesture.state == UIGestureRecognizerStateBegan) {
    /* Consume the pending Pencil tap before it can become a Blender click. */
    [(GHOSTUIWindow *)_view.window cancelPencilStroke];
    [self setViewPieVisible:YES animated:YES];
    [_view_gizmo_open_feedback impactOccurred];
    [_view_gizmo_selection_feedback prepare];
  }
  else if (gesture.state == UIGestureRecognizerStateChanged) {
    [self setViewPieHoverButton:[self viewPieButtonAtGestureLocation:gesture]];
  }
  else if (gesture.state == UIGestureRecognizerStateEnded) {
    UIButton *button = [self viewPieButtonAtGestureLocation:gesture];
    if (button) {
      WM_apple_ios_ui_command_enqueue((int)button.tag);
    }
    [self setViewPieVisible:NO animated:YES];
  }
  else if (gesture.state == UIGestureRecognizerStateCancelled ||
           gesture.state == UIGestureRecognizerStateFailed) {
    [self setViewPieVisible:NO animated:YES];
  }
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
      initWithImage:image menu:[UIMenu menuWithTitle:title children:children]] autorelease];
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
                                        [self settingsActionWithTitle:@"Vertex"
                                                          systemImage:@"point.topleft.down.curvedto.point.bottomright.up"
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
  NSArray *keys = @[@"orientation", @"pivot", @"snap", @"proportional"];
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
                   mixed ? blenderIOSIcon(@"control.Snapping", @"magnet") :
                           [UIImage systemImageNamed:@"rotate.3d"];
    button.accessibilityValue = selected ? selected.title :
                                custom ? @"Custom Orientation" : @"Multiple or Other Elements";
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
    _top_settings_group.representativeItem.image = _top_settings_group.barButtonItems.firstObject.image;
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
  UIAction *action = [UIAction actionWithTitle:title image:image identifier:value
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
  UIBarButtonItem *control = [[[UIBarButtonItem alloc]
      initWithCustomView:_mesh_select_control] autorelease];
  control.identifier = @"blender.mesh-selection";
  control.hidesSharedBackground = YES;
  if (@available(iOS 27.0, *)) {
    control.visibilityPriority = UIBarButtonItemVisibilityPriorityHigh;
    compact.visibilityPriority = UIBarButtonItemVisibilityPriorityHigh;
  }
  _mesh_select_group = [UIBarButtonItemGroup fixedGroupWithRepresentativeItem:compact
                                                                      items:@[control]];
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
  NSUInteger index = [@[@"VERT", @"EDGE", @"FACE"] indexOfObject:value ?: @""];
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
                                          command:GHOST_IOS_UI_COMMAND_SET_VIEWPORT_WIREFRAME],
             [self viewportShadingActionWithTitle:@"Solid"
                                      systemImage:@"cube"
                                          command:GHOST_IOS_UI_COMMAND_SET_VIEWPORT_SOLID],
             [self viewportShadingActionWithTitle:@"Material Preview"
                                      systemImage:@"circle.lefthalf.filled"
                                          command:GHOST_IOS_UI_COMMAND_SET_VIEWPORT_MATERIAL],
             [self viewportShadingActionWithTitle:@"Rendered"
                                      systemImage:@"camera.aperture"
                                          command:GHOST_IOS_UI_COMMAND_SET_VIEWPORT_RENDERED],
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
             [self modeActionWithTitle:@"Object Mode" command:GHOST_IOS_UI_COMMAND_SET_MODE_OBJECT],
             [self modeActionWithTitle:@"Edit Mode" command:GHOST_IOS_UI_COMMAND_SET_MODE_EDIT],
             [self modeActionWithTitle:@"Sculpt Mode" command:GHOST_IOS_UI_COMMAND_SET_MODE_SCULPT],
             [self modeActionWithTitle:@"Vertex Paint"
                                command:GHOST_IOS_UI_COMMAND_SET_MODE_VERTEX_PAINT],
             [self modeActionWithTitle:@"Weight Paint"
                                command:GHOST_IOS_UI_COMMAND_SET_MODE_WEIGHT_PAINT],
             [self modeActionWithTitle:@"Texture Paint"
                                command:GHOST_IOS_UI_COMMAND_SET_MODE_TEXTURE_PAINT],
           ]];
  button.menu = mode_menu;
  button.menuRepresentation = mode_menu;
  button.image = ((UIAction *)mode_menu.children.firstObject).image;
  button.accessibilityValue = button.title;
  return button;
}

- (void)updateActionsButtonMenu
{
  if (_actions_button == nil) {
    return;
  }

  if (_actions_button.menu != nil) {
    return;
  }

  /* The iPadOS menu bar has all of Blender's menus; this button keeps the essentials. Submenus
   * load from Blender when opened, so the menu itself never needs rebuilding. */
  /* iPadOS document symbols rather than Blender's file icons. */
  UIAction *(^command)(NSString *, NSString *, const char *) =
      ^UIAction *(NSString *title, NSString *symbol, const char *op_name) {
        return [UIAction actionWithTitle:title
                                   image:[UIImage systemImageNamed:symbol]
                              identifier:nil
                                 handler:^(__kindof UIAction *) { WM_apple_ios_ui_operator_enqueue(op_name); }];
      };
  UIMenu *(^submenu)(NSString *, NSString *, NSString *) = ^UIMenu *(NSString *title, NSString *symbol, NSString *menu) {
    UIMenu *live = blenderIOSNativeSubmenu(@{@"title": title, @"route": @{@"root": menu, @"path": @[]}}, nil);
    return [UIMenu menuWithTitle:title image:[UIImage systemImageNamed:symbol] identifier:nil options:0
                        children:live.children];
  };
  UIMenu *(^section)(NSArray *) = ^UIMenu *(NSArray *children) {
    return [UIMenu menuWithTitle:@"" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:children];
  };
  UIAction *desktop = [UIAction actionWithTitle:@"Switch to Desktop Interface"
                                          image:[UIImage systemImageNamed:@"macwindow"]
                                     identifier:@"blender.interface.desktop"
                                        handler:^(__kindof UIAction *) {
                                          WM_apple_ios_ui_setting_enqueue("interface", "desktop");
                                        }];
  UIMenu *menu = [UIMenu menuWithTitle:@"Blender" children:@[
    section(@[submenu(@"New", @"doc.badge.plus", @"TOPBAR_MT_file_new"),
              command(@"Open…", @"folder", "WM_OT_open_mainfile"),
              submenu(@"Open Recent", @"clock", @"TOPBAR_MT_file_open_recent")]),
    section(@[command(@"Save", @"square.and.arrow.down", "WM_OT_save_mainfile"),
              command(@"Save As…", @"square.and.arrow.down.on.square", "WM_OT_save_as_mainfile")]),
    section(@[desktop]),
  ]];
  _actions_button.menu = menu;
  _actions_button.menuRepresentation = menu;
  _actions_button.enabled = YES;
}

- (UIBarButtonItem *)actionsButton
{
  UIBarButtonItem *button = [[[UIBarButtonItem alloc]
      initWithImage:[UIImage systemImageNamed:@"ellipsis.circle"]
               menu:nil] autorelease];
  button.title = @"Menus";
  button.accessibilityLabel = @"Blender Menus";
  button.identifier = @"blender.actions";
  return button;
}

- (void)installNativeTopBar
{
  if (self.navigationController) {
    _top_bar = (GHOSTIOSNavigationBar *)[self.navigationController.navigationBar retain];
    _top_bar_item = [self.navigationItem retain];
  }
  else {
    _top_bar = [[GHOSTIOSNavigationBar alloc] initWithFrame:CGRectZero];
    _top_bar.translatesAutoresizingMaskIntoConstraints = NO;
    _top_bar_item = [[UINavigationItem alloc] initWithTitle:@""];
  }
  _top_bar.tintColor = UIColor.systemBlueColor;
  _top_bar.accessibilityIdentifier = @"blender.top-toolbar";
  _top_bar_item.style = UINavigationItemStyleEditor;

  _mode_button = [self modePickerButton];
  _actions_button = [self actionsButton];
  _viewport_shading_button = [self viewportShadingButton];
  _undo_button = [self undoButton];
  [self updateActionsButtonMenu];
  [self setViewportShadingValue:@"solid"];

  /* Real bar items provide UIKit with actions and menu representations for overflow.
   * Group scene actions beside Mode; keep viewport shading on the trailing side. */
  _top_bar_item.leadingItemGroups = @[
    _mode_button.creatingFixedGroup,
    _assets.button.creatingFixedGroup,
    [self meshSelectModeControlGroup],
    _actions_button.creatingFixedGroup,
    _undo_button.creatingFixedGroup,
  ];
  if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
    _finger_input_button = [self fingerInputButton];
    [self setFingerInputMode:YES];
    _top_bar_item.leadingItemGroups = [_top_bar_item.leadingItemGroups
        arrayByAddingObject:_finger_input_button.creatingFixedGroup];
  }
  _top_bar_item.centerItemGroups = @[ [self topSettingsControlGroup] ];
  _top_bar_item.trailingItemGroups = @[
    [UIBarButtonItemGroup fixedGroupWithRepresentativeItem:nil
        items:@[_scene_panels.hierarchyButton, _timeline.button,
                 _scene_panels.inspectorButton]],
  ];
  _viewport_shading_button.sharesBackground = NO;
  _top_bar_item.pinnedTrailingGroup = _viewport_shading_button.creatingFixedGroup;
  if (@available(iOS 27.0, *)) {
    _mode_button.visibilityPriority = UIBarButtonItemVisibilityPriorityHigh;
    _undo_button.visibilityPriority = UIBarButtonItemVisibilityPriorityHigh;
    _viewport_shading_button.visibilityPriority = UIBarButtonItemVisibilityPriorityHigh;
    _actions_button.visibilityPriority = UIBarButtonItemVisibilityPriorityLow;
  }
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 270100
  if (@available(iOS 27.1, *)) {
    _top_bar_item.verticalBarCompressionBehavior = UIVerticalBarCompressionBehaviorPrefersBarItems;
  }
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

- (void)installIPadOverlay
{
  if (_overlay_view != nil) {
    return;
  }

  _overlay_view = [[GHOSTIOSPassThroughOverlayView alloc] initWithFrame:self.view.bounds];
  _overlay_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  _overlay_view.backgroundColor = UIColor.clearColor;
  [self.view addSubview:_overlay_view];

  _mesh_select_buttons = [[NSMutableArray alloc] init];
  _redo_strip = [[GHOSTIOSRedoStrip alloc] initWithOverlay:_overlay_view icon:^UIImage *(NSString *name) {
    return GHOSTIOSBlenderIcon(name, NO);
  }];
  _scene_panels = [[GHOSTIOSScenePanels alloc] initWithOverlay:_overlay_view icon:^UIImage *(NSString *name) {
    return GHOSTIOSBlenderIcon(name, NO);
  }];
  _scene_panels.menuProvider = ^UIMenu *(NSDictionary *property) {
    NSMutableDictionary *route = [NSMutableDictionary dictionaryWithDictionary:
        @{@"root": property[@"menu"], @"path": @[]}];
    if (property[@"tab"]) { route[@"propertiesTab"] = property[@"tab"]; }
    return blenderIOSNativeSubmenu(@{@"title": property[@"label"],
                                    @"icon": property[@"icon"] ?: @"",
                                    @"route": route},
                                   property[@"menu"]);
  };
  _timeline = [[GHOSTIOSTimeline alloc] initWithOverlay:_overlay_view];
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
  _assets = [[GHOSTIOSAssets alloc] initWithOverlay:_overlay_view];
  _assets.visibilityDidChange = relayout_tray;
  /* Assets dragged out of the pane drop onto the viewport, which Blender draws in this view. */
  __block GHOST_IOSViewController *assets_host = self;
  _assets.clientPoint = ^CGPoint(CGPoint point) {
    GHOST_WindowIOS *ghost_window = assets_host->_ghost_window;
    return ghost_window ? ghost_window->scalePointToWindow(point) : point;
  };
  [self.view addInteraction:_assets.dropInteraction];
  [self installNativeTopBar];
  _pencil_menu = [[GHOSTIOSPencilMenu alloc] initWithFrame:_overlay_view.bounds];
  _pencil_menu.delegate = self;
  const BOOL phone = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone;
  [(GHOSTUIWindow *)_ghost_window->rootWindow setPencilContextMenu:_pencil_menu];
  _toolbar_buttons = [[NSMutableArray alloc] init];
  _action_buttons = [[NSMutableArray alloc] init];
  _view_pie_buttons = [[NSMutableArray alloc] init];
  _toolbar_expanded = NO;
  _view_pie_visible = NO;

  _tool_rail_glass_effect = [[UIGlassEffect effectWithStyle:UIGlassEffectStyleClear] retain];
  _tool_rail_glass_effect.interactive = YES;
  _tool_rail =
      [[[GHOSTIOSPassThroughVisualEffectView alloc] initWithEffect:_tool_rail_glass_effect] autorelease];
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

  _tool_drag_handle = [self toolbarDragHandle];
  [_tool_rail.contentView addSubview:_tool_drag_handle];
  _tool_drag_handle_height_constraint = [_tool_drag_handle.heightAnchor constraintEqualToConstant:0.0];

  _tool_scroll = [[GHOSTIOSButtonScrollView alloc] initWithFrame:CGRectZero];
  UIHoverGestureRecognizer *tool_hover = [[UIHoverGestureRecognizer alloc]
      initWithTarget:self action:@selector(handleToolbarHover:)];
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

  _tool_stack = [[GHOSTIOSToolStackView alloc] initWithFrame:CGRectZero];
  _tool_stack.axis = UILayoutConstraintAxisVertical;
  _tool_stack.alignment = UIStackViewAlignmentFill;
  _tool_stack.distribution = UIStackViewDistributionEqualSpacing;
  _tool_stack.spacing = 2.0;
  _tool_stack.translatesAutoresizingMaskIntoConstraints = NO;
  [_tool_scroll addSubview:_tool_stack];
  [self installBrushRail];

  _action_rail = [[[GHOSTIOSPassThroughVisualEffectView alloc] initWithEffect:nil] autorelease];
  _action_rail.translatesAutoresizingMaskIntoConstraints = NO;
  _action_rail.backgroundColor = UIColor.clearColor;
  _action_rail.clipsToBounds = YES;
  _action_rail.layer.cornerRadius = 0.0;
  _action_rail.layer.cornerCurve = kCACornerCurveContinuous;
  _action_rail.hidden = YES;
  _action_rail.alpha = 0.0;
  [_overlay_view addSubview:_action_rail];

  _action_scroll = [[GHOSTIOSFadingButtonScrollView alloc] initWithFrame:CGRectZero];
  _action_scroll.translatesAutoresizingMaskIntoConstraints = NO;
  _action_scroll.backgroundColor = UIColor.clearColor;
  _action_scroll.clipsToBounds = YES;
  _action_scroll.showsHorizontalScrollIndicator = NO;
  _action_scroll.alwaysBounceHorizontal = NO;
  _action_scroll.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
  [_action_rail.contentView addSubview:_action_scroll];

  _action_stack = [[GHOSTIOSPassThroughStackView alloc] initWithFrame:CGRectZero];
  _action_stack.axis = UILayoutConstraintAxisHorizontal;
  _action_stack.alignment = UIStackViewAlignmentCenter;
  _action_stack.distribution = UIStackViewDistributionFill;
  _action_stack.spacing = 4.0;
  _action_stack.translatesAutoresizingMaskIntoConstraints = NO;
  [_action_scroll addSubview:_action_stack];
  [self installActionStripToggle];

  NSArray *view_pie_specs = @[
    @{@"title" : @"Frame All",
      @"symbol" : @"rectangle.expand.vertical",
      @"command" : @(GHOST_IOS_UI_COMMAND_VIEW_ALL)},
    @{@"title" : @"Frame Selected",
      @"symbol" : @"viewfinder",
      @"command" : @(GHOST_IOS_UI_COMMAND_FRAME_SELECTED)},
    @{@"title" : @"Camera",
      @"symbol" : @"camera",
      @"command" : @(GHOST_IOS_UI_COMMAND_VIEW_CAMERA)},
    @{@"title" : @"Perspective",
      @"symbol" : @"cube.transparent",
      @"command" : @(GHOST_IOS_UI_COMMAND_VIEW_PERSP_ORTHO)},
    @{@"title" : @"Local View",
      @"symbol" : @"scope",
      @"command" : @(GHOST_IOS_UI_COMMAND_VIEW_LOCAL)},
    @{@"title" : @"Front",
      @"symbol" : @"arrow.up.square",
      @"command" : @(GHOST_IOS_UI_COMMAND_VIEW_FRONT)},
    @{@"title" : @"Right",
      @"symbol" : @"arrow.right.square",
      @"command" : @(GHOST_IOS_UI_COMMAND_VIEW_RIGHT)},
    @{@"title" : @"Top",
      @"symbol" : @"arrow.down.to.line.square",
      @"command" : @(GHOST_IOS_UI_COMMAND_VIEW_TOP)},
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
  _view_gizmo_selection_feedback = [[UISelectionFeedbackGenerator
      feedbackGeneratorForView:_view_gizmo_hotspot] retain];
  _view_gizmo_open_feedback = [[UIImpactFeedbackGenerator
      feedbackGeneratorWithStyle:UIImpactFeedbackStyleSoft forView:_view_gizmo_hotspot] retain];
  _view_gizmo_feedback = [[[UIView alloc] initWithFrame:CGRectMake(14, 14, 84, 84)] autorelease];
  _view_gizmo_feedback.userInteractionEnabled = NO;
  _view_gizmo_feedback.layer.cornerRadius = 42;
  _view_gizmo_feedback.layer.borderWidth = 1;
  _view_gizmo_feedback.alpha = 0;
  [_view_gizmo_hotspot addSubview:_view_gizmo_feedback];
  [gizmo_control addTarget:self action:@selector(handleViewGizmoPress:)
          forControlEvents:UIControlEventTouchDown];
  [gizmo_control addTarget:self action:@selector(handleViewGizmoRelease:)
          forControlEvents:UIControlEventTouchUpInside |
                           UIControlEventTouchUpOutside | UIControlEventTouchCancel];

  UIHoverGestureRecognizer *view_gizmo_hover = [[UIHoverGestureRecognizer alloc]
      initWithTarget:self action:@selector(handleViewGizmoHover:)];
  [_view_gizmo_hotspot addGestureRecognizer:view_gizmo_hover];
  [view_gizmo_hover release];

  UILongPressGestureRecognizer *view_gizmo_hold = [[UILongPressGestureRecognizer alloc]
      initWithTarget:self action:@selector(handleViewGizmoHold:)];
  view_gizmo_hold.minimumPressDuration = 0.32;
  view_gizmo_hold.allowableMovement = 10.0;
  view_gizmo_hold.allowedTouchTypes = @[ @(UITouchTypeDirect), @(UITouchTypePencil) ];
  [_view_gizmo_hotspot addGestureRecognizer:view_gizmo_hold];

  UITapGestureRecognizer *view_gizmo_tap = [[UITapGestureRecognizer alloc]
      initWithTarget:self action:@selector(handleViewGizmoTap:)];
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
      name:UIApplicationWillResignActiveNotification object:nil];

  UILayoutGuide *safe_area = _overlay_view.safeAreaLayoutGuide;
  /* The radial view is frontmost and passes through when the palette is closed. */
  [_overlay_view addSubview:_pencil_menu];
  _tool_rail_width_constraint = [_tool_rail.widthAnchor constraintEqualToConstant:64.0];
  _tool_rail_height_constraint = [_tool_rail.heightAnchor constraintEqualToConstant:64.0];
  _tool_rail_bottom_constraint = [[_tool_rail.bottomAnchor constraintEqualToAnchor:safe_area.bottomAnchor
                                                                          constant:-12.0] retain];
  _tool_rail_center_y_constraint = [[_tool_rail.centerYAnchor constraintEqualToAnchor:safe_area.centerYAnchor]
      retain];
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
    [_tool_drag_handle.trailingAnchor constraintEqualToAnchor:_tool_rail.contentView.trailingAnchor],
    [_tool_drag_handle.topAnchor constraintEqualToAnchor:_tool_rail.contentView.topAnchor],
    _tool_drag_handle_height_constraint,
    [_tool_scroll.topAnchor constraintEqualToAnchor:_tool_drag_handle.bottomAnchor],
    [_tool_scroll.bottomAnchor constraintEqualToAnchor:_tool_rail.contentView.bottomAnchor],

    [_tool_stack.leadingAnchor constraintEqualToAnchor:_tool_scroll.contentLayoutGuide.leadingAnchor
                                             constant:6.0],
    [_tool_stack.trailingAnchor constraintEqualToAnchor:_tool_scroll.contentLayoutGuide.trailingAnchor
                                              constant:-6.0],
    [_tool_stack.topAnchor constraintEqualToAnchor:_tool_scroll.contentLayoutGuide.topAnchor constant:6.0],
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
    [_action_stack.centerXAnchor constraintEqualToAnchor:_action_scroll.contentLayoutGuide.centerXAnchor],
    [_action_stack.leadingAnchor
        constraintGreaterThanOrEqualToAnchor:_action_scroll.contentLayoutGuide.leadingAnchor constant:6.0],
    [_action_stack.trailingAnchor
        constraintLessThanOrEqualToAnchor:_action_scroll.contentLayoutGuide.trailingAnchor constant:-6.0],
    [_action_stack.topAnchor constraintEqualToAnchor:_action_scroll.contentLayoutGuide.topAnchor
                                           constant:6.0],
    [_action_stack.bottomAnchor constraintEqualToAnchor:_action_scroll.contentLayoutGuide.bottomAnchor
                                              constant:-6.0],
    [_action_stack.heightAnchor constraintEqualToAnchor:_action_scroll.frameLayoutGuide.heightAnchor
                                             constant:-12.0],

  ]];
  [self installBrushRailLayout];
  [self installActionStripLayout];

  [self layoutViewPie];
}

- (void)handleViewGizmoTap:(UITapGestureRecognizer *)gesture
{
  [(GHOSTUIWindow *)_view.window handleNavigationTap:gesture];
}

- (void)handleOverlayCommand:(UIButton *)sender
{
  if ([_view_pie_buttons containsObject:sender]) {
    [self setViewPieVisible:NO animated:YES];
  }
  WM_apple_ios_ui_command_enqueue((int)sender.tag);
}

#pragma mark - Brush Rail

- (void)installBrushRail
{
  _brush_buttons = [[NSMutableArray alloc] init];
  _brush_rail =
      [[[GHOSTIOSPassThroughVisualEffectView alloc] initWithEffect:_tool_rail_glass_effect] autorelease];
  _brush_rail.translatesAutoresizingMaskIntoConstraints = NO;
  _brush_rail.clipsToBounds = YES;
  _brush_rail.layer.cornerRadius = 32.0;
  _brush_rail.layer.cornerCurve = kCACornerCurveContinuous;
  _brush_rail.accessibilityIdentifier = @"blender.brushbar";
  _brush_rail.hidden = YES;
  _brush_rail.alpha = 0.0;
  [_overlay_view addSubview:_brush_rail];

  UIPanGestureRecognizer *dock_pan = [[UIPanGestureRecognizer alloc]
      initWithTarget:self action:@selector(handleBrushDockPan:)];
  dock_pan.maximumNumberOfTouches = 1;
  dock_pan.cancelsTouchesInView = YES;
  dock_pan.delaysTouchesBegan = NO;
  dock_pan.delegate = self;
  [_brush_rail addGestureRecognizer:dock_pan];
  [dock_pan release];

  _brush_drag_handle = [self toolbarDragHandle];
  _brush_drag_handle.accessibilityLabel = @"Collapse Brushes";
  _brush_drag_handle.accessibilityIdentifier = @"blender.brushbar.handle";
  for (id<UIInteraction> interaction in [[_brush_drag_handle.interactions copy] autorelease]) {
    if ([(id)interaction isKindOfClass:UIToolTipInteraction.class]) {
      [_brush_drag_handle removeInteraction:interaction];
    }
  }
  [_brush_drag_handle addInteraction:[[[UIToolTipInteraction alloc]
      initWithDefaultToolTip:@"Collapse Brushes"] autorelease]];
  [_brush_drag_handle removeTarget:self action:nil forControlEvents:UIControlEventAllEvents];
  [_brush_drag_handle addTarget:self action:@selector(collapseBrushRailFromHandle:)
               forControlEvents:UIControlEventTouchUpInside];
  for (UIGestureRecognizer *gesture in _brush_drag_handle.gestureRecognizers) {
    [gesture removeTarget:self action:nil];
    [gesture addTarget:self action:@selector(handleBrushHandlePan:)];
  }
  [_brush_rail.contentView addSubview:_brush_drag_handle];
  _brush_drag_handle_height_constraint = [_brush_drag_handle.heightAnchor constraintEqualToConstant:0.0];

  _brush_scroll = [[GHOSTIOSButtonScrollView alloc] initWithFrame:CGRectZero];
  UIHoverGestureRecognizer *hover = [[UIHoverGestureRecognizer alloc]
      initWithTarget:self action:@selector(handleBrushHover:)];
  [_brush_scroll addGestureRecognizer:hover];
  [hover release];
  _brush_scroll.translatesAutoresizingMaskIntoConstraints = NO;
  _brush_scroll.backgroundColor = UIColor.clearColor;
  _brush_scroll.clipsToBounds = YES;
  _brush_scroll.showsVerticalScrollIndicator = NO;
  _brush_scroll.alwaysBounceVertical = YES;
  _brush_scroll.canCancelContentTouches = YES;
  _brush_scroll.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
  [_brush_rail.contentView addSubview:_brush_scroll];

  _brush_stack = [[GHOSTIOSToolStackView alloc] initWithFrame:CGRectZero];
  _brush_stack.axis = UILayoutConstraintAxisVertical;
  _brush_stack.alignment = UIStackViewAlignmentFill;
  _brush_stack.distribution = UIStackViewDistributionEqualSpacing;
  _brush_stack.spacing = 2.0;
  _brush_stack.translatesAutoresizingMaskIntoConstraints = NO;
  [_brush_scroll addSubview:_brush_stack];
}

- (void)installBrushRailLayout
{
  UILayoutGuide *safe_area = _overlay_view.safeAreaLayoutGuide;
  /* The brush rail shares the tool rail's column, stacked directly above it.
   * #updateBrushRailGeometry divides the column's height between both rails. */
  _brush_rail_leading_constraint = [_brush_rail.leadingAnchor
      constraintEqualToAnchor:safe_area.leadingAnchor constant:14.0];
  _brush_rail_width_constraint = [_brush_rail.widthAnchor constraintEqualToConstant:0.0];
  _brush_rail_height_constraint = [_brush_rail.heightAnchor constraintEqualToConstant:0.0];
  _brush_rail_bottom_constraint = [[_brush_rail.bottomAnchor
      constraintEqualToAnchor:_tool_rail.topAnchor constant:0.0] retain];
  [NSLayoutConstraint activateConstraints:@[
    _brush_rail_leading_constraint,
    _brush_rail_width_constraint,
    _brush_rail_height_constraint,
    _brush_rail_bottom_constraint,

    [_brush_drag_handle.leadingAnchor constraintEqualToAnchor:_brush_rail.contentView.leadingAnchor],
    [_brush_drag_handle.trailingAnchor constraintEqualToAnchor:_brush_rail.contentView.trailingAnchor],
    [_brush_drag_handle.topAnchor constraintEqualToAnchor:_brush_rail.contentView.topAnchor],
    _brush_drag_handle_height_constraint,
    [_brush_scroll.leadingAnchor constraintEqualToAnchor:_brush_rail.contentView.leadingAnchor],
    [_brush_scroll.trailingAnchor constraintEqualToAnchor:_brush_rail.contentView.trailingAnchor],
    [_brush_scroll.topAnchor constraintEqualToAnchor:_brush_drag_handle.bottomAnchor],
    [_brush_scroll.bottomAnchor constraintEqualToAnchor:_brush_rail.contentView.bottomAnchor],

    [_brush_stack.leadingAnchor constraintEqualToAnchor:_brush_scroll.contentLayoutGuide.leadingAnchor
                                               constant:6.0],
    [_brush_stack.trailingAnchor constraintEqualToAnchor:_brush_scroll.contentLayoutGuide.trailingAnchor
                                                constant:-6.0],
    [_brush_stack.topAnchor constraintEqualToAnchor:_brush_scroll.contentLayoutGuide.topAnchor constant:6.0],
    [_brush_stack.bottomAnchor constraintEqualToAnchor:_brush_scroll.contentLayoutGuide.bottomAnchor
                                              constant:-6.0],
    [_brush_stack.widthAnchor constraintEqualToAnchor:_brush_scroll.frameLayoutGuide.widthAnchor
                                             constant:-12.0],
  ]];
}

- (BOOL)isBrushListVisible
{
  return _brush_rail_expanded || _brush_rail_docked_left_center;
}

- (void)updateBrushRailGeometry
{
  if (_brush_rail_width_constraint == nil) {
    return;
  }
  const BOOL list_visible = [self isBrushListVisible];
  _brush_rail_width_constraint.constant = !_brush_rail_available ? 0.0 :
                                          _brush_rail_expanded ? [self expandedToolbarWidth] :
                                                                 64.0;
  if (!_brush_rail_available) {
    /* The tool rail owns the column alone, with its own docking constraints. */
    _brush_rail_height_constraint.constant = 0.0;
    _brush_rail_bottom_constraint.constant = 0.0;
    _tool_rail_bottom_constraint.constant = -12.0 - _bottom_tray;
    _tool_rail_center_y_constraint.constant = -_bottom_tray / 2;
    _tool_rail_center_y_constraint.active = NO;
    _tool_rail_bottom_constraint.active = !_tool_rail_docked_left_center;
    _tool_rail_center_y_constraint.active = _tool_rail_docked_left_center;
  }
  else {
    /* Both rails share one column without overlapping: compact rails stay 64pt, an open list
     * takes the remaining height, and two open lists split it. */
    const CGFloat gap = 10.0, compact = 64.0, top_inset = 16.0, bottom_inset = 12.0;
    CGFloat safe_height = _overlay_view.safeAreaLayoutGuide.layoutFrame.size.height;
    if (safe_height <= 0.0) {
      safe_height = self.view.bounds.size.height;
    }
    /* The rails share the column above the open asset pane. */
    safe_height = MAX(0.0, safe_height - _bottom_tray);
    const CGFloat column = MAX(2 * compact, safe_height - top_inset - bottom_inset - gap);
    const CGFloat max_list = [self expandedToolbarHeight];
    const BOOL tool_list = [self isToolbarListVisible];
    CGFloat tool_height = compact, brush_height = compact;
    if (tool_list && list_visible) {
      tool_height = brush_height = MAX(compact, MIN(max_list, floor(column / 2)));
    }
    else if (tool_list) {
      tool_height = MAX(compact, MIN(max_list, column - compact));
    }
    else if (list_visible) {
      brush_height = MAX(compact, MIN(max_list, column - compact));
    }
    _tool_rail_height_constraint.constant = tool_height;
    _brush_rail_height_constraint.constant = brush_height;
    _brush_rail_bottom_constraint.constant = -gap;
    /* Docking either rail to the center centers the pair; otherwise it rests at the bottom. */
    const BOOL centered = _tool_rail_docked_left_center || _brush_rail_docked_left_center;
    const CGFloat group = tool_height + gap + brush_height;
    _tool_rail_center_y_constraint.active = NO;
    _tool_rail_bottom_constraint.active = YES;
    _tool_rail_bottom_constraint.constant =
        -(centered ? MAX(bottom_inset, (safe_height - group) / 2) : bottom_inset) - _bottom_tray;
  }
  _brush_drag_handle.hidden = !list_visible;
  _brush_drag_handle_height_constraint.constant = list_visible ? 44.0 : 0.0;
  _brush_scroll.scrollEnabled = list_visible;
}

- (void)updateBrushRailWithModel:(id)model
{
  NSArray *items = [model isKindOfClass:NSDictionary.class] ? model[@"items"] : nil;
  NSString *active = [model isKindOfClass:NSDictionary.class] ? model[@"active"] : nil;
  if (![items isKindOfClass:NSArray.class]) {
    items = @[];
  }
  if (![active isKindOfClass:NSString.class]) {
    active = @"";
  }
  /* While asset libraries reload, keep showing the rail rather than flickering it away. */
  const BOOL loading = [model isKindOfClass:NSDictionary.class] && [model[@"loading"] boolValue];
  [self setBrushRailAvailable:items.count > 0 || (loading && _brush_rail_available)];
  if (loading && items.count == 0) {
    return;
  }

  NSData *data = [NSJSONSerialization dataWithJSONObject:items options:NSJSONWritingSortedKeys error:nil];
  NSString *signature = [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
  if (_brush_signature == nil || ![_brush_signature isEqualToString:signature ?: @""]) {
    [_brush_signature release];
    _brush_signature = [signature ?: @"" copy];
    [self rebuildBrushRailWithItems:items];
  }
  [self setActiveBrushIdentifier:active];
}

- (void)setBrushRailAvailable:(BOOL)available
{
  if (_brush_rail_available == available) {
    return;
  }
  _brush_rail_available = available;
  if (!available) {
    _brush_rail_expanded = NO;
    _brush_press_select_active = NO;
    [self setBrushHoverButton:nil];
    if ([_toolbar_label_button isDescendantOfView:_brush_rail]) {
      [self hideToolbarLabelAnimated:NO];
    }
  }
  if (available) {
    _brush_rail.hidden = NO;
  }
  /* Recomputes the tool rail's own height first, then the shared column. */
  [self updateToolbarHeightForCurrentState];
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0.0 : 0.24
      delay:0.0
      options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
      animations:^{
        self->_brush_rail.alpha = available ? 1.0 : 0.0;
        [self->_overlay_view layoutIfNeeded];
      }
      completion:^(BOOL finished) {
        if (!self->_brush_rail_available) {
          self->_brush_rail.hidden = YES;
        }
      }];
}

- (UIButton *)brushButtonForItem:(NSDictionary *)item
{
  NSString *title = item[@"title"];
  NSString *preview = [item[@"preview"] isKindOfClass:NSString.class] ? item[@"preview"] : @"";
  UIImage *image = blenderIOSBrushImage(preview, 36.0);

  UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
  button.tintColor = UIColor.labelColor;
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
  button.accessibilityLabel = title;
  button.accessibilityIdentifier = item[@"id"];
  button.accessibilityHint = @"Touch and hold to choose from all brushes";
  button.configurationUpdateHandler = ^(UIButton *updated_button) {
    const BOOL shows_text = self->_brush_rail_expanded;
    const BOOL is_emphasized = [self isBrushListVisible] &&
                               (updated_button.selected ||
                                updated_button == self->_brush_hover_button);
    UIButtonConfiguration *configuration = is_emphasized && !updated_button.selected ?
                                              [UIButtonConfiguration glassButtonConfiguration] :
                                              [UIButtonConfiguration plainButtonConfiguration];
    configuration.baseForegroundColor = updated_button.selected ? UIColor.whiteColor : UIColor.labelColor;
    configuration.automaticallyUpdateForSelection = NO;
    configuration.attributedTitle = shows_text ? [[[NSAttributedString alloc]
        initWithString:title attributes:@{NSForegroundColorAttributeName: configuration.baseForegroundColor}]
        autorelease] : nil;
    configuration.image = image;
    configuration.imagePlacement = NSDirectionalRectEdgeLeading;
    configuration.imagePadding = shows_text ? 10.0 : 0.0;
    configuration.contentInsets = shows_text ?
                                      NSDirectionalEdgeInsetsMake(6.0, 8.0, 6.0, 14.0) :
                                      NSDirectionalEdgeInsetsMake(6.0, 6.0, 6.0, 6.0);
    configuration.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    updated_button.contentHorizontalAlignment = shows_text ?
                                                    UIControlContentHorizontalAlignmentLeading :
                                                    UIControlContentHorizontalAlignmentCenter;
    updated_button.configuration = configuration;
  };
  [button addTarget:self action:@selector(handleBrushButton:)
      forControlEvents:UIControlEventTouchUpInside];

  UILongPressGestureRecognizer *press_select = [[UILongPressGestureRecognizer alloc]
      initWithTarget:self action:@selector(handleBrushPressSelectGesture:)];
  press_select.minimumPressDuration = 0.22;
  press_select.allowableMovement = 10.0;
  press_select.cancelsTouchesInView = YES;
  press_select.delegate = self;
  [button addGestureRecognizer:press_select];
  [press_select release];

  [button.heightAnchor constraintEqualToConstant:52.0].active = YES;
  [button setNeedsUpdateConfiguration];
  return button;
}

- (void)rebuildBrushRailWithItems:(NSArray *)items
{
  NSString *label_brush = [_toolbar_label_button isDescendantOfView:_brush_rail] ?
      [[_toolbar_label_button.accessibilityIdentifier copy] autorelease] : nil;
  if (label_brush) {
    _toolbar_label_button = nil;
  }
  for (UIView *view in [[_brush_stack.arrangedSubviews copy] autorelease]) {
    [_brush_stack removeArrangedSubview:view];
    [view removeFromSuperview];
  }
  [_brush_buttons removeAllObjects];
  _brush_hover_button = nil;

  NSString *previous_catalog = nil;
  for (NSDictionary *item in items) {
    if (![item isKindOfClass:NSDictionary.class] || ![item[@"id"] isKindOfClass:NSString.class] ||
        ![item[@"title"] isKindOfClass:NSString.class] || [item[@"id"] length] == 0)
    {
      continue;
    }
    /* Separate catalogs the way the tool rail separates tool groups. */
    NSString *catalog = [item[@"catalog"] isKindOfClass:NSString.class] ? item[@"catalog"] : @"";
    UIView *previous = _brush_stack.arrangedSubviews.lastObject;
    if (previous && ![catalog isEqualToString:previous_catalog ?: @""]) {
      [_brush_stack setCustomSpacing:12.0 afterView:previous];
    }
    previous_catalog = catalog;

    UIButton *button = [self brushButtonForItem:item];
    [_brush_buttons addObject:button];
    [_brush_stack addArrangedSubview:button];
    if ([button.accessibilityIdentifier isEqualToString:label_brush]) {
      _toolbar_label_button = button;
    }
  }
  [self updateBrushRailVisibilityAnimated:NO];
  if (_toolbar_label_button) {
    [self layoutToolbarLabel];
  }
  else if (label_brush) {
    [self hideToolbarLabelAnimated:NO];
  }
}

- (void)setActiveBrushIdentifier:(NSString *)brushIdentifier
{
  if ([_active_brush_identifier isEqualToString:brushIdentifier]) {
    return;
  }
  [_active_brush_identifier release];
  _active_brush_identifier = [brushIdentifier copy];
  [self updateBrushRailVisibilityAnimated:NO];
  [UIView performWithoutAnimation:^{
    for (UIButton *button in self->_brush_buttons) {
      [button updateConfiguration];
    }
  }];
}

- (void)updateBrushRailVisibilityAnimated:(BOOL)animated
{
  const BOOL list_visible = [self isBrushListVisible];
  UIButton *active_button = nil;
  for (UIButton *button in _brush_buttons) {
    if ([button.accessibilityIdentifier isEqualToString:_active_brush_identifier]) {
      active_button = button;
      break;
    }
  }
  /* The compact rail shows the active brush, or the first one while none is active. */
  if (active_button == nil) {
    active_button = _brush_buttons.firstObject;
  }

  for (UIButton *button in _brush_buttons) {
    button.hidden = NO;
    if (list_visible && button.alpha < 1.0) {
      button.alpha = 0.0;
    }
    [button setNeedsUpdateConfiguration];
  }

  void (^applyVisibility)(void) = ^{
    for (UIButton *button in self->_brush_buttons) {
      const BOOL is_active = button == active_button;
      button.selected = list_visible && is_active &&
                        [button.accessibilityIdentifier isEqualToString:self->_active_brush_identifier];
      [button setNeedsUpdateConfiguration];
      button.alpha = (list_visible || is_active) ? 1.0 : 0.0;
    }
    [self->_brush_stack setNeedsLayout];
  };
  void (^finishCompact)(void) = ^{
    if ([self isBrushListVisible]) {
      return;
    }
    for (UIButton *button in self->_brush_buttons) {
      button.selected = NO;
      button.hidden = button != active_button;
      [button setNeedsUpdateConfiguration];
    }
    [self->_brush_scroll setContentOffset:CGPointZero animated:NO];
  };

  [self updateBrushRailGeometry];
  if (!animated) {
    applyVisibility();
    finishCompact();
    [_overlay_view layoutIfNeeded];
    if (list_visible && active_button) {
      [_brush_scroll scrollRectToVisible:CGRectInset(active_button.frame, 0, -12) animated:NO];
    }
    return;
  }
  [UIView animateWithDuration:0.32
                        delay:0.0
       usingSpringWithDamping:0.84
        initialSpringVelocity:0.0
                      options:UIViewAnimationOptionCurveEaseInOut |
                              UIViewAnimationOptionAllowUserInteraction |
                              UIViewAnimationOptionBeginFromCurrentState
                   animations:^{
                     applyVisibility();
                     [self->_overlay_view layoutIfNeeded];
                     if (list_visible && active_button) {
                       [self->_brush_scroll
                           scrollRectToVisible:CGRectInset(active_button.frame, 0, -12)
                                      animated:NO];
                     }
                   }
                   completion:^(BOOL finished) {
                     finishCompact();
                   }];
}

- (void)setBrushRailExpanded:(BOOL)expanded animated:(BOOL)animated
{
  if (_brush_rail_expanded == expanded && animated) {
    return;
  }
  _brush_rail_expanded = expanded;
  [self hideToolbarLabelAnimated:NO];
  [self updateBrushRailVisibilityAnimated:animated];
}

- (void)setBrushRailDockedLeftCenter:(BOOL)dockedLeftCenter animated:(BOOL)animated
{
  if (_brush_rail_bottom_constraint == nil) {
    return;
  }
  _brush_rail_docked_left_center = dockedLeftCenter;
  [self updateBrushRailVisibilityAnimated:animated];
  void (^applyDock)(void) = ^{
    self->_brush_rail.transform = CGAffineTransformIdentity;
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

- (void)handleBrushDockPan:(UIPanGestureRecognizer *)gesture
{
  if ([self isBrushListVisible]) {
    return;
  }
  CGPoint translation = [gesture translationInView:_overlay_view];
  if (gesture.state == UIGestureRecognizerStateChanged) {
    _brush_rail.transform = CGAffineTransformMakeTranslation(0.0, translation.y);
    return;
  }
  if (gesture.state != UIGestureRecognizerStateEnded &&
      gesture.state != UIGestureRecognizerStateCancelled &&
      gesture.state != UIGestureRecognizerStateFailed)
  {
    return;
  }
  /* Same flick threshold as the tool rail. The rail sits above the tool rail, so dock it
   * on a clear upward drag rather than by its distance to the column's center. */
  CGPoint velocity = [gesture velocityInView:_overlay_view];
  BOOL dock_left_center = translation.y < -40.0;
  if (velocity.y < -520.0) {
    dock_left_center = YES;
  }
  else if (velocity.y > 520.0) {
    dock_left_center = NO;
  }
  [self setBrushRailDockedLeftCenter:dock_left_center animated:YES];
}

- (void)collapseBrushRailFromHandle:(id)sender
{
  UNUSED_VARS(sender);
  [self hideToolbarLabelAnimated:YES];
  _brush_press_select_active = NO;
  _brush_rail_expanded = NO;
  [self setBrushHoverButton:nil];
  [self setBrushRailDockedLeftCenter:NO animated:YES];
}

- (void)handleBrushHandlePan:(UIPanGestureRecognizer *)gesture
{
  CGPoint translation = [gesture translationInView:_overlay_view];
  if (gesture.state == UIGestureRecognizerStateChanged) {
    _brush_rail.transform = CGAffineTransformMakeTranslation(
        0.0, MIN(32.0, MAX(0.0, translation.y * 0.3)));
  }
  else if (gesture.state == UIGestureRecognizerStateEnded ||
           gesture.state == UIGestureRecognizerStateCancelled ||
           gesture.state == UIGestureRecognizerStateFailed)
  {
    CGPoint velocity = [gesture velocityInView:_overlay_view];
    if (gesture.state == UIGestureRecognizerStateEnded &&
        (translation.y > 36.0 || (translation.y > 8.0 && velocity.y > 420.0)))
    {
      [self collapseBrushRailFromHandle:gesture];
    }
    else {
      [UIView animateWithDuration:0.2 animations:^{
        self->_brush_rail.transform = CGAffineTransformIdentity;
      }];
    }
  }
}

- (void)activateBrushButton:(UIButton *)button
{
  NSString *brush_id = button.accessibilityIdentifier;
  if (![brush_id isKindOfClass:NSString.class] || brush_id.length == 0) {
    return;
  }
  [self setActiveBrushIdentifier:brush_id];
  WM_apple_ios_ui_brush_activate(brush_id.UTF8String);
}

- (void)handleBrushButton:(UIButton *)sender
{
  if (![self isBrushListVisible]) {
    /* A tap on the collapsed rail opens it to pick a different brush. */
    [self setBrushRailExpanded:YES animated:YES];
    return;
  }
  [self activateBrushButton:sender];
  if (_brush_rail_expanded && !_brush_rail_docked_left_center) {
    /* Opened by a tap: picking closes it again. A docked list stays open. */
    [self setBrushRailExpanded:NO animated:YES];
  }
  [self showToolbarLabelForButton:sender transient:YES];
}

- (UIButton *)brushButtonAtGestureLocation:(UIGestureRecognizer *)gesture
{
  CGPoint location = [gesture locationInView:_overlay_view];
  for (UIButton *button in _brush_buttons) {
    if (button.hidden || button.alpha < 0.05) {
      continue;
    }
    CGPoint local_point = [button convertPoint:location fromView:_overlay_view];
    if (CGRectContainsPoint(CGRectInset(button.bounds, -10.0, -6.0), local_point)) {
      return button;
    }
  }
  return nil;
}

- (void)setBrushHoverButton:(UIButton *)hoverButton
{
  if (_brush_hover_button == hoverButton) {
    return;
  }
  UIButton *previous = _brush_hover_button;
  _brush_hover_button = hoverButton;
  previous.highlighted = NO;
  [previous setNeedsUpdateConfiguration];
  _brush_hover_button.highlighted = YES;
  [_brush_hover_button setNeedsUpdateConfiguration];
}

- (void)handleBrushPressSelectGesture:(UILongPressGestureRecognizer *)gesture
{
  switch (gesture.state) {
    case UIGestureRecognizerStateBegan:
      if (_brush_rail_expanded) {
        return;
      }
      _brush_press_select_active = YES;
      [self setBrushRailExpanded:YES animated:YES];
      [self setBrushHoverButton:[self brushButtonAtGestureLocation:gesture]];
      break;
    case UIGestureRecognizerStateChanged:
      if (_brush_press_select_active) {
        [self setBrushHoverButton:[self brushButtonAtGestureLocation:gesture]];
      }
      break;
    case UIGestureRecognizerStateEnded: {
      if (!_brush_press_select_active) {
        return;
      }
      _brush_press_select_active = NO;
      UIButton *button = [self brushButtonAtGestureLocation:gesture] ?: _brush_hover_button;
      [self setBrushHoverButton:nil];
      if (button) {
        [self activateBrushButton:button];
      }
      [self setBrushRailExpanded:NO animated:YES];
      if (button) {
        [self showToolbarLabelForButton:button transient:YES];
      }
      break;
    }
    case UIGestureRecognizerStateCancelled:
    case UIGestureRecognizerStateFailed:
      if (!_brush_press_select_active) {
        return;
      }
      _brush_press_select_active = NO;
      [self setBrushHoverButton:nil];
      [self setBrushRailExpanded:NO animated:YES];
      break;
    default:
      break;
  }
}

- (void)handleBrushHover:(UIHoverGestureRecognizer *)gesture
{
  UIButton *hovered = nil;
  if (gesture.state == UIGestureRecognizerStateBegan ||
      gesture.state == UIGestureRecognizerStateChanged)
  {
    CGPoint location = [gesture locationInView:_brush_scroll];
    if (CGRectContainsPoint(_brush_scroll.bounds, location)) {
      for (UIView *hit = [_brush_scroll hitTest:location withEvent:nil]; hit && hit != _brush_scroll;
           hit = hit.superview)
      {
        if ([_brush_buttons containsObject:hit]) {
          hovered = (UIButton *)hit;
          break;
        }
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
  CGSize text_size = [_toolbar_label sizeThatFits:CGSizeMake(MAX(16.0, max_width - 28), CGFLOAT_MAX)];
  CGSize size = CGSizeMake(MIN(max_width, ceil(text_size.width) + 28), ceil(text_size.height) + 16);
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
                   animations:^{ self->_toolbar_label_bubble.alpha = 0; }
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
    _toolbar_label_bubble = [[[UIVisualEffectView alloc]
        initWithEffect:[UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular]] autorelease];
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
                   dispatch_get_main_queue(), ^{
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
      gesture.state == UIGestureRecognizerStateChanged) {
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

- (void)handleActionButton:(UIButton *)sender
{
  NSString *op_name = sender.accessibilityIdentifier;
  if ([op_name hasPrefix:@"blender.action."]) {
    WM_apple_ios_ui_setting_enqueue("native_action",
                                    [op_name substringFromIndex:@"blender.action.".length].UTF8String);
    return;
  }
  NSString *property = sender.accessibilityValue;
  NSString *value = sender.restorationIdentifier;
  NSString *key = sender.accessibilityHint;
  if ([@[@"X", @"Y", @"Z"] containsObject:key ?: @""]) {
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
  WM_apple_ios_ui_operator_enum_enqueue(op_name.UTF8String,
                                        property.UTF8String,
                                        value.UTF8String);
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

  GHOST_SystemIOS *system = _ghost_window->getSystem();
  if (system == nullptr) {
    return;
  }

  const uint64_t time = system->getMilliSeconds();
  system->pushEvent(std::make_unique<GHOST_EventKey>(time, GHOST_kEventKeyDown, _ghost_window, key, false, nullptr));
  system->pushEvent(std::make_unique<GHOST_EventKey>(time, GHOST_kEventKeyUp, _ghost_window, key, false, nullptr));
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

GHOST_WindowIOS::GHOST_WindowIOS(GHOST_SystemIOS *system_ios,
                                 const char *title,
                                 int32_t left,
                                 int32_t bottom,
                                 uint32_t width,
                                 uint32_t height,
                                 GHOST_TWindowState state,
                                 GHOST_TDrawingContextType type,
                                 const GHOST_ContextParams &context_params,
                                 bool is_dialog,
                                 GHOST_WindowIOS *parent_window)
    : GHOST_Window(width, height, state, context_params, false), metal_view_(nil)
{
  full_screen_ = true;
  system_ios_ = system_ios;
  /* Parent window will be the window that focus is returned to upon close. */
  parent_window_ = parent_window;
  window_title_ = nullptr;
  is_dialog_ = is_dialog;
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();

  GHOST_ios_main_sync(^{
    UIWindowScene *window_scene = GHOST_getIOSWindowScene();
    CGRect window_frame = full_screen_ && window_scene ?
                              window_scene.effectiveGeometry.coordinateSpace.bounds :
                              CGRectMake(left, bottom, width, height);

    /* Create MTKView. */
    metal_view_ = [[MTKView alloc] initWithFrame:window_frame];
    metal_view_.device = device;
    metal_view_.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                                   UIViewAutoresizingFlexibleHeight;
    GHOST_ASSERT(metal_view_, "metalview not valid");

    /* Create view controller. */
    UIApplication *app = [UIApplication sharedApplication];
    GHOST_ASSERT(app, "App not valid");
    id<UIApplicationDelegate> app_delegate = [app delegate];
    GHOST_ASSERT(app_delegate, "App not valid");

    GHOSTUIWindow *ghost_rootWindow = nullptr;

    if (full_screen_) {
      /* Init window at native res. */
      ghost_rootWindow = window_scene ? [[GHOSTUIWindow alloc] initWithWindowScene:window_scene] :
                                        [[GHOSTUIWindow alloc] initWithFrame:window_frame];
      /* Ensure fullscreen. */
      ghost_rootWindow.frame = window_frame;
    }
    else {
      /* Init window at specified size. */
      ghost_rootWindow = [[GHOSTUIWindow alloc] initWithWindowScene:window_scene];
      ghost_rootWindow.frame = CGRectMake(left, bottom, width, height);
      [ghost_rootWindow setClipsToBounds:YES];
    }

    rootWindow = (UIWindow *)ghost_rootWindow;
    GHOST_ios_register_window(rootWindow);

    [ghost_rootWindow setSystemAndWindowIOS:system_ios_ windowIOS:this];
    rootWindow.windowLevel = UIWindowLevelAlert;

    GHOST_ASSERT(rootWindow, "UIWindow not valid");
    uiview_controller_ = [[GHOST_IOSViewController alloc] initWithMetalKitView:metal_view_
                                                                        window:this];
    GHOST_ASSERT(uiview_controller_, "UIViewController not valid");

    /* Set presentation style depending on whether main window, dialog or temporary window. */
    if (full_screen_) {
      /* Initial window has no parent and is always fullscreen. */
      uiview_controller_.modalPresentationStyle = UIModalPresentationFullScreen;
    }
    else {
      /* Initial window has no parent and is always fullscreen. */
      uiview_controller_.modalPresentationStyle = UIModalPresentationPageSheet;
    }
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
      GHOSTIOSNavigationController *navigation = [[GHOSTIOSNavigationController
          alloc] initWithNavigationBarClass:GHOSTIOSNavigationBar.class toolbarClass:nil];
      [navigation setViewControllers:@[ uiview_controller_ ] animated:NO];
      rootWindow.rootViewController = navigation;
      [navigation release];
    }
    else {
      rootWindow.rootViewController = uiview_controller_;
    }
    [uiview_controller_ loadViewIfNeeded];

    /* Create UIView */
    GHOST_ASSERT(width > 0 && height > 0, "invalid wh");
    uiview_ = uiview_controller_.view;
    GHOST_ASSERT(uiview_, "uiview not valid");

    /* Enable HDR/EDR Support. */
    CAMetalLayer *metalLayer = (CAMetalLayer *)metal_view_.layer;
    metalLayer.wantsExtendedDynamicRangeContent = YES;
    metalLayer.pixelFormat = MTLPixelFormatRGBA16Float;
    CGColorSpaceRef colorspace = CGColorSpaceCreateWithName(kCGColorSpaceExtendedSRGB);
    metalLayer.colorspace = colorspace;
    CGColorSpaceRelease(colorspace);
    metalLayer.allowsNextDrawableTimeout = YES;
    surface = std::make_shared<GHOST_IOSSurface>(metalLayer);
    publishGeometry(metal_view_.drawableSize);

    [ghost_rootWindow registerGestureRecognizers];
  });
  [device release];

  setDrawingContextType(type);
  updateDrawingContext();
  activateDrawingContext();

  setTitle(title);

  deferred_swap_buffers_count = 0;

  /* Deactive the parent (if it exists) and activate this one. */
  if (parent_window_) {
    parent_window_->requestToDeactivateWindow();
  }

  /* Engine-owned activation, with native visibility applied on UIKit. */
  if (!system_ios_->current_active_window_) {
    request_to_make_active_ = true;
    makeKeyWindow();
  }
  /* Activate this window at the end of the next draw loop. */
  else {
    requestToActivateWindow();
  }
}

GHOST_WindowIOS::~GHOST_WindowIOS()
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  detachNativeInput();
  if (surface) {
    surface->close();
  }

  releaseNativeHandles();

  /* Restore application control and display to parent window. */
  if (parent_window_) {
    parent_window_->requestToActivateWindow();
    parent_window_ = nil;
  }
  /* We have no choice but to resign, however this seems like it might cause issues. */
  if (system_ios_->current_active_window_ == this) {
    IOS_WINDOW_LOG(@"~GHOST_WindowIOS(): Warning, deactivating the active window %p?", this);
    requestToDeactivateWindow();
    resignKeyWindow();
  }

  GHOST_ios_main_sync(^{
    if (metal_view_) {
      metal_view_.delegate = nil;
      [metal_view_ release];
      metal_view_ = nil;
    }
    if (uiview_) {
      uiview_ = nil;
    }

    /* Release window. */
    if (rootWindow) {
      GHOST_ios_unregister_window(rootWindow);
      rootWindow.hidden = YES;
      rootWindow.rootViewController = nil;
      [rootWindow release];
      rootWindow = nil;
    }
    if (uiview_controller_) {
      [uiview_controller_ release];
      uiview_controller_ = nil;
    }
  });

  if (window_title_) {
    free(window_title_);
    window_title_ = nullptr;
  }

  [pool drain];
}

void GHOST_WindowIOS::detachNativeInput()
{
  GHOST_ios_main_sync(^{
    if (input_alive->exchange(false)) {
      [(GHOSTUIWindow *)rootWindow detachBlender];
      [(GHOST_IOSViewController *)uiview_controller_ detachBlender];
    }
  });
}

void GHOST_WindowIOS::publishGeometry(CGSize size)
{
  GHOST_ASSERT(NSThread.isMainThread, "Geometry is published by UIKit");
  scale_.store(MAX(1.0, metal_view_.contentScaleFactor));
  const uint64_t packed = (uint64_t(MAX(1.0, size.width)) << 32) | uint32_t(MAX(1.0, size.height));
  if (surface) {
    surface->resize(size);
  }
  if (drawable_size_.exchange(packed) != packed) {
    system_ios_->pushEvent(
        std::make_unique<GHOST_Event>(system_ios_->getMilliSeconds(), GHOST_kEventWindowSize, this));
  }
}

#pragma mark accessors

bool GHOST_WindowIOS::getValid() const
{
  MTKView *view = metal_view_;
  return GHOST_Window::getValid() && uiview_ != NULL && view != NULL;
}

void *GHOST_WindowIOS::getOSWindow() const
{
  return (void *)uiview_;
}

GHOST_TSuccess GHOST_WindowIOS::swapBufferRelease()
{
  deferred_swap_buffers_count = 1;
  return GHOST_kSuccess;
}

void GHOST_WindowIOS::flushDeferredSwapBuffers()
{
  if (deferred_swap_buffers_count) {

    /* These two messages should be made asserts when we've fixed all the issues. */
    if (!getValid()) {
      IOS_WINDOW_LOG(@"Ignoring swap (invalid) con(%p) (win=%p)", getContext(), this);
      return;
    }

    if (!is_active_window_) {
      IOS_WINDOW_LOG(@"Ignoring swap (not active window) con(%p) (win=%p)", getContext(), this);
      return;
    }

    IOS_WINDOW_LOG(@"Swapping (ui_View)%p (mtkView)%p con(%p) (win=%p) (sc=%d)",
                   uiview_,
                   metal_view_,
                   getContext(),
                   this,
                   deferred_swap_buffers_count);

    GHOST_ContextIOS *context = reinterpret_cast<GHOST_ContextIOS *>(getContext());
    if (context->swapBufferRelease() == GHOST_kSuccess) {
      deferred_swap_buffers_count = 0;
    }
  }
}

void GHOST_WindowIOS::beginFrame() {}

void GHOST_WindowIOS::endFrame() {}

void GHOST_WindowIOS::setTitle(const char *title)
{
  if (window_title_) {
    free(window_title_);
    window_title_ = nullptr;
  }
  window_title_ = (char *)malloc(strlen(title) + 1);
  if (!window_title_) {
    GHOST_ASSERT(getValid(), "GHOST_WindowIOS::setTitle(): Failed to alloc mem for window title");
  }
  strcpy(window_title_, title);
  NSString *window_title = [NSString stringWithCString:title encoding:NSUTF8StringEncoding];
  UIViewController *controller = uiview_controller_;
  dispatch_async(dispatch_get_main_queue(), ^{
    controller.title = window_title;
  });
}

std::string GHOST_WindowIOS::getTitle() const
{
  return window_title_;
}

void GHOST_WindowIOS::needsDisplayUpdate()
{
  GHOST_ios_engine_wake();
}

void GHOST_WindowIOS::getWindowBounds(GHOST_Rect &bounds) const
{
  GHOST_ASSERT(getValid(), "GHOST_WindowIOS::getWindowBounds(): window invalid");

  getClientBounds(bounds);
}

void GHOST_WindowIOS::getClientBounds(GHOST_Rect &bounds) const
{
  GHOST_ASSERT(getValid(), "GHOST_WindowIOS::getWindowBounds(): window invalid");

  const uint64_t size = drawable_size_.load();

  bounds.l_ = 0;
  bounds.t_ = 0;
  bounds.r_ = size >> 32;
  bounds.b_ = uint32_t(size);
}

GHOST_TSuccess GHOST_WindowIOS::setClientWidth(uint32_t /*width*/)
{
  /* Ignore on iOS fow now. */
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowIOS::setClientHeight(uint32_t /*height*/)
{
  /* Ignore on iOS fow now. */
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowIOS::setClientSize(uint32_t /*width*/, uint32_t /*height*/)
{
  /* Ignore on iOS fow now. */
  return GHOST_kSuccess;
}

GHOST_TWindowState GHOST_WindowIOS::getState() const
{
  /* TODO: Implement. */
  return GHOST_kWindowStateNormal;
}

void GHOST_WindowIOS::screenToClient(int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const
{
  /* Pass through for fullscreen windows.
   * TODO: Support coordinate mapping for sized windows. */
  outX = inX;
  outY = inY;
}

void GHOST_WindowIOS::clientToScreen(int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const
{
  /* Pass through for fullscreen windows.
   * TODO: Support coordinate mapping for sized windows. */
  outX = inX;
  outY = inY;
}

void GHOST_WindowIOS::screenToClientIntern(int32_t inX,
                                           int32_t inY,
                                           int32_t &outX,
                                           int32_t &outY) const
{
  /* Pass through for fullscreen windows.
   * TODO: Support coordinate mapping for sized windows. */
  outX = inX;
  outY = inY;
}

void GHOST_WindowIOS::clientToScreenIntern(int32_t inX,
                                           int32_t inY,
                                           int32_t &outX,
                                           int32_t &outY) const
{
  /* Pass through for fullscreen windows.
   * TODO: Support coordinate mapping for sized windows. */
  outX = inX;
  outY = inY;
}

/* called for event, when window leaves monitor to another */
void GHOST_WindowIOS::setNativePixelSize(void) {}

/**
 * \note Fullscreen switch is not actual fullscreen with display capture.
 * As this capture removes all OS X window manager features.
 *
 * Instead, the menu bar and the dock are hidden, and the window is made border-less and
 * enlarged. Thus, process switch, exposé, spaces, ... still work in fullscreen mode
 */
GHOST_TSuccess GHOST_WindowIOS::setState(GHOST_TWindowState /*state*/)
{
  // Ignore on iOS?
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowIOS::setModifiedState(bool isUnsavedChanges)
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  [pool drain];
  return GHOST_Window::setModifiedState(isUnsavedChanges);
}

GHOST_TSuccess GHOST_WindowIOS::setOrder(GHOST_TWindowOrder /*order*/)
{
  /* TODO: Support or deprecate for iOS */
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  GHOST_ASSERT(getValid(), "GHOST_WindowIOS::setOrder(): window invalid");

  [pool drain];
  return GHOST_kSuccess;
}

#pragma mark Drawing context

GHOST_Context *GHOST_WindowIOS::newDrawingContext(GHOST_TDrawingContextType type)
{

  if (type == GHOST_kDrawingContextTypeMetal) {

    GHOST_Context *context = new GHOST_ContextIOS(want_context_params_, surface);

    if (context->initializeDrawingContext())
      return context;
    else
      delete context;
  }

  return NULL;
}

#pragma mark invalidate

GHOST_TSuccess GHOST_WindowIOS::invalidate()
{
  GHOST_ASSERT(getValid(), "GHOST_WindowIOS::invalidate(): window invalid");
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  [pool drain];
  return GHOST_kSuccess;
}

#pragma mark Progress bar

GHOST_TSuccess GHOST_WindowIOS::setProgressBar(float /*progress*/)
{
  return GHOST_kSuccess;
}

static void postNotification() {}

GHOST_TSuccess GHOST_WindowIOS::endProgressBar()
{
  return GHOST_kSuccess;
}

#pragma mark Cursor handling

void GHOST_WindowIOS::loadCursor(bool /*visible*/, GHOST_TStandardCursor /*shape*/) const {}

bool GHOST_WindowIOS::isDialog() const
{
  return is_dialog_;
}

GHOST_TSuccess GHOST_WindowIOS::setWindowCursorVisibility(bool /*visible*/)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowIOS::setWindowCursorGrab(GHOST_TGrabCursorMode /*mode*/)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowIOS::setWindowCursorShape(GHOST_TStandardCursor /*shape*/)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowIOS::hasCursorShape(GHOST_TStandardCursor /*shape*/)
{
  return GHOST_kSuccess;
}

/** Reverse the bits in a uint16_t */
static uint16_t uns16ReverseBits(uint16_t shrt)
{
  shrt = ((shrt >> 1) & 0x5555) | ((shrt << 1) & 0xAAAA);
  shrt = ((shrt >> 2) & 0x3333) | ((shrt << 2) & 0xCCCC);
  shrt = ((shrt >> 4) & 0x0F0F) | ((shrt << 4) & 0xF0F0);
  shrt = ((shrt >> 8) & 0x00FF) | ((shrt << 8) & 0xFF00);
  return shrt;
}

GHOST_TSuccess GHOST_WindowIOS::setWindowCustomCursorShape(const uint8_t * /*bitmap*/,
                                                           const uint8_t * /*mask*/,
                                                           const int /*size*/[2],
                                                           const int /*hot_spot*/[2],
                                                           bool /*canInvertColor*/)
{
  /* Passthrough for iOS. */
  return GHOST_kSuccess;
}

uint16_t GHOST_WindowIOS::getDPIHint()
{
  return 288;
}

GHOST_TSuccess GHOST_WindowIOS::popupOnscreenKeyboard(
    const GHOST_KeyboardProperties &keyboard_properties)
{
  GHOSTUIWindow *ghost_rootWindow = (GHOSTUIWindow *)rootWindow;
  __block GHOST_TSuccess result;
  GHOST_ios_main_sync(^{
    result = [ghost_rootWindow popupOnscreenKeyboard:keyboard_properties];
  });
  return result;
}

GHOST_TSuccess GHOST_WindowIOS::hideOnscreenKeyboard()
{
  GHOSTUIWindow *ghost_rootWindow = (GHOSTUIWindow *)rootWindow;
  __block GHOST_TSuccess result;
  GHOST_ios_main_sync(^{
    result = [ghost_rootWindow hideOnscreenKeyboard];
  });
  return result;
}

const char *GHOST_WindowIOS::getLastKeyboardString()
{
  GHOSTUIWindow *ghost_rootWindow = (GHOSTUIWindow *)rootWindow;
  GHOST_ios_main_sync(^{
    const char *text = [ghost_rootWindow getLastKeyboardString];
    keyboard_string_ = text ? text : "";
  });
  return keyboard_string_.c_str();
}

const GHOST_TabletData GHOST_WindowIOS::getTabletData()
{
  GHOSTUIWindow *ghost_rootWindow = (GHOSTUIWindow *)rootWindow;
  __block GHOST_TabletData result;
  GHOST_ios_main_sync(^{
    result = [ghost_rootWindow getTabletData];
  });
  return result;
}

/* This is the size of the window pre-scaled */
CGSize GHOST_WindowIOS::getLogicalWindowSize()
{
  CGSize size = getNativeWindowSize();
  float scale = scale_.load();
  return CGSizeMake(size.width / scale, size.height / scale);
}

/* This is the size of the window post-scaled */
CGSize GHOST_WindowIOS::getNativeWindowSize()
{
  const uint64_t size = drawable_size_.load();
  return CGSizeMake(size >> 32, uint32_t(size));
}

float GHOST_WindowIOS::getWindowScaleFactor()
{
  return scale_.load();
}

/* Indicate that we want this window to be the next active one. */
void GHOST_WindowIOS::requestToActivateWindow()
{
  /* Check we're not already active. */
  if (system_ios_->current_active_window_ != this) {
    /* Replace any outstanding requests. */
    if (system_ios_->next_active_window_) {
      system_ios_->next_active_window_->requestToDeactivateWindow();
    }
    request_to_make_active_ = true;
    system_ios_->next_active_window_ = this;
  }
}

void GHOST_WindowIOS::requestToDeactivateWindow()
{
  if (system_ios_->next_active_window_ == this) {
    IOS_WINDOW_LOG(@"requestToDeactivateWindow(): Has something gone wrong? %p", this);
    system_ios_->next_active_window_ = nullptr;
  }
  request_to_make_active_ = false;
}

bool GHOST_WindowIOS::makeKeyWindow()
{
  if (!getValid()) {
    IOS_WINDOW_LOG(@"Failed to activate (invalid) con(%p) (win=%p)", getContext(), this);
    return false;
  }

  GHOST_ContextIOS *context = reinterpret_cast<GHOST_ContextIOS *>(getContext());
  GHOST_ASSERT(rootWindow != nil, "GHOST_WindowIOS::makeKeyWindow() root window required");
  GHOST_ASSERT(context != nullptr, "GHOST_WindowIOS::makeKeyWindow() context required");
  GHOST_ASSERT(request_to_make_active_,
               "GHOST_WindowIOS::makeKeyWindow() must request activation first");

  /* Make window primary visible window. */
  GHOST_ios_main_sync(^{
    [rootWindow makeKeyAndVisible];
    id<UIWindowSceneDelegate> delegate = (id<UIWindowSceneDelegate>)
                                             rootWindow.windowScene.delegate;
    delegate.window = rootWindow;
    surface->setVisible(rootWindow.windowScene.activationState ==
                        UISceneActivationStateForegroundActive);
  });

  IOS_WINDOW_LOG(@"Key Window: (ui_View)%p (mtkView)%p con(%p) (win=%p)",
                 uiview_,
                 metal_view_,
                 getContext(),
                 this);

  system_ios_->current_active_window_ = this;
  is_active_window_ = true;
  request_to_make_active_ = false;
  return true;
}

void GHOST_WindowIOS::resignKeyWindow()
{
  GHOST_ASSERT(system_ios_->current_active_window_ == this,
               "GHOST_WindowIOS::resignKeyWindow(): Can only resign current active window");
  GHOST_ASSERT(is_active_window_,
               "GHOST_WindowIOS::resignKeyWindow(): Can't resign non active window");
  GHOST_ASSERT(!request_to_make_active_,
               "GHOST_WindowIOS::resignKeyWindow(): activation request outstanding");

  surface->setVisible(false);
  IOS_WINDOW_LOG(@"Resigning Key Window: (ui_View)%p (mtkView)%p con(%p) (win=%p)",
                 uiview_,
                 metal_view_,
                 getContext(),
                 this);
  is_active_window_ = false;
  system_ios_->current_active_window_ = nullptr;
}

CGPoint GHOST_WindowIOS::scalePointToWindow(CGPoint &point)
{
  CGPoint scaled_point;
  scaled_point.x = point.x * getWindowScaleFactor();
  scaled_point.y = point.y * getWindowScaleFactor();
  return scaled_point;
}

#ifdef WITH_INPUT_IME
void GHOST_WindowIOS::beginIME(
    int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/, bool /*completed*/)
{
  /* Passthrough for iOS. */
}

void GHOST_WindowIOS::endIME()
{
  /* Passthrough for iOS. */
}
#endif /* WITH_INPUT_IME */
