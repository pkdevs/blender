/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#import <UIKit/UIKit.h>

#include "GHOST_IOSNativeInterface.hh"
#include "wm_apple_ios_alerts.hh"

@class WMIOSDialogRequest;

/* The sheet for dialogs with options, drawn in SwiftUI
 * (`source/blender/windowmanager/apple_ios/swift/DialogSheet.swift`). */
@interface UIViewController (WMIOSSwiftDialogSheet)
- (instancetype)initWithTitle:(NSString *)title
                      message:(NSString *)message
                      toggles:(NSArray<NSString *> *)toggles
                     selected:(NSArray<NSNumber *> *)selected
                      actions:(NSArray<NSString *> *)actions
                        roles:(NSArray<NSNumber *> *)roles
                    preferred:(NSInteger)preferred
                       choose:(void (^)(NSInteger action, uint64_t toggles))choose;
@end
static NSMutableArray<WMIOSDialogRequest *> *dialog_queue;
static NSTimer *dialog_timer;

@interface WMIOSDialogRequest : NSObject <UIAdaptivePresentationControllerDelegate>
{
 @public
  wmIOSDialog model;
  std::shared_ptr<std::atomic<bool>> alive;
}
@property(nonatomic) uint64_t identifier;
@property(nonatomic) wmIOSDialogCompletion completion;
@property(nonatomic, retain) UIWindow *window;
@property(nonatomic, retain) UIViewController *controller;
@property(nonatomic) BOOL finished;
/* The options chosen in the sheet, one bit per toggle. */
@property(nonatomic) uint64_t chosenToggles;
- (void)finish:(int)action;
- (void)presentFrom:(UIViewController *)presenter;
@end

