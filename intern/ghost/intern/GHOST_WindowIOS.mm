/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GHOST_IOSNativeInterface.hh"
#import "GHOST_IOSWindowInput.hh"

/* Input the native interface responds to, see #GHOST_IOSNativeInterface. */
static void ios_pencil_hover(bool active, int x, int y)
{
  if (const GHOST_IOSNativeInterface *native = GHOST_ios_native_interface()) {
    native->pencil_hover(active, x, y);
  }
}

static void ios_navigation_hover(bool active)
{
  if (const GHOST_IOSNativeInterface *native = GHOST_ios_native_interface()) {
    native->navigation_hover(active);
  }
}

static void ios_view_roll(float angle)
{
  if (const GHOST_IOSNativeInterface *native = GHOST_ios_native_interface()) {
    native->view_roll(angle);
  }
}

/* Hardware keys held for Blender, by HID usage: their release goes to Blender too. */
static NSMutableDictionary<NSNumber *, NSNumber *> *held_keys = nil;
/* The key that repeats while held, as on the desktop, and its text. */
static CFTimeInterval key_repeat_next = 0;
static GHOST_TKey key_repeat_key = GHOST_kKeyUnknown;
static char key_repeat_utf8[6] = {0};
static CADisplayLink *key_repeat_link = nil;
/* The first responder, found by #ghost_captureFirstResponder:. */
static UIResponder *found_first_responder = nil;

@implementation UIResponder (GHOSTFirstResponder)
- (void)ghost_captureFirstResponder:(id)sender
{
  UNUSED_VARS(sender);
  found_first_responder = self;
}
@end

static UIResponder *current_first_responder()
{
  found_first_responder = nil;
  [UIApplication.sharedApplication sendAction:@selector(ghost_captureFirstResponder:)
                                           to:nil
                                         from:nil
                                     forEvent:nil];
  return found_first_responder;
}

static bool modifier_from_key(GHOST_TKey key, GHOST_TModifierKey *modifier)
{
  switch (key) {
    case GHOST_kKeyLeftShift:
      *modifier = GHOST_kModifierKeyLeftShift;
      return true;
    case GHOST_kKeyRightShift:
      *modifier = GHOST_kModifierKeyRightShift;
      return true;
    case GHOST_kKeyLeftControl:
      *modifier = GHOST_kModifierKeyLeftControl;
      return true;
    case GHOST_kKeyRightControl:
      *modifier = GHOST_kModifierKeyRightControl;
      return true;
    case GHOST_kKeyLeftAlt:
      *modifier = GHOST_kModifierKeyLeftAlt;
      return true;
    case GHOST_kKeyRightAlt:
      *modifier = GHOST_kModifierKeyRightAlt;
      return true;
    case GHOST_kKeyLeftOS:
      *modifier = GHOST_kModifierKeyLeftOS;
      return true;
    case GHOST_kKeyRightOS:
      *modifier = GHOST_kModifierKeyRightOS;
      return true;
    default:
      return false;
  }
}

/** Blender's main window, which shows the open document over the launcher. */
UIWindow *document_window = nil;

@implementation GHOSTUITapGestureRecognizer

- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window
{
  CGPoint touch_point = [self locationInView:window->getView()];
  return window->scalePointToWindow(touch_point);
}

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

@implementation GHOSTUIHoverGestureRecognizer

- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window
{
  CGPoint touch_point = [self locationInView:window->getView()];
  return window->scalePointToWindow(touch_point);
}
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

