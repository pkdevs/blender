/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * The iOS window's input: touches, the Pencil and gestures, turned into Blender's events.
 */

#pragma once

#include "GHOST_ContextIOS.hh"
#include "GHOST_Debug.hh"
#include "GHOST_EventButton.hh"
#include "GHOST_EventCursor.hh"
#include "GHOST_EventDragnDrop.hh"
#include "GHOST_EventKey.hh"
#include "GHOST_EventTouch.hh"
#include "GHOST_EventTrackpad.hh"
#include "GHOST_EventWheel.hh"
#include "GHOST_IOSNativeInterface.hh"
#import "GHOST_IOSPencilMenu.hh"
#import "GHOST_IOSPlatform.hh"
#include "GHOST_IOSRuntime.hh"
#include "GHOST_SystemIOS.hh"
#include "GHOST_WindowIOS.hh"
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <UIKit/UIPencilInteraction.h>
#import <objc/runtime.h>
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

/** Blender's main window, which shows the open document over the launcher. */
extern UIWindow *document_window;

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

@interface GHOSTUITapGestureRecognizer : UITapGestureRecognizer

- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window;

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
@interface GHOSTUIHoverGestureRecognizer : UIHoverGestureRecognizer
- (CGPoint)getScaledTouchPoint:(GHOST_WindowIOS *)window;
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

/* GHOSTUIWindow interface. */
#if defined(APPLE_TARGET_VISIONOS)
@interface GHOSTUIWindow : UIWindow <UIGestureRecognizerDelegate, GHOST_IOSInputWindow>
#else
@interface GHOSTUIWindow
    : UIWindow <UIGestureRecognizerDelegate, UIPencilInteractionDelegate, GHOST_IOSInputWindow>
#endif
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
#if !defined(APPLE_TARGET_VISIONOS)
  UIPencilInteraction *pencil_interaction;
  UIScreenEdgePanGestureRecognizer *edge_swipe_left;
  UIScreenEdgePanGestureRecognizer *edge_swipe_right;
#endif
  // GHOSTUILongPressGestureRecognizer *long_press_gesture_recognizer;

#if !defined(APPLE_TARGET_VISIONOS)
  /* A mouse or trackpad: the pointer, its buttons, scrolling and trackpad gestures. */
  UIHoverGestureRecognizer *pointer_hover_recognizer;
  UIPanGestureRecognizer *trackpad_scroll_recognizer;
  UIPanGestureRecognizer *wheel_scroll_recognizer;
  UIPinchGestureRecognizer *trackpad_pinch_recognizer;
  UIRotationGestureRecognizer *trackpad_rotation_recognizer;
  UITouch *pointer_touch;
  UIEventButtonMask pointer_buttons;
  CGPoint scroll_translation;
#endif

  /* Data from the Apple pencil */
  UITouch *current_pencil_touch;
  GHOST_TabletData tablet_data;
  bool pencil_button_active;
  bool pencil_navigation_pending;
  CGPoint pencil_navigation_start;
  bool last_tap_with_pencil;
  bool finger_tool_enabled;
  UIView<GHOSTIOSPencilMenu> *pencil_context_menu;

  CADisplayLink *orbit_display_link;
  CGPoint orbit_velocity;
  CGPoint orbit_location;
  CGPoint orbit_remainder;
  CFTimeInterval orbit_timestamp;
  uint64_t orbit_gesture_id;

  /* Keyboard handling. */
  /** Text is edited by Blender from hardware key events, not in #text_field. */
  bool hardware_text_edit;
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
- (void)setPencilContextMenu:(UIView<GHOSTIOSPencilMenu> *)menu;
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