static NSString *dialogString(const std::string &text)
{
  return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

static void dialogPump()
{
  WMIOSDialogRequest *request = dialog_queue.firstObject;
  if (!request) {
    [dialog_timer invalidate];
    dialog_timer = nil;
    return;
  }
  if (!request->alive->load()) {
    [request finish:-1];
    return;
  }
  if (request.controller || request.finished) {
    return;
  }
  UIViewController *presenter = request.window.rootViewController;
  while (presenter.presentedViewController) {
    if (presenter.isBeingDismissed || presenter.presentedViewController.isBeingDismissed ||
        [presenter.presentedViewController isKindOfClass:UIAlertController.class])
    {
      return;
    }
    presenter = presenter.presentedViewController;
  }
  if (!presenter.view.window || presenter.isBeingPresented || presenter.isBeingDismissed) {
    return;
  }
  [request presentFrom:presenter];
}

@implementation WMIOSDialogRequest
- (void)dealloc
{
  [_controller release];
  [_window release];
  [super dealloc];
}

- (void)finish:(int)action
{
  if (self.finished) {
    return;
  }
  self.finished = YES;
  const uint64_t toggles = self.chosenToggles;
  /* Resolve only after dismissal so the next action can present a file picker. */
  void (^complete)(void) = ^{
    wmIOSDialogCompletion callback = self.completion;
    self.completion = nullptr;
    self.controller = nil;
    [dialog_queue removeObject:self];
    if (callback) {
      callback(self.identifier, action, toggles);
    }
    dialogPump();
  };
  if (self.controller.presentingViewController) {
    [self.controller dismissViewControllerAnimated:YES completion:complete];
  }
  else {
    [[self retain] autorelease];
    complete();
  }
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)controller
{
  [self finish:-1];
}

- (void)presentFrom:(UIViewController *)presenter
{
  if (model.toggles.empty()) {
    UIAlertController *alert = [UIAlertController
        alertControllerWithTitle:dialogString(model.title)
                         message:dialogString(model.message)
                  preferredStyle:UIAlertControllerStyleAlert];
    alert.view.accessibilityIdentifier = @"blender.nativeDialog";
    for (size_t i = 0; i < model.actions.size(); i++) {
      const wmIOSDialogAction &item = model.actions[i];
      UIAlertActionStyle style = item.role == wmIOSDialogRole::Cancel ?
                                     UIAlertActionStyleCancel :
                                 item.role == wmIOSDialogRole::Destructive ?
                                     UIAlertActionStyleDestructive :
                                     UIAlertActionStyleDefault;
      UIAlertAction *action = [UIAlertAction actionWithTitle:dialogString(item.title)
                                                       style:style
                                                     handler:^(UIAlertAction *) {
                                                       [self finish:int(i)];
                                                     }];
      [alert addAction:action];
      if (int(i) == model.preferred_action) {
        alert.preferredAction = action;
      }
    }
    self.controller = alert;
  }
  else {
    Class sheet_class = NSClassFromString(@"WMIOSSwiftDialogSheet");
    NSCAssert(sheet_class, @"The SwiftUI dialog sheet is missing");
    NSMutableArray *toggles = [NSMutableArray array], *selected = [NSMutableArray array];
    for (const wmIOSDialogToggle &option : model.toggles) {
      [toggles addObject:dialogString(option.title)];
      [selected addObject:@(option.selected)];
    }
    NSMutableArray *actions = [NSMutableArray array], *roles = [NSMutableArray array];
    for (const wmIOSDialogAction &action : model.actions) {
      [actions addObject:dialogString(action.title)];
      [roles addObject:@(action.role == wmIOSDialogRole::Cancel      ? 1 :
                         action.role == wmIOSDialogRole::Destructive ? 2 :
                                                                       0)];
    }
    __block WMIOSDialogRequest *owner = self;
    UIViewController *sheet = [[[sheet_class alloc]
        initWithTitle:dialogString(model.title)
              message:dialogString(model.message)
              toggles:toggles
             selected:selected
              actions:actions
                roles:roles
            preferred:model.preferred_action
               choose:^(NSInteger action, uint64_t chosen) {
                 owner.chosenToggles = chosen;
                 [owner finish:int(action)];
               }] autorelease];
    sheet.modalPresentationStyle = UIModalPresentationFormSheet;
    sheet.preferredContentSize = CGSizeMake(520, 600);
    self.controller = sheet;
  }
  [presenter presentViewController:self.controller animated:YES completion:nil];
  if (![self.controller isKindOfClass:UIAlertController.class]) {
    self.controller.presentationController.delegate = self;
  }
}
@end

bool wm_apple_ios_dialog_begin(void *window,
                               uint64_t identifier,
                               const wmIOSDialog &dialog,
                               wmIOSDialogCompletion completion)
{
  if (!window || dialog.actions.empty() || dialog.toggles.size() > 64) {
    return false;
  }
  GHOST_WindowIOS *ghost = static_cast<GHOST_WindowIOS *>(window);
  UIWindow *native_window = GHOST_ios_window_native(ghost);
  auto token = GHOST_ios_window_alive(ghost);
  wmIOSDialog owned = dialog;
  dispatch_async(dispatch_get_main_queue(), ^{
    if (!token->load()) {
      if (completion) {
        completion(identifier, -1, 0);
      }
      return;
    }
    if (!dialog_queue) {
      dialog_queue = [[NSMutableArray alloc] init];
    }
    /* Reports can arrive through both the operator popup and global report log. */
    if (!completion) {
      for (WMIOSDialogRequest *request in dialog_queue) {
        if (!request.completion && request.window == native_window &&
            request->model.title == owned.title && request->model.message == owned.message)
        {
          return;
        }
      }
    }
    WMIOSDialogRequest *request = [[[WMIOSDialogRequest alloc] init] autorelease];
    request->model = owned;
    request->alive = token;
    request.identifier = identifier;
    request.completion = completion;
    request.window = native_window;
    [dialog_queue addObject:request];
    if (!dialog_timer) {
      dialog_timer = [NSTimer timerWithTimeInterval:0.05
                                            repeats:YES
                                              block:^(NSTimer *) {
                                                dialogPump();
                                              }];
      [NSRunLoop.mainRunLoop addTimer:dialog_timer forMode:NSRunLoopCommonModes];
    }
    dialogPump();
  });
  return true;
}

void wm_apple_ios_dialog_cancel(uint64_t identifier)
{
  dispatch_async(dispatch_get_main_queue(), ^{
    for (WMIOSDialogRequest *request in [[dialog_queue copy] autorelease]) {
      if (request.identifier == identifier) {
        request.completion = nullptr;
        [request finish:-1];
        break;
      }
    }
  });
}