@implementation GHOSTUIWindow
- (void)detachBlender
{
  [self releaseKeys];
#if !defined(APPLE_TARGET_VISIONOS)
  [self releasePointerButtons];
#endif
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

- (void)setPencilContextMenu:(UIView<GHOSTIOSPencilMenu> *)menu
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

#if !defined(APPLE_TARGET_VISIONOS)
  /* Edge swipe: visionOS windows have no screen edges. */
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
#endif

  /* Apple Pencil hover recognizer. */
  hover_gesture_recognizer = [[GHOSTUIHoverGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handleHover:)];
  hover_gesture_recognizer.delegate = self;
  hover_gesture_recognizer.allowedTouchTypes = @[ @(UITouchTypePencil) ];
  /* Observe both the viewport and the modal native radial overlay. */
  [self addGestureRecognizer:hover_gesture_recognizer];
  current_pencil_touch = nil;

#if !defined(APPLE_TARGET_VISIONOS)
  /* A mouse or trackpad, as on the desktop: the pointer moves Blender's cursor. */
  pointer_hover_recognizer = [[UIHoverGestureRecognizer alloc]
      initWithTarget:self
              action:@selector(handlePointerHover:)];
  pointer_hover_recognizer.delegate = self;
  pointer_hover_recognizer.allowedTouchTypes = @[ @(UITouchTypeIndirectPointer) ];
  [self addGestureRecognizer:pointer_hover_recognizer];

  /**  Apple Pencil double-tap. */
  pencil_interaction = [[UIPencilInteraction alloc] init];
  pencil_interaction.delegate = self;
  [window->getView() addInteraction:pencil_interaction];
#endif
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
          system->pushEvent(std::make_unique<GHOST_EventCursor>(system->getMilliSeconds(),
                                                                GHOST_kEventCursorMove,
                                                                window,
                                                                event_info.location.x,
                                                                event_info.location.y,
                                                                tablet_data));
          break;
        case UserInputEvent::EventTypes::ORBIT_GESTURE:
          system->pushEvent(std::make_unique<GHOST_EventTrackpad>(system->getMilliSeconds(),
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
          system->pushEvent(std::make_unique<GHOST_EventKey>(
              system->getMilliSeconds(), GHOST_kEventKeyDown, window, GHOST_kKeyEsc, false));
          system->pushEvent(std::make_unique<GHOST_EventKey>(
              system->getMilliSeconds(), GHOST_kEventKeyUp, window, GHOST_kKeyEsc, false));
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
      [otherGestureRecognizer isKindOfClass:UIHoverGestureRecognizer.class])
  {
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

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer
       shouldReceiveTouch:(UITouch *)touch
{
  if (finger_tool_enabled &&
      (gestureRecognizer == pan_gesture_recognizer || gestureRecognizer == tap_gesture_recognizer))
  {
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
      gestureRecognizer == rotation_gesture_recognizer ||
#if !defined(APPLE_TARGET_VISIONOS)
      gestureRecognizer == edge_swipe_left || gestureRecognizer == edge_swipe_right ||
#endif
      gestureRecognizer == hover_gesture_recognizer)
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
  ios_pencil_hover(false, 0, 0);
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
          touch.phase != UITouchPhaseCancelled && [self touchBeganInBlenderViewport:touch])
      {
        fingers++;
      }
    }
    /* Hand the sequence to two-finger navigation without committing a partial tool drag. */
    if (fingers > 1 && current_pencil_touch.type == UITouchTypeDirect) {
      [self cancelPencilStroke];
    }
    for (UITouch *touch in event.allTouches) {
      if (touch.phase == UITouchPhaseBegan) {
        ios_pencil_hover(false, 0, 0);
        [self stopOrbitMomentum];
      }
      if (touch.type == UITouchTypeDirect && pencil_context_menu.active) {
        if (touch.phase == UITouchPhaseBegan) {
          [pencil_context_menu cancel];
        }
        continue;
      }
#if !defined(APPLE_TARGET_VISIONOS)
      if (touch.type == UITouchTypeIndirectPointer) {
        [self handlePointerTouch:touch event:event];
        continue;
      }
#endif
      if (touch.type != UITouchTypePencil &&
          !(finger_tool_enabled && touch.type == UITouchTypeDirect && fingers <= 1))
      {
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
          ios_navigation_hover(false);
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
            if (hypot(point.x - pencil_navigation_start.x, point.y - pencil_navigation_start.y) >
                10.0)
            {
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

/* -------------------------------------------------------------------- */
/* Hardware keyboard
 *
 * Keys go to Blender's keymap as on the desktop, except while text is typed into a native field
 * or a native sheet or alert shows. Modifier keys and Escape reach UIKit too, so holding Command
 * shows the menu bar's shortcuts; other keys don't, so UIKit's own uses of Tab, Space or arrows
 * can't act on the interface around Blender. */

- (bool)blenderTakesKeyboard
{
  if (!window || !system || self.hidden || !GHOST_ios_document_window_visible() ||
      self.rootViewController.presentedViewController != nil)
  {
    return false;
  }
  UIResponder *responder = current_first_responder();
  return ![responder conformsToProtocol:@protocol(UIKeyInput)];
}

- (void)pushKey:(GHOST_TKey)key down:(bool)down repeat:(bool)repeat utf8:(const char *)utf8
{
  system->pushEvent(std::make_unique<GHOST_EventKey>(system->getMilliSeconds(),
                                                     down ? GHOST_kEventKeyDown :
                                                            GHOST_kEventKeyUp,
                                                     window,
                                                     key,
                                                     repeat,
                                                     down ? utf8 : nullptr));
}

- (BOOL)handleKeyboardEvent:(UIPressesEvent *)event
{
  if (!window || !system) {
    return NO;
  }
  if (!held_keys) {
    held_keys = [[NSMutableDictionary alloc] init];
  }
  bool takes = false;
  bool hide_from_uikit = false;
  for (UIPress *press in event.allPresses) {
    UIKey *ui_key = press.key;
    if (!ui_key) {
      continue;
    }
    NSNumber *usage = @(ui_key.keyCode);
    const bool down = press.phase == UIPressPhaseBegan;
    const bool up = press.phase == UIPressPhaseEnded || press.phase == UIPressPhaseCancelled;
    if (!down && !up) {
      continue;
    }
    GHOST_TKey key;
    if (up) {
      /* A release goes where its press went. */
      NSNumber *held = held_keys[usage];
      if (!held) {
        continue;
      }
      key = GHOST_TKey(held.intValue);
      [held_keys removeObjectForKey:usage];
    }
    else {
      if (!takes && ![self blenderTakesKeyboard]) {
        continue;
      }
      NSString *base = ui_key.charactersIgnoringModifiers;
      key = convertKey(int(ui_key.keyCode), base.length ? [base characterAtIndex:0] : 0);
      if (key == GHOST_kKeyUnknown) {
        continue;
      }
      held_keys[usage] = @(int(key));
    }
    takes = true;

    /* Keys go to the editor under the pointer, as on the desktop. Without a Pencil, mouse or
     * trackpad there has been none yet: use the middle of the view, the viewport. */
    int32_t pointer_x, pointer_y;
    if (down && window->getPointerPosition(pointer_x, pointer_y) != GHOST_kSuccess) {
      const CGSize size = window->getView().bounds.size;
      CGPoint middle = CGPointMake(size.width / 2, size.height / 2);
      middle = window->scalePointToWindow(middle);
      UserInputEvent move(&middle, nullptr, nullptr, false);
      move.add_event(UserInputEvent::EventTypes::CURSOR_MOVE);
      [self generateUserInputEvents:move];
    }

    GHOST_TModifierKey modifier;
    const bool is_modifier = modifier_from_key(key, &modifier);
    if (is_modifier) {
      GHOST_ios_set_modifier_held(modifier, down);
    }
    else if (key != GHOST_kKeyEsc) {
      /* Escape also closes native panels, such as the Outliner, as their cancel action. */
      hide_from_uikit = true;
    }

    /* Text as on macOS: none for commands, arrows and function keys. */
    char utf8[6] = {0};
    NSString *text = ui_key.characters;
    if (down && !is_modifier && text.length &&
        !(ui_key.modifierFlags & (UIKeyModifierCommand | UIKeyModifierControl)) &&
        !(key >= GHOST_kKeyLeftArrow && key <= GHOST_kKeyDownArrow) &&
        !(key >= GHOST_kKeyF1 && key <= GHOST_kKeyF24))
    {
      const char *bytes = text.UTF8String;
      if (bytes && strlen(bytes) < sizeof(utf8) && uint8_t(bytes[0]) >= 0x20 && bytes[0] != 0x7f)
      {
        strcpy(utf8, bytes);
      }
    }
    [self pushKey:key down:down repeat:false utf8:utf8];

    if (down && !is_modifier) {
      [self startKeyRepeat:key utf8:utf8];
    }
    else if (up && key == key_repeat_key) {
      [self stopKeyRepeat];
    }
  }
  return hide_from_uikit;
}

/** UIKit doesn't repeat hardware keys, Blender expects them to repeat as the desktop does. */
- (void)startKeyRepeat:(GHOST_TKey)key utf8:(const char *)utf8
{
  key_repeat_key = key;
  memcpy(key_repeat_utf8, utf8, sizeof(key_repeat_utf8));
  key_repeat_next = CACurrentMediaTime() + 0.45;
  if (!key_repeat_link) {
    key_repeat_link = [[CADisplayLink displayLinkWithTarget:self
                                                   selector:@selector(repeatKey:)] retain];
    key_repeat_link.preferredFrameRateRange = CAFrameRateRangeMake(30, 60, 30);
    [key_repeat_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  }
}

- (void)repeatKey:(CADisplayLink *)link
{
  if (!window || !system) {
    [self stopKeyRepeat];
    return;
  }
  if (link.timestamp < key_repeat_next) {
    return;
  }
  key_repeat_next = link.timestamp + 1.0 / 30.0;
  [self pushKey:key_repeat_key down:true repeat:true utf8:key_repeat_utf8];
}

- (void)stopKeyRepeat
{
  [key_repeat_link invalidate];
  [key_repeat_link release];
  key_repeat_link = nil;
  key_repeat_key = GHOST_kKeyUnknown;
}

/** Release every held key, such as when the app leaves the foreground with keys down. */
- (void)releaseKeys
{
  [self stopKeyRepeat];
  for (NSNumber *held in held_keys.allValues) {
    const GHOST_TKey key = GHOST_TKey(held.intValue);
    GHOST_TModifierKey modifier;
    if (modifier_from_key(key, &modifier)) {
      GHOST_ios_set_modifier_held(modifier, false);
    }
    if (window && system) {
      [self pushKey:key down:false repeat:false utf8:nullptr];
    }
  }
  [held_keys removeAllObjects];
}

#if !defined(APPLE_TARGET_VISIONOS)
/* -------------------------------------------------------------------- */
/* Mouse and trackpad
 *
 * As on the desktop: the pointer moves Blender's cursor and its buttons are mouse buttons. */

- (bool)pointerOverViewport:(CGPoint)location_in_window
{
  return [self isBlenderPencilInputView:[self hitTest:location_in_window withEvent:nil]];
}

- (void)sendPointerMove:(CGPoint)location
{
  tablet_data = GHOST_TABLET_DATA_NONE;
  CGPoint point = window->scalePointToWindow(location);
  UserInputEvent event_info(&point, nullptr, nullptr, false);
  event_info.add_event(UserInputEvent::EventTypes::CURSOR_MOVE);
  [self generateUserInputEvents:event_info];
}

- (void)handlePointerHover:(UIHoverGestureRecognizer *)sender
{
  if (!window || pointer_touch) {
    return;
  }
  const bool hovering = (sender.state == UIGestureRecognizerStateBegan ||
                         sender.state == UIGestureRecognizerStateChanged) &&
                        [self pointerOverViewport:[sender locationInView:self]];
  if (!hovering) {
    /* Paint cursors and hover highlights show while something hovers over the view. */
    ios_pencil_hover(false, 0, 0);
    return;
  }
  const CGPoint location = [sender locationInView:window->getView()];
  [self sendPointerMove:location];
  CGPoint point = location;
  point = window->scalePointToWindow(point);
  ios_pencil_hover(true, point.x, point.y);
}

static GHOST_TButton pointer_button(int index)
{
  switch (index) {
    case 0:
      return GHOST_kButtonMaskLeft;
    case 1:
      return GHOST_kButtonMaskRight;
    case 2:
      return GHOST_kButtonMaskMiddle;
    case 3:
      return GHOST_kButtonMaskButton4;
    default:
      return GHOST_kButtonMaskButton5;
  }
}

- (void)sendPointerButtons:(UIEventButtonMask)buttons
{
  for (int index = 0; index < 5; index++) {
    const UIEventButtonMask mask = UIEventButtonMaskForButtonNumber(index + 1);
    if ((buttons & mask) == (pointer_buttons & mask)) {
      continue;
    }
    system->pushEvent(std::make_unique<GHOST_EventButton>(system->getMilliSeconds(),
                                                          (buttons & mask) ?
                                                              GHOST_kEventButtonDown :
                                                              GHOST_kEventButtonUp,
                                                          window,
                                                          pointer_button(index),
                                                          GHOST_TABLET_DATA_NONE));
  }
  pointer_buttons = buttons;
}

/** A click or drag of the pointer, which began over the view. */
- (void)handlePointerTouch:(UITouch *)touch event:(UIEvent *)event
{
  if (!window || !system) {
    return;
  }
  if (touch.phase == UITouchPhaseBegan) {
    if (pointer_touch || ![self touchBeganInBlenderViewport:touch]) {
      return;
    }
    pointer_touch = touch;
    pointer_buttons = 0;
    ios_pencil_hover(false, 0, 0);
  }
  if (touch != pointer_touch) {
    return;
  }
  const bool released = touch.phase == UITouchPhaseEnded ||
                        touch.phase == UITouchPhaseCancelled;
  if (touch.phase == UITouchPhaseMoved) {
    NSArray<UITouch *> *samples = [event coalescedTouchesForTouch:touch];
    for (UITouch *sample in samples.count ? samples : @[ touch ]) {
      [self sendPointerMove:[sample preciseLocationInView:window->getView()]];
    }
  }
  else {
    [self sendPointerMove:[touch preciseLocationInView:window->getView()]];
  }
  /* A click without a mask, as from a tap on a trackpad, is the primary button. */
  UIEventButtonMask buttons = event.buttonMask ? event.buttonMask : UIEventButtonMaskPrimary;
  [self sendPointerButtons:released ? 0 : buttons];
  if (released) {
    pointer_touch = nil;
  }
}

/** Release the pointer's buttons, as when the app leaves the foreground during a drag. */
- (void)releasePointerButtons
{
  if (window && system) {
    [self sendPointerButtons:0];
  }
  pointer_buttons = 0;
  pointer_touch = nil;
}

#endif

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
      !finger_tool_enabled && sender.state == UIGestureRecognizerStateEnded)
  {
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
  [self releaseKeys];
#if !defined(APPLE_TARGET_VISIONOS)
  [self releasePointerButtons];
#endif
  [self stopOrbitMomentum];
  [self cancelPencilStroke];
  [pencil_context_menu cancel];
  ios_navigation_hover(false);
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

#if !defined(APPLE_TARGET_VISIONOS)
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
#endif

- (void)handleHover:(GHOSTUIHoverGestureRecognizer *)sender
{
  if (!pencil_context_menu.active &&
      (sender.state == UIGestureRecognizerStateBegan ||
       sender.state == UIGestureRecognizerStateChanged) &&
      ![self isBlenderPencilInputView:[self hitTest:[sender locationInView:(UIView *)self]
                                          withEvent:nil]])
  {
    ios_navigation_hover(false);
    ios_pencil_hover(false, 0, 0);
    return;
  }
  if ([pencil_context_menu handlePencilHover:sender]) {
    ios_navigation_hover(false);
    ios_pencil_hover(false, 0, 0);
    return;
  }
  if (pencil_button_active) {
    ios_pencil_hover(false, 0, 0);
    return;
  }
  if (sender.state == UIGestureRecognizerStateBegan ||
      sender.state == UIGestureRecognizerStateChanged)
  {
    ios_navigation_hover(true);
    /* Tablet needs to be set to stylus mode because we need
     * wmTabletData.is_motion_absolute set to true. */
    tablet_data.Active = GHOST_kTabletModeStylus;
    tablet_data.Pressure = 0.0f;
    CGPoint hover_point = [sender getScaledTouchPoint:window];
    UIView *hit = [self hitTest:[sender locationInView:self] withEvent:nil];
    ios_pencil_hover(![hit.accessibilityIdentifier isEqualToString:@"blender.viewport.navigation"],
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
    ios_navigation_hover(false);
    tablet_data = GHOST_TABLET_DATA_NONE;
    ios_pencil_hover(false, 0, 0);
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
      ios_view_roll(float(relative_rotation));
    }
  }
  else if (sender.state == UIGestureRecognizerStateEnded ||
           sender.state == UIGestureRecognizerStateCancelled ||
           sender.state == UIGestureRecognizerStateFailed)
  {
    sender.rotation = 0.0;
  }
}

#if !defined(APPLE_TARGET_VISIONOS)
- (void)togglePencilMenuForInteraction:(UIPencilInteraction *)interaction
                             hoverPose:(UIPencilHoverPose *)pose
{
  NSValue *location = nil;
  if (pose) {
    CGPoint point = [interaction.view convertPoint:pose.location toView:self];
    if (!pencil_context_menu.active && ![self isBlenderPencilInputView:[self hitTest:point
                                                                           withEvent:nil]])
    {
      return;
    }
    location = [NSValue valueWithCGPoint:[self convertPoint:point toView:pencil_context_menu]];
  }
  else if (current_pencil_touch) {
    location = [NSValue
        valueWithCGPoint:[current_pencil_touch preciseLocationInView:pencil_context_menu]];
  }
  ios_navigation_hover(false);
  [self stopOrbitMomentum];
  [self cancelPencilStroke];
  [pencil_context_menu toggleWithPencilLocation:location];
}

- (void)pencilInteraction:(UIPencilInteraction *)interaction
            didReceiveTap:(UIPencilInteractionTap *)tap
{
  const UIPencilPreferredAction action = UIPencilInteraction.preferredTapAction;
  if (action != UIPencilPreferredActionIgnore &&
      action != UIPencilPreferredActionRunSystemShortcut)
  {
    [self togglePencilMenuForInteraction:interaction hoverPose:tap.hoverPose];
  }
}

- (void)pencilInteraction:(UIPencilInteraction *)interaction
        didReceiveSqueeze:(UIPencilInteractionSqueeze *)squeeze
{
  const UIPencilPreferredAction action = UIPencilInteraction.preferredSqueezeAction;
  if (squeeze.phase == UIPencilInteractionPhaseEnded && action != UIPencilPreferredActionIgnore &&
      action != UIPencilPreferredActionRunSystemShortcut)
  {
    [self togglePencilMenuForInteraction:interaction hoverPose:squeeze.hoverPose];
  }
}
#endif

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

#if !defined(APPLE_TARGET_VISIONOS)
    if (toolbar_enabled) {
      [self initToolbar];
      text_field.inputAccessoryView = toolbar;
    }
#endif

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
    /* With a hardware keyboard Blender edits the text itself from key events, as on the
     * desktop; #getLastKeyboardString then has nothing to replace it with. */
    hardware_text_edit = [GCKeyboard coalescedKeyboard] != nil;
    if (hardware_text_edit) {
      return GHOST_kSuccess;
    }
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
    if (hardware_text_edit) {
      return nullptr;
    }

    /* Update text string if one exists */
    if (text_field.text && ![text_field.text isEqualToString:@""]) {
      /* Save the input to a c-string */
      text_field_string = [[text_field text] UTF8String];
    }
  }
  return text_field_string;
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
    metal_view_.multipleTouchEnabled = YES;
    metal_view_.enableSetNeedsDisplay = NO;
    metal_view_.clearColor = MTLClearColorMake(0, 0, 0, 1.0);
    /* MTKView only manages native layout. It must never acquire a drawable on UIKit. */
    metal_view_.paused = YES;
    metal_view_.colorPixelFormat = MTLPixelFormatRGBA16Float;
    metal_view_.autoResizeDrawable = YES;
    metal_view_.contentMode = UIViewContentModeScaleToFill;
    /* Set the refresh rate to the screen's maximum. There may be some value in capping
     * this value to preserve battery life (60fps seems to work well). */
    metal_view_.preferredFramesPerSecond = GHOSTIOSMaximumFramesPerSecond();

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
    if (document_window == nil) {
      document_window = rootWindow;
    }

    [ghost_rootWindow setSystemAndWindowIOS:system_ios_ windowIOS:this];
    rootWindow.windowLevel = UIWindowLevelAlert;

    GHOST_ASSERT(rootWindow, "UIWindow not valid");
    /* The native interface's view controller around the Metal view, see
     * #GHOST_IOSNativeInterface::window_controller. */
    const GHOST_IOSNativeInterface *native = GHOST_ios_native_interface();
    GHOST_ASSERT(native && native->window_controller, "No native interface");
    void *root = nullptr;
    uiview_controller_ = (UIViewController *)native->window_controller(metal_view_, this, &root);
    GHOST_ASSERT(uiview_controller_, "UIViewController not valid");
    /* Set presentation style depending on whether main window, dialog or temporary window. */
    uiview_controller_.modalPresentationStyle = full_screen_ ? UIModalPresentationFullScreen :
                                                               UIModalPresentationPageSheet;
    rootWindow.rootViewController = (UIViewController *)root;
    [uiview_controller_ loadViewIfNeeded];

    /* The view that takes Blender's input: the Metal view, wherever the native interface places
     * it in its controller's views. */
    GHOST_ASSERT(width > 0 && height > 0, "invalid wh");
    uiview_ = metal_view_;
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
    metal_renderer_ = [[GHOST_IOSMetalRenderer alloc] initWithWindow:this];
    metal_view_.delegate = metal_renderer_;

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
    if (metal_renderer_) {
      [metal_renderer_ detach];
      [metal_renderer_ release];
      metal_renderer_ = nil;
    }
    if (uiview_) {
      uiview_ = nil;
    }

    /* Release window. */
    if (rootWindow) {
      if (document_window == rootWindow) {
        document_window = nil;
      }
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
      [(UIViewController<GHOST_IOSWindowController> *)uiview_controller_ detachBlender];
      [metal_renderer_ detach];
      metal_view_.delegate = nil;
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
    system_ios_->pushEvent(std::make_unique<GHOST_Event>(
        system_ios_->getMilliSeconds(), GHOST_kEventWindowSize, this));
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
#if defined(APPLE_TARGET_VISIONOS)
  /* The window's title bar names the document, as other visionOS apps do, without its path. */
  NSRange path = [window_title rangeOfString:@" ["];
  if (path.location != NSNotFound) {
    window_title = [window_title substringToIndex:path.location];
  }
  if ([window_title hasPrefix:@"* "]) {
    window_title = [window_title substringFromIndex:2];
  }
#endif
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
  /* Out of sight in the background: iOS allows no drawing there. */
  return GHOST_ios_app_in_background() ? GHOST_kWindowStateMinimized : GHOST_kWindowStateNormal;
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
  /* Unsaved changes are what makes the document save itself. */
  dispatch_async(dispatch_get_main_queue(), ^{
    if (const GHOST_IOSNativeInterface *native = GHOST_ios_native_interface()) {
      native->document_modified(isUnsavedChanges);
    }
  });
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
    /* Over the launcher only while it has a document open. */
    if (GHOST_ios_document_window_visible()) {
      [rootWindow makeKeyAndVisible];
    }
    else {
      rootWindow.hidden = YES;
    }
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

/* -------------------------------------------------------------------- */
/** \name The Window, for the Native Interface
 * \{ */

std::shared_ptr<std::atomic<bool>> GHOST_ios_window_alive(GHOST_WindowIOS *window)
{
  return window->input_alive;
}

bool GHOST_ios_window_presented_frame(GHOST_WindowIOS *window)
{
  return window->surface && window->surface->hasPresentedFrame();
}

void GHOST_ios_window_send_key(GHOST_WindowIOS *window, GHOST_TKey key)
{
  GHOST_SystemIOS *system = window->getSystem();
  if (system == nullptr) {
    return;
  }
  const uint64_t time = system->getMilliSeconds();
  system->pushEvent(
      std::make_unique<GHOST_EventKey>(time, GHOST_kEventKeyDown, window, key, false, nullptr));
  system->pushEvent(
      std::make_unique<GHOST_EventKey>(time, GHOST_kEventKeyUp, window, key, false, nullptr));
}

UIWindow<GHOST_IOSInputWindow> *GHOST_ios_window_native(GHOST_WindowIOS *window)
{
  return (GHOSTUIWindow *)window->rootWindow;
}

CGPoint GHOST_ios_window_pixels(GHOST_WindowIOS *window, CGPoint point)
{
  return window->scalePointToWindow(point);
}

void GHOST_ios_window_drop_files(GHOST_WindowIOS *window,
                                 NSArray<NSString *> *paths,
                                 CGPoint point)
{
  GHOST_SystemIOS *system = window->getSystem();
  if (system == nullptr) {
    return;
  }
  GHOST_TStringArray *files = static_cast<GHOST_TStringArray *>(
      malloc(sizeof(GHOST_TStringArray)));
  files->count = int(paths.count);
  files->strings = static_cast<uint8_t **>(malloc(paths.count * sizeof(uint8_t *)));
  for (NSUInteger i = 0; i < paths.count; i++) {
    files->strings[i] = reinterpret_cast<uint8_t *>(strdup(paths[i].fileSystemRepresentation));
  }
  const CGPoint client = window->scalePointToWindow(point);
  system->pushEvent(std::make_unique<GHOST_EventDragnDrop>(system->getMilliSeconds(),
                                                           GHOST_kEventDraggingDropDone,
                                                           GHOST_kDragnDropTypeFilenames,
                                                           window,
                                                           int(lround(client.x)),
                                                           int(lround(client.y)),
                                                           files));
}

/** \} */
